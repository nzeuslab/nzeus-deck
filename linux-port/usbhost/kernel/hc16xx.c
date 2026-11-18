// SPDX-License-Identifier: GPL-2.0+
/*
 * HiChip HC16XX MUSB "glue layer"
 *
 * Copyright (C) 2026, Siarhei Volkau <lis8215@gmail.com>
 *
 * NZEUS changes (linux-port/usbhost/README.md), 2026-10-10:
 *  - per-controller platform data (the old one was a shared static struct)
 *  - the port mode comes from the device tree "dr_mode":
 *      "peripheral" (or missing)  gadget only, as before
 *      "host"                     host only (like the stock 4.4 kernel)
 *      "otg"                      starts as PERIPHERAL (no ID pin on the R36S)
 *                                 and offers a user-controllable USB role
 *                                 switch: /sys/class/usb_role/<dev>-role-switch/role
 *  - a role change RE-CREATES the MUSB core in the other port mode (unregister
 *    the musb-hdrc child, register a new one). Host mode is then exactly the
 *    MUSB core's well-trodden host-only path (session bit set by musb_start()
 *    when the root hub powers its port), and peripheral mode is exactly the
 *    boot-time gadget path, so the USB rescue gadget is not changed.
 *  - .set_mode never sleeps and never changes the mode: the legacy musb sysfs
 *    "mode" file calls it under a spinlock with interrupts off (the old code
 *    called phy_power_on(), which takes a mutex, from there).
 *  - the controller IRQ is switched on/off idempotently (the core's
 *    request_irq() enables it behind the glue's back, and host mode reaches
 *    musb_start() while the core is still being registered).
 *  - optional "force VBUS valid" in host mode through the PHY (off by default,
 *    nzeus_host_force_vbus sysfs knob; see phy-hichip-usb.c UPHY20).
 *  - a core whose start-up is deferred (-EPROBE_DEFER, or the core never
 *    reached .init) makes the glue probe return -EPROBE_DEFER, not a hard
 *    error, so it is probed again and the USB rescue gadget still appears
 *    (the original glue never waited for the core to bind).
 */

#include <linux/clk.h>
#include <linux/errno.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/usb/of.h>
#include <linux/usb/role.h>
#include <linux/usb/usb_phy_generic.h>

#include "musb_core.h"

#define USB_O_UTMI 0x00000380
#define USB_O_VBUS 0x00000381

#define UTMI_DISABLE_PHY      BIT(0)
#define UTMI_SUSPEND_PHY      BIT(1)
#define UTMI_DISABLE_OTG      BIT(7)
#define UTMI_DISABLE_OTG_BITS (UTMI_DISABLE_OTG | (0x3 << 2))
#define UTMI_PERIPHERAL_MODE  BIT(4)

/* phy_set_mode_ext() submode understood by phy-hichip-usb.c (NZEUS) */
#define HICHIP_USB_PHY_FORCE_VBUS_VALID	BIT(0)

struct hc16xx_glue {
	struct device		*dev;
	struct platform_device	*musb_pdev;	/* the MUSB core that exists now */
	struct musb		*musb;
	struct usb_role_switch	*role_sw;	/* only for dr_mode = "otg" */
	struct mutex		lock;		/* musb_pdev, mode, force_vbus */

	struct clk		*clk;
	struct reset_control	*reset;
	struct phy		*phy;		/* got once in probe (not per core) */
	struct resource		mem;
	int			irq;
	int			dma_irq;
	u8			mode;		/* MUSB_HOST or MUSB_PERIPHERAL */
	bool			mode_applied;	/* the core's own set_mode() call ran */
	bool			clk_on;		/* USB clock on: once, off only at glue remove */
	bool			force_vbus;	/* host only, see phy-hichip-usb.c */
	int			init_ret;	/* last .init result; 1 = .init not called */
};

