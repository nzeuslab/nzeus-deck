// SPDX-License-Identifier: GPL-2.0-or-later
/* Experimental E3100 framebuffer using firmware-initialized DE/DSI timing.
 * Register and descriptor layout from HiChip hcfb.c:
 * Copyright (c) 2021 HiChip Semiconductor Co., Ltd.
 * This driver does not initialize the panel, backlight, clocks or AVP.
 *
 * With CONFIG_NZEUS_AVP_RPC, while our layer is active a watchdog checks every
 * 200 ms for the factory AVP's periodic output blank (GMA0 switched off, output
 * mode bit 0 set, or the descriptor replaced). It then asks the AVP to re-apply
 * its own display mode (which restores correct colours) and re-enables our
 * layer. Without that option the driver behaves exactly as before.
 *
 * Two pixel formats (2026-10-12): ARGB8888 (32 bits, the default, as before)
 * and RGB565 (16 bits) - HiChip's GMA format 6 (hcfb.c HCFB_FMT_RGB565,
 * descriptor dword0 bits 7:4). The program that owns the screen chooses with
 * FBIOPUT_VSCREENINFO (bits_per_pixel 16 or 32). 16 bits moves half the
 * data: the console's memory is slow (a full 32-bit frame took 17.9 ms to
 * write), and NZEUS Deck draws in RGB565 anyway, so no conversion is needed.
 * RGB565 has no alpha: the layer is drawn fully opaque (global alpha 255).
 */
#include <linux/dma-mapping.h>
#include <linux/fb.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/workqueue.h>

#define GMA_OUT2 0x080	/* output config; bit 0 set = AVP blank/tint mode */
#define GMA_CTL 0x300
#define GMA_DMBA 0x304
#define GMA_ALPHA 0x308
#define GMA_MASK 0x350
#define GMA_CTL_HW 0xb00
#define WIDTH 640
#define HEIGHT 480
#define STRIDE (WIDTH * 4)		/* the 32-bit stride; 16 bits uses half */
#define PIXEL_BYTES (STRIDE * HEIGHT)	/* enough for either format */
#define FMT_ARGB8888 1			/* hcfb.c HCFB_FMT_* (descriptor bits 7:4) */
#define FMT_RGB565 6
#define HEADER_BYTES 640
#define WATCH_MS 200
#define WATCH_RETRY_MS 5000
#define WATCH_MAX_FAILURES 20

#if IS_ENABLED(CONFIG_NZEUS_AVP_RPC)
int nzeus_avp_reapply_tvsys_hd(void);	/* drivers/misc/nzeus-avp-rpc.c */
#else
static inline int nzeus_avp_reapply_tvsys_hd(void) { return -ENODEV; }
#endif

struct hc_gma {
	struct device *dev;
	void __iomem *regs;
	void *pixels;
	dma_addr_t pixels_dma;
	__le32 *header;
	dma_addr_t header_dma;
	struct mutex lock;
	u32 palette[16];
	u32 saved_ctl, saved_dmba, saved_alpha, saved_mask;
	bool active;
	bool resume_layer;		/* our layer was showing when we suspended */
	struct delayed_work watch;	/* AVP blank recovery, only while active */
	unsigned int recoveries, failures;
	unsigned int bpp;		/* 32 (ARGB8888, default) or 16 (RGB565) */
};

