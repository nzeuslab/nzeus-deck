// SPDX-License-Identifier: GPL-2.0
/*
 * NZEUS R36S: memory that stays LOCKED at boot and is handed to Linux only
 * after the kernel itself has checked that the factory AVP firmware is not
 * running (the AVP-free "pure Linux" setup).
 *
 * The board tree declares 0x030bf000..0x08000000 (about 79 MiB: the unused
 * framebuffer tail, then the AVP's sysmem, kshm rings and mmz0 pool) as RAM
 * but also as a reserved-memory region, so Linux never allocates from it.
 * Everything above 0x08000000 is a mirror of the lower 128 MiB and is never
 * declared. The AVP's boot-fb header and logo (0x02f92000..0x030bf000) stay
 * outside Linux in every mode.
 *
 * sysfs (root only), all checks read through KSEG1 (uncached, unmapped), so
 * neither the CPU cache nor the MMU setup can hide what the AVP writes:
 *   avp_check     hashes ALL the AVP's areas (sysmem, kshm rings, mmz0 pool)
 *                 twice, 10 s apart. STATIC = nothing wrote them = AVP not
 *                 running. Run it once with the AVP present: it must say
 *                 AVP_RUNNING, which proves the check can see the firmware.
 *   mirror_check  reads the live jiffies counter and a block of kernel text
 *                 at X and X + 128 MiB through KSEG1 (0xA0000000 window).
 *   release       write "release": runs avp_check again and only if STATIC
 *                 gives the whole region to the page allocator (once). Not
 *                 reversible until the next boot.
 * With the AVP running (normal setup) nothing here changes any memory.
 *
 * Brightness without the AVP: once release has proven the AVP is off, this
 * driver also registers a backlight device "r36s-pwm" (0..255) that programs
 * PWM channel 0 directly, the way the bootloader's hc-pwm driver does (clear
 * the channel's enable bit 7, write the low/high tick counts, set bit 7),
 * keeping the period the firmware left (about 270 ticks = 10 us). Before
 * release - i.e. whenever the AVP may own the PWM - it does not exist.
 */
#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/sysfs.h>
#include <asm/addrspace.h>
#include <asm/sections.h>

#define REGION_START	0x030bf000
#define REGION_END	0x08000000
#define MIRROR		0x08000000	/* 128 MiB */
/* AVP areas from the running stock tree (/hcrtos/memory-mapping). */
#define AVP_SYSMEM	0x03da2e50
#define AVP_SYSMEM_LEN	0x00b53600
#define AVP_KSHM	0x048f6450
#define AVP_KSHM_LEN	0x004aca00
#define AVP_MMZ0	0x04da2e50
#define AVP_MMZ0_LEN	0x0325d1b0	/* up to 0x08000000 */
#define CHECK_GAP_MS	10000
#define PWM0_BASE	0x18818414	/* channel bytes; counter 0 at +8 */

struct nzeus_ram {
	struct device *dev;
	phys_addr_t start, end;
	struct mutex lock;
	bool released;
	void __iomem *pwm;
	u32 period;			/* PWM0 ticks per cycle, from the firmware */
};

static int pwm_bl_update(struct backlight_device *bd)
{
	struct nzeus_ram *r = bl_get_data(bd);
	u32 level = backlight_get_brightness(bd);
	u32 high = DIV_ROUND_CLOSEST(r->period * level, 255);
	u8 chan;

	/* Same order as the bootloader's hc-pwm start routine. */
	chan = readb(r->pwm);
	writeb(chan & ~BIT(7), r->pwm);
	writel((high << 16) | (r->period - high), r->pwm + 8);
	writeb(chan | BIT(7), r->pwm);
	return 0;
}

static const struct backlight_ops pwm_bl_ops = {
	.options = BL_CORE_SUSPENDRESUME,
	.update_status = pwm_bl_update,
};

