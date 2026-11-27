// SPDX-License-Identifier: GPL-2.0
/*
 * NZEUS R36S (HiChip E3100) battery and charger, READ-ONLY.
 *
 * The factory bootloader configures the SAR ADC (channels 0, 1, 5) and leaves
 * it free-running; this driver only READS the latest sample bytes, exactly as
 * the bootloader's own "saradc" read routine does
 * (linux-port/boot-analysis/bootloader-disasm.txt lines 42892-42968):
 *   sample byte of channel ch at ADC base + 0xd0 + (ch < 4 ? ch : ch + 2)
 *   stock level = min(255, 241 * raw[ch] / raw[0])   (channel 0 = reference)
 * Measured on this console (linux-port/battery/README.md): ch1 battery level
 * about 128 empty .. 175 full (the bootloader treats <= 127 as low), ch5
 * charger 0 unplugged, about 138 plugged in.
 *
 * The DT node maps only the 8 sample bytes (0x188184d0), never the whole ADC
 * window it shares with the PWM. The ADC clock must stay on
 * (clk_ignore_unused). Nothing is ever written to the ADC.
 *
 * Optional power-gpios (GPIO L14, active low), measured on this console
 * (linux-port/power/README.md): the power button pulls L14 low while pressed.
 * L14 only SENSES the button: driving it low from the SoC does nothing
 * (hardware 2026-10-10: 15 s low, console stayed on). It always stays an INPUT:
 *   - polled every 50 ms as KEY_POWER (input device "r36s-power-button").
 *     Own delayed work, NOT the input poller: the input poller only runs
 *     while a program has the event device open, and hold-to-off must work
 *     with no program running;
 *   - released before 3 s: enter suspend-to-idle when sleep_on_short_press
 *     is enabled. A falling-edge GPIO IRQ wakes it on the next press;
 *   - held for 3 s: orderly power-off (forced if no /sbin/poweroff exists).
 *
 * Optional poweroff-gpios (GPIO L01, active low) = the stock device tree's
 * "power-pin" (panel node, next to power-button = 14). Hardware 2026-10-10
 * (power/pin-drive.c): driving L01 low switches the console off at once.
 * It is requested AS-IS, so probing never touches it (it stays the input the
 * bootloader left); only the power-off handler drives it low. If the power is
 * still on after 2 s, the handler lets go again and reports it.
 *
 * Optional voldown-gpios (GPIO L12, active low) = the Vol- button. L12 is ALSO
 * the debug serial line (uart0 TX, the boot log the Uno logger reads), and the
 * pin controller is strict: while uart0 owns the pin it cannot be a GPIO. So
 * the pin is never taken at probe. sysfs "voldown" (root only):
 *   echo key  > voldown   take L12 as Vol- (only works after uart0 is unbound:
 *                         /sys/bus/platform/drivers/of_serial/unbind);
 *                         polled with the power button, reported as KEY_VOLUMEDOWN
 *   echo uart > voldown   give the pin back (then bind uart0 again)
 * nzeus-voldown (Debian) does both steps; the boot log stays on the UART until
 * the system is up.
 *
 * Optional nzeus,restart-watchdog = <0x18818500> (the stock DT's
 * watchdog@18818500): RESTART. Linux had no restart handler for this SoC, so
 * "reboot" just stopped. HiChip's own kernel restarts like this (SDK
 * arch/mips/hc16xx/board.c, board_restart()): watchdog count = 0xfffffffa,
 * then conf = 0x26 (clkdiv 2 = 27 MHz/128, en, wdten = reset on overflow):
 * it overflows after 6 ticks (~28 us) and resets the chip. Probing only maps
 * the 8 bytes; nothing is written until a restart is asked for.
 */
#include <linux/atomic.h>
#include <linux/delay.h>
#include <linux/devm-helpers.h>
#include <linux/gpio/consumer.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/reboot.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/property.h>
#include <linux/suspend.h>
#include <linux/workqueue.h>

#define LEVEL_EMPTY	128	/* measured: the console died at 128-129 */
#define LEVEL_FULL	175
#define LEVEL_LOW	127	/* bootloader's low-battery threshold */
#define CHARGER_ON	64	/* NZEUS stock-side threshold */
#define BUTTON_POLL_MS	50
#define HOLD_TO_OFF_MS	3000
#define POWER_OFF_LOW_MS	2000	/* L01 low switched off at once on hardware */