/* Keep the hardware descriptor independent of compiler bitfield layout. */
static void hc_header(struct hc_gma *g)
{
	unsigned int stride = WIDTH * (g->bpp / 8);
	__le32 h[HEADER_BYTES / 4];
	/* GPL vendor default BT.709 matrix, also read unchanged from the live
	 * factory descriptor. A fresh DMA descriptor tests coefficient reload.
	 */
	static const s32 coefficients[] = {
		93, 314, 32, 0, -52, -173, 225, 0, 225, -204, -21, 0, 0
	};
	unsigned int i;

	/* Built in a local copy, then copied whole: the hardware may be reading
	 * the descriptor when the format changes. */
	memset(h, 0, sizeof(h));
	if (g->bpp == 16)
		/* RGB565: no alpha in the pixels -> global alpha 255 (bit 8
		 * colour-by-colour off), no premultiply (bit 11 off). */
		h[0] = cpu_to_le32(BIT(0) | (FMT_RGB565 << 4) | BIT(9) |
				   BIT(12) | (32 << 16) | (170U << 24));
	else
		/* Match the known firmware ARGB8888 path, including color conversion,
		 * premultiplied alpha and stock edge thresholds. Scaling stays off.
		 */
		h[0] = cpu_to_le32(BIT(0) | (FMT_ARGB8888 << 4) | BIT(8) | BIT(9) |
				   BIT(11) | BIT(12) | (32 << 16) | (170U << 24));
	h[2] = cpu_to_le32((WIDTH - 1) << 16);
	h[3] = cpu_to_le32((HEIGHT - 1) << 16);
	h[4] = cpu_to_le32(WIDTH | (HEIGHT << 16));
	h[5] = cpu_to_le32(255 | (stride << 16));
	h[7] = cpu_to_le32(g->pixels_dma);
	for (i = 0; i < ARRAY_SIZE(coefficients); i++)
		h[144 + i] = cpu_to_le32(coefficients[i]);
	memcpy(g->header, h, HEADER_BYTES);
}

/* the standard description of one of our two formats */
static void hc_fill_var(struct fb_var_screeninfo *v, unsigned int bpp)
{
	v->bits_per_pixel = bpp;
	if (bpp == 16) {
		v->red = (struct fb_bitfield){11, 5, 0};
		v->green = (struct fb_bitfield){5, 6, 0};
		v->blue = (struct fb_bitfield){0, 5, 0};
		v->transp = (struct fb_bitfield){0, 0, 0};
	} else {
		v->red = (struct fb_bitfield){16, 8, 0};
		v->green = (struct fb_bitfield){8, 8, 0};
		v->blue = (struct fb_bitfield){0, 8, 0};
		v->transp = (struct fb_bitfield){24, 8, 0};
	}
}

static void hc_restore(struct hc_gma *g)
{
	writel(g->saved_mask | BIT(0), g->regs + GMA_MASK);
	writel(g->saved_dmba, g->regs + GMA_DMBA);
	writel(g->saved_alpha, g->regs + GMA_ALPHA);
	writel(g->saved_ctl, g->regs + GMA_CTL);
	writel(g->saved_mask, g->regs + GMA_MASK);
	readl(g->regs + GMA_MASK); /* Flush the posted writes. */
}

/* Point GMA0 at our descriptor and enable it under the update mask. */
static int hc_enable_ours(struct hc_gma *g)
{
	u32 value;

	dma_wmb();
	writel(g->saved_mask | BIT(0), g->regs + GMA_MASK);
	writel(g->header_dma, g->regs + GMA_DMBA);
	/* Preserve unrelated fields; disable global alpha attenuation. */
	writel((g->saved_alpha & ~(BIT(16) | 0xff)) | 0xff,
	       g->regs + GMA_ALPHA);
	/* Native firmware color path: CSC on, sharpness on (zero coefficient).
	 * CSC bypass was visibly incorrect in earlier tests and is not used.
	 */
	writel(BIT(18) | BIT(0), g->regs + GMA_CTL);
	writel(g->saved_mask, g->regs + GMA_MASK);
	return readl_poll_timeout(g->regs + GMA_CTL_HW, value,
				  value & BIT(0), 1000, 100000);
}

/*
 * The factory AVP blanks the output about every 2 minutes when no stock
 * system answers it (GMA0 off, output mode bit 0 set). Re-applying the AVP's
 * own display mode restores correct colours; then we re-enable our layer.
 */