static struct musb_fifo_cfg hc16xx_fifo_mode_cfg[] = {
	MUSB_EP_FIFO_SINGLE(1, FIFO_TX, 512),
	MUSB_EP_FIFO_SINGLE(1, FIFO_RX, 512),
	MUSB_EP_FIFO_SINGLE(2, FIFO_TX, 512),
	MUSB_EP_FIFO_SINGLE(2, FIFO_RX, 512),
	MUSB_EP_FIFO_SINGLE(3, FIFO_TX, 512),
	MUSB_EP_FIFO_SINGLE(3, FIFO_RX, 512),
	MUSB_EP_FIFO_SINGLE(4, FIFO_TX, 512),
	MUSB_EP_FIFO_SINGLE(4, FIFO_RX, 512),
	MUSB_EP_FIFO_SINGLE(5, FIFO_TX, 512),
	MUSB_EP_FIFO_SINGLE(5, FIFO_RX, 512),
};

static const struct musb_hdrc_config hc16xx_musb_config = {
	.fifo_cfg = hc16xx_fifo_mode_cfg,
	.fifo_cfg_size = ARRAY_SIZE(hc16xx_fifo_mode_cfg),
	.multipoint = 1,
	.dyn_fifo = 1,
	.num_eps = 7,
	.ram_bits = 12,
};

static const char *hc16xx_mode_name(u8 mode)
{
	return mode == MUSB_HOST ? "host" : "peripheral";
}

/*
 * Register part of the PHY power sequence (unchanged from the original
 * hc16xx_set_phy_power(), which took the mode from musb->is_host). Only
 * register writes and udelay(): safe in atomic context.
 */
static void hc16xx_utmi_power(struct musb *musb, u8 mode, bool on)
{
	u32 reg, bits = 0;

	if (mode != MUSB_HOST)
		bits = UTMI_PERIPHERAL_MODE;

	if (on) {
		reg = readl(musb->mregs + USB_O_UTMI);
		reg &= ~(UTMI_DISABLE_OTG | UTMI_PERIPHERAL_MODE);
		writel(reg, musb->mregs + USB_O_UTMI);

		reg = readl(musb->mregs + USB_O_UTMI);
		reg |= UTMI_DISABLE_PHY | UTMI_DISABLE_OTG_BITS;
		writel(reg, musb->mregs + USB_O_UTMI);
		udelay(900);
		reg = readl(musb->mregs + USB_O_UTMI);
		reg &= ~(UTMI_DISABLE_PHY | UTMI_PERIPHERAL_MODE);
		reg |= bits;
		writel(reg, musb->mregs + USB_O_UTMI);
	} else {
		reg = readl(musb->mregs + USB_O_UTMI);
		reg |= UTMI_DISABLE_PHY | UTMI_DISABLE_OTG_BITS;
		writel(reg, musb->mregs + USB_O_UTMI);

		reg = readl(musb->mregs + USB_O_UTMI);
		reg &= ~(UTMI_DISABLE_OTG | UTMI_PERIPHERAL_MODE);
		writel(reg, musb->mregs + USB_O_UTMI);
	}
}

/*
 * UTMI host/peripheral selection: the register sequence of the original
 * hc16xx_musb_set_mode() (OTG detection off, the PERIPHERAL bit selects the
 * side), followed by the PHY power-on register sequence. No sleeping calls.
 */
static void hc16xx_utmi_set_mode(struct musb *musb, u8 mode)
{
	u8 reg;

	if (mode == MUSB_HOST) {
		writeb(BIT(7), musb->mregs + USB_O_UTMI);
		udelay(1);

		reg = readb(musb->mregs + USB_O_UTMI);
		/* disable OTG function */
		reg |= UTMI_DISABLE_OTG_BITS;
		/* set as host mode */
		reg &= ~UTMI_PERIPHERAL_MODE;
		writeb(reg, musb->mregs + USB_O_UTMI);
		MUSB_HST_MODE(musb);
	} else {
		writeb(BIT(7) | UTMI_PERIPHERAL_MODE, musb->mregs + USB_O_UTMI);
		udelay(1);

		reg = readb(musb->mregs + USB_O_UTMI);
		/* disable OTG function */
		reg |= UTMI_DISABLE_OTG_BITS;
		/* set as peripheral mode */
		reg |= UTMI_PERIPHERAL_MODE;
		writeb(reg, musb->mregs + USB_O_UTMI);
		MUSB_DEV_MODE(musb);
	}

	hc16xx_utmi_power(musb, mode, true);
}

/*
 * Called (1) once by musb_init_controller() with the port mode this core was
 * created for, and (2) by the legacy musb sysfs "mode" file, under musb->lock
 * with interrupts off. So: no sleeping and never a mode change here. Roles
 * are changed by re-creating the core (hc16xx_role_set()).
 */