static bool sleep_on_short_press;
module_param(sleep_on_short_press, bool, 0644);
MODULE_PARM_DESC(sleep_on_short_press, "Enter suspend-to-idle on a short power press");

struct r36s_power {
	void __iomem *samples;		/* 8 bytes at ADC base + 0xd0 */
	struct power_supply *battery, *charger;
	struct gpio_desc *power;		/* L14 button, NULL if not described */
	struct gpio_desc *poweroff;		/* L01 power-pin, NULL if not described */
	int power_irq;
	struct input_dev *button;
	struct delayed_work poll;
	struct work_struct sleep;
	struct workqueue_struct *sleep_queue;
	atomic_t sleep_in_progress;
	unsigned int held_ms;
	bool off_requested;
	bool wake_press;
	struct gpio_desc *voldown;		/* L12 Vol-, only while taken from uart0 */
	void __iomem *wdt;			/* watchdog count + conf, NULL = no restart */
	struct mutex voldown_lock;		/* voldown vs. the poll work */
};

/* Stock-equivalent level of channel ch, or -EIO if the reference reads 0. */
static int r36s_level(struct r36s_power *p, unsigned int ch)
{
	unsigned int ref = readb(p->samples + 0);
	unsigned int raw = readb(p->samples + (ch < 4 ? ch : ch + 2));

	if (!ref)
		return -EIO;
	return min(241 * raw / ref, 255U);
}

static int r36s_charger_online(struct r36s_power *p)
{
	int level = r36s_level(p, 5);

	return level < 0 ? level : level >= CHARGER_ON;
}

static int r36s_battery_get(struct power_supply *psy,
			    enum power_supply_property psp,
			    union power_supply_propval *val)
{
	struct r36s_power *p = power_supply_get_drvdata(psy);
	int level = r36s_level(p, 1), online;

	if (level < 0)
		return level;
	switch (psp) {
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		/* Rough straight line between the measured empty and full. */
		val->intval = clamp((level - LEVEL_EMPTY) * 100 /
				    (LEVEL_FULL - LEVEL_EMPTY), 0, 100);
		break;
	case POWER_SUPPLY_PROP_CAPACITY_LEVEL:
		val->intval = level <= LEVEL_LOW ? POWER_SUPPLY_CAPACITY_LEVEL_CRITICAL :
			      level < LEVEL_EMPTY + 10 ? POWER_SUPPLY_CAPACITY_LEVEL_LOW :
			      level >= LEVEL_FULL ? POWER_SUPPLY_CAPACITY_LEVEL_FULL :
			      POWER_SUPPLY_CAPACITY_LEVEL_NORMAL;
		break;
	case POWER_SUPPLY_PROP_STATUS:
		online = r36s_charger_online(p);
		if (online < 0)
			return online;
		val->intval = !online ? POWER_SUPPLY_STATUS_DISCHARGING :
			      level >= LEVEL_FULL ? POWER_SUPPLY_STATUS_FULL :
			      POWER_SUPPLY_STATUS_CHARGING;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int r36s_charger_get(struct power_supply *psy,
			    enum power_supply_property psp,
			    union power_supply_propval *val)
{
	struct r36s_power *p = power_supply_get_drvdata(psy);
	int online;

	if (psp != POWER_SUPPLY_PROP_ONLINE)
		return -EINVAL;
	online = r36s_charger_online(p);
	if (online < 0)
		return online;
	val->intval = online;
	return 0;
}

static const enum power_supply_property r36s_battery_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CAPACITY_LEVEL,
};

static const enum power_supply_property r36s_charger_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
};

static const struct power_supply_desc r36s_battery_desc = {
	.name = "battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = r36s_battery_props,
	.num_properties = ARRAY_SIZE(r36s_battery_props),
	.get_property = r36s_battery_get,
};

static const struct power_supply_desc r36s_charger_desc = {
	.name = "charger",
	.type = POWER_SUPPLY_TYPE_MAINS,
	.properties = r36s_charger_props,
	.num_properties = ARRAY_SIZE(r36s_charger_props),
	.get_property = r36s_charger_get,
};