static void hc_watch(struct work_struct *work)
{
	struct hc_gma *g = container_of(to_delayed_work(work), struct hc_gma, watch);
	unsigned long delay = msecs_to_jiffies(WATCH_MS);
	u32 ctl, out, dmba;
	int avp, layer;

	mutex_lock(&g->lock);
	if (!g->active)
		goto out;
	ctl = readl(g->regs + GMA_CTL);
	out = readl(g->regs + GMA_OUT2);
	dmba = readl(g->regs + GMA_DMBA);
	if ((ctl & BIT(0)) && !(out & BIT(0)) && dmba == g->header_dma)
		goto again;
	avp = nzeus_avp_reapply_tvsys_hd();
	layer = hc_enable_ours(g);
	g->recoveries++;
	dev_info(g->dev, "AVP blank %u (ctl=%08x out=%08x dmba=%08x): reapply %d, layer %d, now out=%08x\n",
		 g->recoveries, ctl, out, dmba, avp, layer, readl(g->regs + GMA_OUT2));
	if (avp || layer) {
		delay = msecs_to_jiffies(WATCH_RETRY_MS);
		if (++g->failures >= WATCH_MAX_FAILURES) {
			dev_warn(g->dev, "AVP blank recovery failed %u times; watchdog stopped\n",
				 g->failures);
			goto out;
		}
	} else {
		g->failures = 0;
	}
again:
	schedule_delayed_work(&g->watch, delay);
out:
	mutex_unlock(&g->lock);
}

/* Only an explicit FBIOBLANK unblank starts scanout from our buffer.
 * Blanking returns the original firmware layer. No memory is freed here.
 */
static int hc_blank(int blank, struct fb_info *info)
{
	struct hc_gma *g = info->par;
	bool stop_watch = false;
	int ret = 0;

	if (blank != FB_BLANK_UNBLANK && blank != FB_BLANK_NORMAL &&
	    blank != FB_BLANK_POWERDOWN)
		return -EINVAL;
	mutex_lock(&g->lock);
	if (blank != FB_BLANK_UNBLANK) {
		if (g->active) {
			hc_restore(g);
			g->active = false;
			stop_watch = true;
		}
		goto out;
	}
	if (g->active)
		goto out;
	g->saved_mask = readl(g->regs + GMA_MASK);
	if (g->saved_mask & BIT(0)) {
		ret = -EBUSY; /* Another owner is updating this layer. */
		goto out;
	}
	g->saved_ctl = readl(g->regs + GMA_CTL);
	g->saved_dmba = readl(g->regs + GMA_DMBA);
	g->saved_alpha = readl(g->regs + GMA_ALPHA);
	if ((g->saved_ctl != 0 && g->saved_ctl != (BIT(18) | BIT(0))) ||
	    g->saved_dmba != 0x0af92a80 || g->saved_alpha != 0xff) {
		ret = -EBUSY; /* Only take over the audited factory logo layer. */
		goto out;
	}
	ret = hc_enable_ours(g);
	if (ret) {
		hc_restore(g);
		dev_warn(g->dev, "GMA enable timed out; restored firmware registers\n");
	} else {
		g->active = true;
		dev_info(g->dev, "GMA enabled; screen output still requires visual verification\n");
		if (IS_ENABLED(CONFIG_NZEUS_AVP_RPC)) {
			g->failures = 0;
			schedule_delayed_work(&g->watch, msecs_to_jiffies(WATCH_MS));
		}
	}
out:
	mutex_unlock(&g->lock);
	/* Outside the lock: the watchdog takes it and exits once inactive. */
	if (stop_watch)
		cancel_delayed_work_sync(&g->watch);
	return ret;
}

static int hc_setcolreg(unsigned int reg, unsigned int red, unsigned int green,
			unsigned int blue, unsigned int transp, struct fb_info *info)
{
	struct hc_gma *g = info->par;
	if (reg >= ARRAY_SIZE(g->palette))
		return -EINVAL;
	g->palette[reg] = ((transp >> 8) << 24) | ((red >> 8) << 16) |
			  ((green >> 8) << 8) | (blue >> 8);
	return 0;
}

static int hc_check_var(struct fb_var_screeninfo *v, struct fb_info *info)
{
	if (v->xres != WIDTH || v->yres != HEIGHT ||
	    v->xres_virtual != WIDTH || v->yres_virtual != HEIGHT ||
	    (v->bits_per_pixel != 32 && v->bits_per_pixel != 16) ||
	    v->xoffset || v->yoffset || v->nonstd || v->rotate || v->grayscale)
		return -EINVAL;
	{
		unsigned int bpp = v->bits_per_pixel;

		*v = info->var;		/* everything else stays fixed */
		hc_fill_var(v, bpp);
	}
	return 0;
}