static int hc16xx_musb_set_mode(struct musb *musb, u8 mode)
{
	struct device *dev = musb->controller->parent;
	struct hc16xx_glue *glue = dev_get_drvdata(dev);

	if (mode != glue->mode) {
		dev_warn_ratelimited(dev,
			"mode %u refused: this core is %s; use the usb_role switch\n",
			mode, hc16xx_mode_name(glue->mode));
		return -EBUSY;
	}
	if (glue->mode_applied)
		return 0;

	hc16xx_utmi_set_mode(musb, mode);
	glue->mode_applied = true;
	return 0;
}

static irqreturn_t generic_interrupt(int irq, void *__hci)
{
	unsigned long flags;
	irqreturn_t retval = IRQ_NONE;
	struct musb *musb = __hci;

	spin_lock_irqsave(&musb->lock, flags);
	musb->int_usb = musb_clearb(musb->mregs, MUSB_INTRUSB);
	musb->int_rx = musb_clearw(musb->mregs, MUSB_INTRRX);
	musb->int_tx = musb_clearw(musb->mregs, MUSB_INTRTX);

	if ((musb->int_usb & MUSB_INTR_RESET) && !is_host_active(musb)) {
		/* ep0 FADDR must be 0 when (re)entering peripheral mode */
		musb_ep_select(musb->mregs, 0);
		musb_writeb(musb->mregs, MUSB_FADDR, 0);
	}

	if (musb->int_usb || musb->int_tx || musb->int_rx)
		retval = musb_interrupt(musb);

	spin_unlock_irqrestore(&musb->lock, flags);

	return retval;
}

static int hc16xx_musb_init(struct musb *musb)
{
	struct device *dev = musb->controller->parent;
	struct hc16xx_glue *glue = dev_get_drvdata(dev);
	u8 mode = glue->mode;
	int submode = 0;
	int err;

	/*
	 * The clock is switched on once and stays on across role switches (see
	 * hc16xx_musb_exit()); HiChip's own driver (SDK linux-drivers hcusb.c)
	 * never gates it either.
	 */
	if (!glue->clk_on) {
		err = clk_prepare_enable(glue->clk);
		if (err < 0) {
			dev_err(dev, "Failed to enable clock\n");
			glue->init_ret = err;
			return err;
		}
		glue->clk_on = true;
	}

	err = reset_control_reset(glue->reset);
	if (err < 0) {
		dev_err(dev, "Failed to issue reset\n");
		goto err_clk_disable;
	}

	glue->musb = musb;
	musb->isr = generic_interrupt;
	musb->phy = glue->phy;

	err = phy_init(musb->phy);
	if (err) {
		dev_err(dev, "Failed to init PHY\n");
		goto err_clk_disable;
	}

	/* Same UTMI register sequence as the original init-time set_mode(). */
	hc16xx_utmi_set_mode(musb, mode);
	if (mode == MUSB_HOST && glue->force_vbus)
		submode = HICHIP_USB_PHY_FORCE_VBUS_VALID;
	phy_set_mode_ext(musb->phy, mode == MUSB_HOST ? PHY_MODE_USB_HOST :
			 PHY_MODE_USB_DEVICE, submode);
	err = phy_power_on(musb->phy);
	if (err) {
		dev_err(dev, "Failed to power on PHY\n");
		goto err_phy_power_down;
	}

	glue->init_ret = 0;
	return 0;

err_phy_power_down:
	hc16xx_utmi_power(musb, mode, false);
	phy_set_mode_ext(musb->phy, PHY_MODE_USB_DEVICE, 0);
	phy_exit(musb->phy);
err_clk_disable:
	/* clock left on: the core still touches registers while it backs out (see exit) */
	glue->musb = NULL;
	glue->init_ret = err;	/* hc16xx_add_core() tells -EPROBE_DEFER apart */
	return err;
}