/* Timers stop in s2idle: wake must come from the GPIO interrupt, not polling. */
static irqreturn_t r36s_power_irq(int irq, void *data)
{
	struct r36s_power *p = data;

	if (atomic_read(&p->sleep_in_progress) ||
	    pm_suspend_target_state == PM_SUSPEND_TO_IDLE) {
		WRITE_ONCE(p->wake_press, true);
		pm_system_wakeup();
	}
	return IRQ_HANDLED;
}

static void r36s_sleep(struct work_struct *work)
{
	struct r36s_power *p = container_of(work, struct r36s_power, sleep);
	int ret;

	if (p->power_irq > 0 && !gpiod_get_value(p->power)) {
		ret = pm_suspend(PM_SUSPEND_TO_IDLE);
		if (ret && ret != -EBUSY)
			dev_warn(p->button->dev.parent, "suspend-to-idle failed: %d\n", ret);
	}
	atomic_set(&p->sleep_in_progress, 0);
}

static void r36s_button_poll(struct work_struct *work)
{
	struct r36s_power *p = container_of(work, struct r36s_power, poll.work);
	struct input_dev *input = p->button;
	int pressed = gpiod_get_value(p->power);

	schedule_delayed_work(&p->poll, msecs_to_jiffies(BUTTON_POLL_MS));
	mutex_lock(&p->voldown_lock);
	if (p->voldown) {
		int down = gpiod_get_value(p->voldown);

		if (down >= 0)
			input_report_key(input, KEY_VOLUMEDOWN, down);
	}
	mutex_unlock(&p->voldown_lock);
	if (pressed < 0) {
		input_sync(input);
		return;
	}
	input_report_key(input, KEY_POWER, pressed);
	input_sync(input);
	if (!pressed) {
		if (p->held_ms && !p->off_requested && !READ_ONCE(p->wake_press) &&
		    READ_ONCE(sleep_on_short_press) &&
		    !atomic_cmpxchg(&p->sleep_in_progress, 0, 1))
			queue_work(p->sleep_queue, &p->sleep);
		p->held_ms = 0;
		WRITE_ONCE(p->wake_press, false);
		return;
	}

	/* Suppress another suspend when the wake press is released. */
	if (atomic_read(&p->sleep_in_progress))
		WRITE_ONCE(p->wake_press, true);
	if (pm_suspend_target_state == PM_SUSPEND_TO_IDLE) {
		WRITE_ONCE(p->wake_press, true);
		pm_system_wakeup();
	}
	p->held_ms += BUTTON_POLL_MS;
	if (p->held_ms >= HOLD_TO_OFF_MS && !p->off_requested) {
		p->off_requested = true;
		dev_emerg(input->dev.parent, "power button held 3 s: powering off\n");
		orderly_poweroff(true);
	}
}

static int r36s_power_off(struct sys_off_data *data)
{
	struct r36s_power *p = data->cb_data;

	/* Drive the power-pin L01 low (active low: logical 1 = low); the power goes. */
	pr_emerg("nzeus-r36s-power: driving L01 (power-pin) low to switch off\n");
	gpiod_direction_output(p->poweroff, 1);
	mdelay(POWER_OFF_LOW_MS);
	gpiod_direction_input(p->poweroff);
	pr_emerg("nzeus-r36s-power: power still on %d s after driving L01 low\n",
		 POWER_OFF_LOW_MS / 1000);
	return NOTIFY_DONE;
}

static int r36s_restart(struct sys_off_data *data)
{
	struct r36s_power *p = data->cb_data;

	pr_emerg("nzeus-r36s-power: restarting through the watchdog\n");
	writel(0xfffffffa, p->wdt);		/* 6 ticks before the overflow */
	writel(0x26, p->wdt + 4);		/* clkdiv 2, enable, reset on overflow */
	mdelay(1000);
	pr_emerg("nzeus-r36s-power: the watchdog did not restart the chip\n");
	return NOTIFY_DONE;
}