/* Switch between ARGB8888 and RGB565: new stride + descriptor; if our layer
 * is showing, the hardware is pointed at the new descriptor at once. */
static int hc_set_par(struct fb_info *info)
{
	struct hc_gma *g = info->par;
	unsigned int bpp = info->var.bits_per_pixel;
	int ret = 0;

	if (bpp != 16 && bpp != 32)
		return -EINVAL;
	mutex_lock(&g->lock);
	if (bpp != g->bpp) {
		g->bpp = bpp;
		info->fix.line_length = WIDTH * (bpp / 8);
		hc_header(g);
		if (g->active)
			ret = hc_enable_ours(g);
		dev_info(g->dev, "pixel format now %s (%d)\n",
			 bpp == 16 ? "RGB565" : "ARGB8888", ret);
	}
	mutex_unlock(&g->lock);
	return ret;
}

static int hc_mmap(struct fb_info *info, struct vm_area_struct *vma)
{
	struct hc_gma *g = info->par;

	/* Uncached on purpose. Write-combining ("uncached accelerated") was tried
	 * on 2026-10-12: Linux's cpu-probe.c sets writecombine = _CACHE_UNCACHED
	 * for the MIPS 74K (PRID_IMP_74K) on purpose, so DMA_ATTR_WRITE_COMBINE
	 * would change nothing here, and forcing the accelerated mode past that
	 * decision is not worth the risk. The 16-bit mode is the speed-up. */
	return dma_mmap_coherent(g->dev, vma, g->pixels, g->pixels_dma, PIXEL_BYTES);
}

static const struct fb_ops hc_ops = {
	.owner = THIS_MODULE,
	__FB_DEFAULT_DMAMEM_OPS_RDWR,
	__FB_DEFAULT_DMAMEM_OPS_DRAW,
	.fb_check_var = hc_check_var,
	.fb_set_par = hc_set_par,
	.fb_setcolreg = hc_setcolreg,
	.fb_blank = hc_blank,
	.fb_mmap = hc_mmap,
};

static int hc_probe(struct platform_device *pdev)
{
	struct resource *res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	struct fb_info *info;
	struct hc_gma *g;
	int ret;

	/* Board-specific opt-in, not a generic replacement for a panel driver. */
	if (!of_machine_is_compatible("nzeus,r36s-e3100"))
		return -ENODEV;
	if (!of_property_read_bool(pdev->dev.of_node, "nzeus,firmware-timing"))
		return -EINVAL;
	if (!res || res->start != 0x1883a000 || resource_size(res) != 0x1000)
		return -EINVAL;
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(28));
	if (ret)
		return ret;
	info = framebuffer_alloc(sizeof(*g), &pdev->dev);
	if (!info)
		return -ENOMEM;
	g = info->par;
	g->dev = &pdev->dev;
	mutex_init(&g->lock);
	INIT_DELAYED_WORK(&g->watch, hc_watch);
	g->regs = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(g->regs)) {
		ret = PTR_ERR(g->regs);
		goto fail_info;
	}
	g->pixels = dma_alloc_coherent(g->dev, PIXEL_BYTES, &g->pixels_dma, GFP_KERNEL);
	g->header = dma_alloc_coherent(g->dev, HEADER_BYTES, &g->header_dma, GFP_KERNEL);
	if (!g->pixels || !g->header) {
		ret = -ENOMEM;
		goto fail_dma;
	}
	if (g->header_dma & 31) {
		ret = -EINVAL;
		goto fail_dma;
	}
	memset(g->pixels, 0, PIXEL_BYTES);
	g->bpp = 32;				/* ARGB8888 until a program asks for 16 */
	hc_header(g);
	info->fix = (struct fb_fix_screeninfo) {
		.id = "hc16xx-gma-argb", .smem_start = g->pixels_dma,
		.smem_len = PIXEL_BYTES, .type = FB_TYPE_PACKED_PIXELS,
		.visual = FB_VISUAL_TRUECOLOR, .line_length = STRIDE,
		.accel = FB_ACCEL_NONE,
	};
	info->var = (struct fb_var_screeninfo) {
		.xres = WIDTH, .yres = HEIGHT, .xres_virtual = WIDTH,
		.yres_virtual = HEIGHT, .bits_per_pixel = 32,
		.red = {16, 8, 0}, .green = {8, 8, 0}, .blue = {0, 8, 0},
		.transp = {24, 8, 0},
		.height = -1, .width = -1, .activate = FB_ACTIVATE_NOW,
		.vmode = FB_VMODE_NONINTERLACED,
	};
	info->fbops = &hc_ops;
	info->flags = FBINFO_VIRTFB;
	info->screen_buffer = g->pixels;
	info->screen_size = PIXEL_BYTES;
	info->pseudo_palette = g->palette;
	ret = fb_alloc_cmap(&info->cmap, 16, 0);
	if (ret)
		goto fail_dma;
	ret = register_framebuffer(info);
	if (ret) {
		fb_dealloc_cmap(&info->cmap);
		goto fail_dma;
	}
	platform_set_drvdata(pdev, info);
	dev_info(g->dev, "fb%d ready, 640x480 ARGB8888, native CSC, INACTIVE until explicit unblank\n", info->node);
	return 0;