static int hc16xx_musb_exit(struct musb *musb)
{
	struct device *dev = musb->controller->parent;
	struct hc16xx_glue *glue = dev_get_drvdata(dev);

	hc16xx_utmi_power(musb, glue->mode, false);
	phy_power_off(musb->phy);
	/* undo a forced VBUS-valid (no register access if it was never forced) */
	phy_set_mode_ext(musb->phy, PHY_MODE_USB_DEVICE, 0);
	phy_exit(musb->phy);
	/*
	 * NOT clk_disable_unprepare(): musb_remove() calls this .exit and THEN
	 * pm_runtime_put_sync() -> musb_runtime_suspend() -> musb_save_context(),
	 * which reads the core's registers. On this SoC a read of a clock-gated
	 * block hangs the bus: hardware 2026-10-10, the host -> device switch
	 * froze the console 2/2 times at exactly this point. The clock goes off
	 * only in hc16xx_glue_remove(), after the core is completely gone.
	 */
	glue->musb = NULL;

	return 0;
}

/*
 * Idempotent IRQ on/off. The real state is read from the IRQ core: the MUSB
 * core's request_irq() enables the line by itself, and in host mode
 * musb_start() (-> .enable) already runs while the core is being registered.
 * disable_irq_nosync(): .disable is called with musb->lock held (gadget stop),
 * where waiting for a handler that takes the same lock must not happen.
 */
static void hc16xx_irq_on(struct hc16xx_glue *glue)
{
	if (irqd_irq_disabled(irq_get_irq_data(glue->irq)))
		enable_irq(glue->irq);
}

static void hc16xx_irq_off(struct hc16xx_glue *glue)
{
	if (!irqd_irq_disabled(irq_get_irq_data(glue->irq)))
		disable_irq_nosync(glue->irq);
}

static void hc16xx_musb_enable(struct musb *musb)
{
	struct hc16xx_glue *glue = dev_get_drvdata(musb->controller->parent);

	hc16xx_irq_on(glue);
}

static void hc16xx_musb_disable(struct musb *musb)
{
	struct hc16xx_glue *glue = dev_get_drvdata(musb->controller->parent);

	hc16xx_irq_off(glue);
}

static const struct musb_platform_ops hc16xx_musb_ops = {
	.init		= hc16xx_musb_init,
	.exit		= hc16xx_musb_exit,

	.enable		= hc16xx_musb_enable,
	.disable	= hc16xx_musb_disable,

	.set_mode	= hc16xx_musb_set_mode,
	// // .try_idle	= hc16xx_musb_try_idle,
	//.recover	= hc_musb_recover,

	/*
	 * No .set_vbus: the R36S has no VBUS switch GPIO (USB-HOST.md); the
	 * stock hc16xx_musb_set_vbus() is not available as source.
	 */
#if defined(CONFIG_USB_INVENTRA_DMA) && 0 /* TODO: FIFO only so far */
	.quirks		= MUSB_DMA_INVENTRA | MUSB_INDEXED_EP,
	.dma_init	= musbhs_dma_controller_create,
	.dma_exit	= musbhs_dma_controller_destroy,
#else
	.quirks		= MUSB_INDEXED_EP,
#endif
};

/* Create the MUSB core (musb-hdrc child) in @mode. glue->lock held or probe. */
static int hc16xx_add_core(struct hc16xx_glue *glue, u8 mode)
{
	struct musb_hdrc_platform_data pdata = {
		.mode		= mode,
		.config		= &hc16xx_musb_config,
		.platform_ops	= &hc16xx_musb_ops,
	};
	struct resource res_list[2];
	struct platform_device *pdev;
	int ret;

	pdev = platform_device_alloc("musb-hdrc", PLATFORM_DEVID_AUTO);
	if (!pdev)
		return -ENOMEM;

	pdev->dev.parent = glue->dev;

	res_list[0] = glue->mem;
	res_list[1] = DEFINE_RES_IRQ_NAMED(glue->irq, "mc");
	ret = platform_device_add_resources(pdev, res_list, 2);
	if (ret)
		goto err_put;

	/* a copy for this core only (was one static struct for all controllers) */
	ret = platform_device_add_data(pdev, &pdata, sizeof(pdata));
	if (ret)
		goto err_put;

	glue->mode = mode;
	glue->mode_applied = false;
	glue->musb = NULL;
	glue->init_ret = 1;		/* .init not called yet */

	ret = platform_device_add(pdev);
	if (ret)
		goto err_put;
	glue->musb_pdev = pdev;

	if (!device_is_bound(&pdev->dev) || !glue->musb) {
		/*
		 * Deferred, not failed: the core's start-up returned -EPROBE_DEFER
		 * (it now waits on the deferred-probe list), or the core never
		 * reached .init (e.g. the musb-hdrc driver is not registered yet).
		 * The original glue just added the core and returned 0, so a
		 * deferred core bound later; a hard -EIO here would fail the glue
		 * for good = no USB rescue gadget at all. Say -EPROBE_DEFER: the
		 * caller removes this core, and at boot the driver core probes the
		 * glue again later.
		 */
		if (glue->init_ret == 1 || glue->init_ret == -EPROBE_DEFER) {
			dev_info(glue->dev, "MUSB %s core start-up deferred\n",
				 hc16xx_mode_name(mode));
			return -EPROBE_DEFER;
		}
		dev_err(glue->dev, "MUSB %s core did not start (%d)\n",
			hc16xx_mode_name(mode), glue->init_ret);
		return -EIO;
	}

	/*
	 * Peripheral: as the original probe did, keep the controller IRQ off
	 * until a gadget is bound (musb_start() -> .enable). Host: musb_start()
	 * already ran when the root hub powered its port.
	 */
	if (mode == MUSB_PERIPHERAL)
		hc16xx_irq_off(glue);

	dev_info(glue->dev, "MUSB core started as %s (%s)\n",
		 hc16xx_mode_name(mode), dev_name(&pdev->dev));
	return 0;

err_put:
	platform_device_put(pdev);
	return ret;
}