static int r36s_restart_init(struct device *dev, struct r36s_power *p)
{
	u32 addr;

	if (device_property_read_u32(dev, "nzeus,restart-watchdog", &addr))
		return 0;			/* not described: no restart handler */
	/* Not devm_ioremap_resource: only these 8 bytes, never claimed by others. */
	p->wdt = devm_ioremap(dev, addr, 8);
	if (!p->wdt)
		return -ENOMEM;
	return devm_register_sys_off_handler(dev, SYS_OFF_MODE_RESTART,
					     SYS_OFF_PRIO_DEFAULT, r36s_restart, p);
}

static void r36s_destroy_sleep_queue(void *data)
{
	destroy_workqueue(data);
}

static int r36s_power_button_init(struct device *dev, struct r36s_power *p)
{
	int ret;

	/* AS-IS: probing must not touch the power-pin; only power-off drives it. */
	p->poweroff = devm_gpiod_get_optional(dev, "poweroff", GPIOD_ASIS);
	if (IS_ERR(p->poweroff))
		return dev_err_probe(dev, PTR_ERR(p->poweroff), "poweroff-gpios\n");
	if (p->poweroff) {
		ret = devm_register_sys_off_handler(dev, SYS_OFF_MODE_POWER_OFF,
						    SYS_OFF_PRIO_DEFAULT, r36s_power_off, p);
		if (ret)
			return ret;
	}
	p->power = devm_gpiod_get_optional(dev, "power", GPIOD_IN);
	if (IS_ERR(p->power))
		return dev_err_probe(dev, PTR_ERR(p->power), "power-gpios\n");
	if (!p->power)
		return 0;
	p->button = devm_input_allocate_device(dev);
	if (!p->button)
		return -ENOMEM;
	p->button->name = "r36s-power-button";
	p->button->phys = "nzeus-r36s-power/input0";
	p->button->id.bustype = BUS_HOST;
	input_set_capability(p->button, EV_KEY, KEY_POWER);
	input_set_capability(p->button, EV_KEY, KEY_VOLUMEDOWN);	/* only while "voldown" = key */
	ret = input_register_device(p->button);
	if (ret)
		return ret;
	p->power_irq = gpiod_to_irq(p->power);
	if (p->power_irq > 0) {
		ret = devm_request_irq(dev, p->power_irq, r36s_power_irq,
				       IRQF_TRIGGER_FALLING | IRQF_NO_SUSPEND,
				       "r36s-power-wake", p);
		if (ret) {
			dev_warn(dev, "no power wake IRQ: %d; sleep disabled\n", ret);
			p->power_irq = 0;
		}
	}
	/* Suspend callbacks can flush the system workqueue, so use our own. */
	p->sleep_queue = alloc_ordered_workqueue("r36s-sleep", 0);
	if (!p->sleep_queue)
		return -ENOMEM;
	ret = devm_add_action_or_reset(dev, r36s_destroy_sleep_queue, p->sleep_queue);
	if (ret)
		return ret;
	ret = devm_work_autocancel(dev, &p->sleep, r36s_sleep);
	if (ret)
		return ret;
	/* Cancelled (and waited for) on unbind, before the input device goes. */
	ret = devm_delayed_work_autocancel(dev, &p->poll, r36s_button_poll);
	if (ret)
		return ret;
	schedule_delayed_work(&p->poll, msecs_to_jiffies(BUTTON_POLL_MS));
	return 0;
}

/* Raw stock-scale levels, for calibrating the percentage later. */
static ssize_t stock_levels_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct r36s_power *p = dev_get_drvdata(dev);

	return sysfs_emit(buf, "ref=%u battery=%d charger=%d\n",
			  readb(p->samples), r36s_level(p, 1), r36s_level(p, 5));
}
static DEVICE_ATTR_RO(stock_levels);

/* "key" while L12 is taken as Vol-, else "uart" (the pin is left to uart0). */
static ssize_t voldown_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct r36s_power *p = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s\n", READ_ONCE(p->voldown) ? "key" : "uart");
}