fail_dma:
	if (g->header)
		dma_free_coherent(g->dev, HEADER_BYTES, g->header, g->header_dma);
	if (g->pixels)
		dma_free_coherent(g->dev, PIXEL_BYTES, g->pixels, g->pixels_dma);
fail_info:
	framebuffer_release(info);
	return ret;
}

/*
 * System sleep: hand the layer back to the firmware, as FBIOBLANK does, and
 * retake it on resume if it was ours. fb_blank() is not called on suspend in
 * this kernel.
 */
static int hc_suspend(struct device *dev)
{
	struct fb_info *info = dev_get_drvdata(dev);
	struct hc_gma *g;
	bool stop_watch = false;

	if (!info)
		return 0;
	g = info->par;
	mutex_lock(&g->lock);
	g->resume_layer = g->active;
	if (g->active) {
		hc_restore(g);
		g->active = false;
		stop_watch = true;
		dev_info(g->dev, "layer returned to firmware for system sleep\n");
	}
	mutex_unlock(&g->lock);
	if (stop_watch)
		cancel_delayed_work_sync(&g->watch);
	return 0;
}

static int hc_resume(struct device *dev)
{
	struct fb_info *info = dev_get_drvdata(dev);
	struct hc_gma *g;
	int ret = 0;

	if (!info)
		return 0;
	g = info->par;
	mutex_lock(&g->lock);
	if (g->resume_layer && !g->active) {
		ret = hc_enable_ours(g);
		if (!ret) {
			g->active = true;
			g->failures = 0;
			if (IS_ENABLED(CONFIG_NZEUS_AVP_RPC))
				schedule_delayed_work(&g->watch, msecs_to_jiffies(WATCH_MS));
			dev_info(g->dev, "layer retaken after system resume\n");
		} else {
			dev_warn(g->dev, "resume: GMA enable failed %d, left to firmware\n", ret);
		}
	}
	g->resume_layer = false;
	mutex_unlock(&g->lock);
	return 0;
}

static const struct dev_pm_ops hc_pm_ops = {
	SYSTEM_SLEEP_PM_OPS(hc_suspend, hc_resume)
};

static const struct of_device_id hc_match[] = {
	{ .compatible = "nzeus,hc16xx-gma-firmware-fb" }, { }
};

/* Built-in only, no unbind: scanout memory must live until the next reset. */
static struct platform_driver hc_driver = {
	.probe = hc_probe,
	.driver = {
		.name = "hc16xx-gma-firmware-fb", .of_match_table = hc_match,
		.suppress_bind_attrs = true,
		.pm = pm_sleep_ptr(&hc_pm_ops),
	},
};
builtin_platform_driver(hc_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Experimental E3100 firmware-timed GMA framebuffer");