static void hc16xx_del_core(struct hc16xx_glue *glue)
{
	if (!glue->musb_pdev)
		return;
	platform_device_unregister(glue->musb_pdev);
	glue->musb_pdev = NULL;
	glue->musb = NULL;
}

static int hc16xx_role_set(struct usb_role_switch *sw, enum usb_role role)
{
	struct hc16xx_glue *glue = usb_role_switch_get_drvdata(sw);
	u8 mode;
	int ret = 0;

	switch (role) {
	case USB_ROLE_HOST:
		mode = MUSB_HOST;
		break;
	case USB_ROLE_DEVICE:
		mode = MUSB_PERIPHERAL;
		break;
	default:
		/* "none": there is always one core; not offered */
		return -EINVAL;
	}

	mutex_lock(&glue->lock);
	if (mode == glue->mode && glue->musb_pdev)
		goto out;

	dev_info(glue->dev, "role switch: %s -> %s (re-creating the MUSB core)\n",
		 hc16xx_mode_name(glue->mode), hc16xx_mode_name(mode));
	hc16xx_del_core(glue);
	ret = hc16xx_add_core(glue, mode);
	if (ret && mode != MUSB_PERIPHERAL) {
		dev_err(glue->dev, "%s core failed (%d): back to peripheral\n",
			hc16xx_mode_name(mode), ret);
		hc16xx_del_core(glue);
		if (hc16xx_add_core(glue, MUSB_PERIPHERAL))
			dev_err(glue->dev, "peripheral core failed too\n");
	}
	/* -EPROBE_DEFER is kernel-internal; the sysfs writer gets "try again" */
	if (ret == -EPROBE_DEFER)
		ret = -EAGAIN;
out:
	mutex_unlock(&glue->lock);
	return ret;
}

static enum usb_role hc16xx_role_get(struct usb_role_switch *sw)
{
	struct hc16xx_glue *glue = usb_role_switch_get_drvdata(sw);

	return READ_ONCE(glue->mode) == MUSB_HOST ? USB_ROLE_HOST : USB_ROLE_DEVICE;
}

/* nzeus_host_force_vbus: 0/1, used the next time a HOST core is created. */
static ssize_t nzeus_host_force_vbus_show(struct device *dev,
					  struct device_attribute *attr, char *buf)
{
	struct hc16xx_glue *glue = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", glue->force_vbus);
}

static ssize_t nzeus_host_force_vbus_store(struct device *dev,
					   struct device_attribute *attr,
					   const char *buf, size_t n)
{
	struct hc16xx_glue *glue = dev_get_drvdata(dev);
	bool on;
	int ret;

	ret = kstrtobool(buf, &on);
	if (ret)
		return ret;
	mutex_lock(&glue->lock);
	glue->force_vbus = on;
	mutex_unlock(&glue->lock);
	return n;
}
static DEVICE_ATTR_RW(nzeus_host_force_vbus);