/* Only after the AVP is proven off (release). Starts at the current level. */
static void pwm_bl_register(struct nzeus_ram *r)
{
	struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.max_brightness = 255,
		.scale = BACKLIGHT_SCALE_NON_LINEAR,
	};
	struct backlight_device *bd;
	u32 counter, high;

	r->pwm = devm_ioremap(r->dev, PWM0_BASE, 12);	/* shared window: no request */
	if (!r->pwm)
		return;
	counter = readl(r->pwm + 8);
	high = counter >> 16;
	r->period = high + (counter & 0xffff);
	if (!(readb(r->pwm) & BIT(7)) || r->period < 100 || r->period > 1000) {
		dev_warn(r->dev, "PWM0 not in the firmware's state (%08x); no backlight device\n", counter);
		return;
	}
	props.brightness = DIV_ROUND_CLOSEST(high * 255, r->period);
	bd = devm_backlight_device_register(r->dev, "r36s-pwm", r->dev, r, &pwm_bl_ops, &props);
	if (IS_ERR(bd))
		dev_warn(r->dev, "backlight r36s-pwm not registered: %ld\n", PTR_ERR(bd));
	else
		dev_info(r->dev, "AVP off: backlight r36s-pwm on PWM0 (period %u, level %d)\n",
			 r->period, props.brightness);
}

static u64 hash_uncached(phys_addr_t start, size_t len)
{
	const volatile u32 *w = (const volatile u32 *)CKSEG1ADDR(start & ~3);
	u64 h = 1469598103934665603ULL;
	size_t i;

	for (i = 0; i < len / 4; i++) {
		h = (h ^ w[i]) * 1099511628211ULL;
		if (!(i & 0xffff))
			cond_resched();
	}
	return h;
}

/* true if all AVP areas were unchanged over CHECK_GAP_MS. */
static bool avp_static(char *buf, ssize_t *len)
{
	static const struct { const char *name; phys_addr_t start; size_t len; } area[] = {
		{ "SYSMEM", AVP_SYSMEM, AVP_SYSMEM_LEN },
		{ "KSHM", AVP_KSHM, AVP_KSHM_LEN },
		{ "MMZ0", AVP_MMZ0, AVP_MMZ0_LEN },
	};
	u64 before[ARRAY_SIZE(area)], after;
	bool quiet = true;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(area); i++)
		before[i] = hash_uncached(area[i].start, area[i].len);
	msleep(CHECK_GAP_MS);
	for (i = 0; i < ARRAY_SIZE(area); i++) {
		after = hash_uncached(area[i].start, area[i].len);
		quiet &= after == before[i];
		if (buf)
			*len += sysfs_emit_at(buf, *len, "%s %016llx -> %016llx %s\n", area[i].name,
					      before[i], after, after == before[i] ? "STATIC" : "CHANGED");
	}
	return quiet;
}

static ssize_t avp_check_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	struct nzeus_ram *r = dev_get_drvdata(dev);
	ssize_t len = 0;
	bool quiet;

	mutex_lock(&r->lock);
	quiet = avp_static(buf, &len);
	len += sysfs_emit_at(buf, len, "%s\n", quiet ? "AVP_NOT_RUNNING" : "AVP_RUNNING");
	mutex_unlock(&r->lock);
	return len;
}
static DEVICE_ATTR_ADMIN_RO(avp_check);