static ssize_t voldown_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct r36s_power *p = dev_get_drvdata(dev);
	struct gpio_desc *d;
	int ret = count;

	if (!p->button)
		return -ENODEV;		/* no power button = no poll work to read it */
	mutex_lock(&p->voldown_lock);
	if (sysfs_streq(buf, "key")) {
		if (!p->voldown) {
			/* Fails (-EBUSY) while uart0 still owns L12: unbind it first. */
			d = gpiod_get_optional(dev, "voldown", GPIOD_IN);
			if (IS_ERR(d))
				ret = PTR_ERR(d);
			else if (!d)
				ret = -ENODEV;	/* no voldown-gpios in the device tree */
			else
				p->voldown = d;
		}
	} else if (sysfs_streq(buf, "uart")) {
		if (p->voldown) {
			input_report_key(p->button, KEY_VOLUMEDOWN, 0);
			input_sync(p->button);
			gpiod_put(p->voldown);
			p->voldown = NULL;
		}
	} else {
		ret = -EINVAL;
	}
	mutex_unlock(&p->voldown_lock);
	if (ret > 0)
		dev_info(dev, "L12: %s\n", p->voldown ? "Vol- button" : "left to the UART");
	return ret;
}
static DEVICE_ATTR_RW(voldown);

static int r36s_power_probe(struct platform_device *pdev)
{
	struct power_supply_config cfg = {};
	struct device *dev = &pdev->dev;
	struct resource *res;
	struct r36s_power *p;
	int level, ret;

	p = devm_kzalloc(dev, sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res || resource_size(res) != 8 || res->start != 0x188184d0)
		return dev_err_probe(dev, -EINVAL, "expected the 8 ADC sample bytes at 0x188184d0\n");
	/* Not devm_ioremap_resource: the ADC window is shared with the PWM. */
	p->samples = devm_ioremap(dev, res->start, 8);
	if (!p->samples)
		return -ENOMEM;
	platform_set_drvdata(pdev, p);
	mutex_init(&p->voldown_lock);
	atomic_set(&p->sleep_in_progress, 0);

	level = r36s_level(p, 1);
	if (level < 0)
		return dev_err_probe(dev, level, "ADC reference reads 0: not configured by the bootloader?\n");

	cfg.drv_data = p;
	cfg.fwnode = dev_fwnode(dev);
	p->charger = devm_power_supply_register(dev, &r36s_charger_desc, &cfg);
	if (IS_ERR(p->charger))
		return PTR_ERR(p->charger);
	p->battery = devm_power_supply_register(dev, &r36s_battery_desc, &cfg);
	if (IS_ERR(p->battery))
		return PTR_ERR(p->battery);
	ret = device_create_file(dev, &dev_attr_stock_levels);
	if (ret)
		return ret;
	ret = r36s_restart_init(dev, p);
	if (!ret)
		ret = r36s_power_button_init(dev, p);
	if (ret) {
		device_remove_file(dev, &dev_attr_stock_levels);
		return ret;
	}
	ret = device_create_file(dev, &dev_attr_voldown);
	if (ret) {
		device_remove_file(dev, &dev_attr_stock_levels);
		return ret;
	}
	dev_info(dev, "battery level %d (stock scale), charger level %d; power button %s\n",
		 level, r36s_level(p, 5), p->power ? "on L14, hold 3 s = power off" : "not described");
	return 0;
}

static void r36s_power_remove(struct platform_device *pdev)
{
	struct r36s_power *p = platform_get_drvdata(pdev);

	device_remove_file(&pdev->dev, &dev_attr_voldown);
	device_remove_file(&pdev->dev, &dev_attr_stock_levels);
	/* The poll work is cancelled by devm after this; stop it reading L12 first. */
	mutex_lock(&p->voldown_lock);
	if (p->voldown) {
		gpiod_put(p->voldown);
		p->voldown = NULL;
	}
	mutex_unlock(&p->voldown_lock);
}

static const struct of_device_id r36s_power_of_match[] = {
	{ .compatible = "nzeus,r36s-power" },
	{ }
};
MODULE_DEVICE_TABLE(of, r36s_power_of_match);

static struct platform_driver r36s_power_driver = {
	.probe = r36s_power_probe,
	.remove = r36s_power_remove,
	.driver = {
		.name = "nzeus-r36s-power",
		.of_match_table = r36s_power_of_match,
	},
};
module_platform_driver(r36s_power_driver);

MODULE_DESCRIPTION("NZEUS R36S battery, charger and power button");
MODULE_LICENSE("GPL");