static struct attribute *hc16xx_glue_attrs[] = {
	&dev_attr_nzeus_host_force_vbus.attr,
	NULL
};
ATTRIBUTE_GROUPS(hc16xx_glue);

static int hc16xx_glue_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct hc16xx_glue *glue;
	struct resource *res;
	bool switchable = false;
	u8 mode;
	int ret;

	glue = devm_kzalloc(dev, sizeof(*glue), GFP_KERNEL);
	if (!glue)
		return -ENOMEM;

	platform_set_drvdata(pdev, glue);
	glue->dev = dev;
	mutex_init(&glue->lock);

	glue->irq = platform_get_irq_byname(pdev, "mc");
	if (glue->irq < 0)
		return glue->irq;

	glue->dma_irq = platform_get_irq_byname(pdev, "dma");
	if (glue->dma_irq < 0)
		return glue->dma_irq;

	glue->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(glue->clk))
		return PTR_ERR(glue->clk);

	glue->reset = devm_reset_control_get_exclusive(dev, NULL);
	if (IS_ERR(glue->reset))
		return PTR_ERR(glue->reset);

	/*
	 * Once per controller: the core is re-created on every role change and
	 * a devm PHY reference taken per core would pile up until unbind.
	 */
	glue->phy = devm_of_phy_get_by_index(dev, dev->of_node, 0);
	if (IS_ERR(glue->phy))
		return dev_err_probe(dev, PTR_ERR(glue->phy), "Unable to get PHY\n");

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -EINVAL;
	glue->mem = *res;

	switch (usb_get_dr_mode(dev)) {
	case USB_DR_MODE_HOST:
		mode = MUSB_HOST;
		break;
	case USB_DR_MODE_OTG:
		/* no ID pin: start as peripheral, user switches the role */
		mode = MUSB_PERIPHERAL;
		switchable = true;
		break;
	default:
		mode = MUSB_PERIPHERAL;
		break;
	}

	if (switchable) {
		struct usb_role_switch_desc desc = {
			.set = hc16xx_role_set,
			.get = hc16xx_role_get,
			.allow_userspace_control = true,
			.driver_data = glue,
			.fwnode = dev_fwnode(dev),
		};

		glue->role_sw = usb_role_switch_register(dev, &desc);
		if (IS_ERR(glue->role_sw))
			return dev_err_probe(dev, PTR_ERR(glue->role_sw),
					     "Failed to register USB role switch\n");
	}

	mutex_lock(&glue->lock);
	ret = hc16xx_add_core(glue, mode);
	if (ret)
		hc16xx_del_core(glue);
	mutex_unlock(&glue->lock);
	if (ret) {
		if (glue->role_sw)
			usb_role_switch_unregister(glue->role_sw);
		/* -EPROBE_DEFER (core start-up deferred): probed again later */
		return dev_err_probe(dev, ret, "MUSB core did not start\n");
	}

	if (switchable)
		dev_info(dev, "dr_mode otg: started as peripheral; role switch %s-role-switch\n",
			 dev_name(dev));
	return 0;
}

static void hc16xx_glue_remove(struct platform_device *pdev)
{
	struct hc16xx_glue *glue = platform_get_drvdata(pdev);

	if (glue->role_sw)
		usb_role_switch_unregister(glue->role_sw);
	mutex_lock(&glue->lock);
	hc16xx_del_core(glue);
	/* the core and its runtime PM are gone: now the clock may go off */
	if (glue->clk_on) {
		clk_disable_unprepare(glue->clk);
		glue->clk_on = false;
	}
	mutex_unlock(&glue->lock);
}

static const struct of_device_id hc16xx_id_table[] = {
	{ .compatible = "hichip,hc16xx-musb", },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, hc16xx_id_table);

static struct platform_driver hc16xx_driver = {
	.probe = hc16xx_glue_probe,
	.remove = hc16xx_glue_remove,
	.driver = {
		.name = "musb-hc16xx",
		.of_match_table = of_match_ptr(hc16xx_id_table),
		.dev_groups = hc16xx_glue_groups,
	},
};

MODULE_DESCRIPTION("HC16xx MUSB Glue Layer");
MODULE_AUTHOR("Siarhei Volkau <lis8215@gmail.com>");
MODULE_LICENSE("GPL v2");
module_platform_driver(hc16xx_driver);