/* Siarhei's question: is X + 128 MiB really the same memory? (KSEG1 only) */
static ssize_t mirror_check_show(struct device *dev, struct device_attribute *attr,
				 char *buf)
{
	phys_addr_t j = __pa_symbol(&jiffies), text = __pa_symbol(_text);
	const volatile u32 *lo = (const volatile u32 *)CKSEG1ADDR(j);
	const volatile u32 *hi = (const volatile u32 *)CKSEG1ADDR(j + MIRROR);
	u32 a1, b1, a2, b2;
	unsigned int same = 0, i;
	ssize_t len;

	/*
	 * jiffies lives in the write-back data cache: the uncached reads below
	 * see RAM, which only changes when the line is written back (seen on
	 * the console 2026-10-10: X 0002905a -> 0002905a after 200 ms). So
	 * write the line back first; then RAM holds the live value.
	 */
	dma_cache_wback((unsigned long)&jiffies, sizeof(jiffies));
	a1 = *lo;
	b1 = *hi;
	msleep(200);
	dma_cache_wback((unsigned long)&jiffies, sizeof(jiffies));
	a2 = *lo;
	b2 = *hi;
	for (i = 0; i < 4096; i++)
		same += ((const volatile u32 *)CKSEG1ADDR(text))[i] ==
			((const volatile u32 *)CKSEG1ADDR(text + MIRROR))[i];
	len = sysfs_emit(buf, "JIFFIES phys %pa: X %08x -> %08x, X+128M %08x -> %08x\n",
			 &j, a1, a2, b1, b2);
	len += sysfs_emit_at(buf, len, "KERNEL_TEXT 4096 words at %pa: %u identical at X+128M\n",
			     &text, same);
	len += sysfs_emit_at(buf, len, "%s\n",
			     a1 != a2 && a2 == b2 && same == 4096 ? "MIRROR_CONFIRMED_KSEG1" :
			     "NOT_A_MIRROR_OR_INCONCLUSIVE");
	return len;
}
static DEVICE_ATTR_ADMIN_RO(mirror_check);

static ssize_t release_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct nzeus_ram *r = dev_get_drvdata(dev);
	unsigned long pages;

	if (!sysfs_streq(buf, "release"))
		return -EINVAL;
	mutex_lock(&r->lock);
	if (r->released) {
		mutex_unlock(&r->lock);
		return -EALREADY;
	}
	if (!avp_static(NULL, NULL)) {
		dev_warn(dev, "AVP areas changed: firmware is running, memory stays locked\n");
		mutex_unlock(&r->lock);
		return -EBUSY;
	}
	pages = free_reserved_area(phys_to_virt(r->start), phys_to_virt(r->end), -1, NULL);
	r->released = true;
	dev_info(dev, "AVP-free: gave %lu KiB (%pa..%pa) to Linux\n",
		 pages << (PAGE_SHIFT - 10), &r->start, &r->end);
	pwm_bl_register(r);
	mutex_unlock(&r->lock);
	return count;
}

static ssize_t release_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct nzeus_ram *r = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s %pa..%pa\n", r->released ? "RELEASED" : "LOCKED",
			  &r->start, &r->end);
}
static DEVICE_ATTR_ADMIN_RW(release);

static struct attribute *nzeus_ram_attrs[] = {
	&dev_attr_avp_check.attr,
	&dev_attr_mirror_check.attr,
	&dev_attr_release.attr,
	NULL,
};
ATTRIBUTE_GROUPS(nzeus_ram);

static int nzeus_ram_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct reserved_mem *rmem;
	struct device_node *np;
	struct nzeus_ram *r;

	np = of_parse_phandle(dev->of_node, "memory-region", 0);
	if (!np)
		return -ENODEV;
	rmem = of_reserved_mem_lookup(np);
	of_node_put(np);
	if (!rmem)
		return -ENODEV;
	/* Only the exact region this driver was written for. */
	if (rmem->base != REGION_START || rmem->base + rmem->size != REGION_END)
		return dev_err_probe(dev, -EINVAL, "unexpected region %pa + %pa\n",
				     &rmem->base, &rmem->size);
	r = devm_kzalloc(dev, sizeof(*r), GFP_KERNEL);
	if (!r)
		return -ENOMEM;
	r->dev = dev;
	r->start = rmem->base;
	r->end = rmem->base + rmem->size;
	mutex_init(&r->lock);
	platform_set_drvdata(pdev, r);
	dev_info(dev, "%pa..%pa locked (%llu KiB); released only after avp_check\n",
		 &r->start, &r->end, (unsigned long long)rmem->size >> 10);
	return 0;
}

static const struct of_device_id nzeus_ram_of_match[] = {
	{ .compatible = "nzeus,r36s-avp-free-ram" },
	{ }
};

static struct platform_driver nzeus_ram_driver = {
	.probe = nzeus_ram_probe,
	.driver = {
		.name = "nzeus-ram",
		.of_match_table = nzeus_ram_of_match,
		.dev_groups = nzeus_ram_groups,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(nzeus_ram_driver);
