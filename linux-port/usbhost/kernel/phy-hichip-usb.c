// SPDX-License-Identifier: GPL-2.0
/*
 * HiChip SoCs USB PHY driver
 *
 * Copyright (C) Siarhei Volkau <lis8215@gmail.com>
 *
 * NZEUS (linux-port/usbhost/README.md): optional "force VBUS valid" in host
 * mode, requested by the musb glue with phy_set_mode_ext(PHY_MODE_USB_HOST,
 * HICHIP_USB_PHY_FORCE_VBUS_VALID) only when the user set
 * nzeus_host_force_vbus=1. Without that request this driver touches no
 * register beyond what it did before (UPHY20 is never read or written).
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>

/* PHY register offsets */
#define REG_UPHY0_OFFSET	0x00
#define REG_UPHY2_OFFSET	0x02
#define REG_UPHY3_OFFSET	0x03
#define REG_UPHY4_OFFSET	0x04
#define REG_UPHY5_OFFSET	0x05
#define REG_UPHY20_OFFSET	0x20

/* Some known bits:
 * UPHY0[2:0] pre-emphasis (0x07 - always enable)
 * UPHY2[6:4] HS eye tuning
 * UPHY3[6:2] odt (default:0x15, 0x1f:fastest rise time; 0x10:slowest rise time)
 * UPHY3[7]   default is 1 (may be odt related)
 * UPHY4[7:4] disconnect threshold voltage (0x0d - 675mV, step might be 50mV)
 * UPHY5[4:2] TX HS pre-emphasis stength
 * UPHY5[4:2] TX HS pre-emphasis stength
 * UPHY20     0x8000 configure phy vbus/avalid/bvalid = 1
 */

#define UPHY4_DISCONNECT_THRESHOLD_MASK		GENMASK(7, 4)
#define UPHY4_DISCONNECT_THRESHOLD_675MV	0xd

/* UPHY20 bit 15: "configure phy vbus/avalid/bvalid = 1" (comment above) */
#define UPHY20_FORCE_VBUS_VALID			BIT(15)
/* phy_set_mode_ext() submode, same value in drivers/usb/musb/hc16xx.c */
#define HICHIP_USB_PHY_FORCE_VBUS_VALID		BIT(0)

struct hichip_usb_phy {
	struct phy *phy;
	void __iomem *base;
	bool vbus_forced;	/* UPHY20 changed by us; uphy20_saved holds the old value */
	u32 uphy20_saved;
};

/* Set or undo the forced VBUS-valid bit; no register access if nothing changes. */
static void hichip_usb_phy_force_vbus(struct phy *phy, bool on)
{
	struct hichip_usb_phy *priv = phy_get_drvdata(phy);

	if (on && !priv->vbus_forced) {
		priv->uphy20_saved = readl(priv->base + REG_UPHY20_OFFSET);
		writel(priv->uphy20_saved | UPHY20_FORCE_VBUS_VALID,
		       priv->base + REG_UPHY20_OFFSET);
		priv->vbus_forced = true;
		dev_info(&phy->dev, "host: VBUS valid forced (UPHY20 %08x -> %08x)\n",
			 priv->uphy20_saved,
			 readl(priv->base + REG_UPHY20_OFFSET));
	} else if (!on && priv->vbus_forced) {
		writel(priv->uphy20_saved, priv->base + REG_UPHY20_OFFSET);
		priv->vbus_forced = false;
		dev_info(&phy->dev, "VBUS valid no longer forced (UPHY20 %08x)\n",
			 readl(priv->base + REG_UPHY20_OFFSET));
	}
}

static int hichip_usb_phy_init(struct phy *phy)
{
	struct hichip_usb_phy *priv = phy_get_drvdata(phy);
	u8 tmp;

        /* set disconnect threshold voltage */
        tmp = readb(priv->base + REG_UPHY4_OFFSET);
        tmp &= ~UPHY4_DISCONNECT_THRESHOLD_MASK;
        tmp |= FIELD_PREP(UPHY4_DISCONNECT_THRESHOLD_MASK,
			  UPHY4_DISCONNECT_THRESHOLD_675MV);
        writeb(tmp, priv->base + REG_UPHY4_OFFSET);

#if 0 /* vendor says this is not required anymore */
	writeb(0x1f, priv->base + REG_UPHY0_OFFSET);
	writeb(0x56, priv->base + REG_UPHY2_OFFSET);
	writeb(0xe0, priv->base + REG_UPHY3_OFFSET);
	writeb(0xbc, priv->base + REG_UPHY5_OFFSET);
#endif
	return 0;
}

static int hichip_usb_phy_exit(struct phy *phy)
{
	struct hichip_usb_phy *priv = phy_get_drvdata(phy);

	(void)priv;
	return 0;
}

static int hichip_usb_phy_power_on(struct phy *phy)
{
	struct hichip_usb_phy *priv = phy_get_drvdata(phy);

	(void)priv;
	return 0;
}

static int hichip_usb_phy_power_off(struct phy *phy)
{
	struct hichip_usb_phy *priv = phy_get_drvdata(phy);

	(void)priv;
	return 0;
}

static int hichip_usb_phy_set_mode(struct phy *phy,
				  enum phy_mode mode, int submode)
{
	/* Host/device itself is handled entirely by the UTMI register in the
	 * musb glue layer. Only the optional forced VBUS-valid lives here.
	 */
	switch (mode) {
	case PHY_MODE_USB_HOST:
		/*
		 * Only switches the force ON. A plain HOST request (the MUSB
		 * core's own musb_host_setup() sends one) keeps the state;
		 * DEVICE/OTG (glue exit) undoes it.
		 */
		if (submode & HICHIP_USB_PHY_FORCE_VBUS_VALID)
			hichip_usb_phy_force_vbus(phy, true);
		break;
	case PHY_MODE_USB_DEVICE:
	case PHY_MODE_USB_OTG:
		hichip_usb_phy_force_vbus(phy, false);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static const struct phy_ops hichip_usb_phy_ops = {
	.init		= hichip_usb_phy_init,
	.exit		= hichip_usb_phy_exit,
	.power_on	= hichip_usb_phy_power_on,
	.power_off	= hichip_usb_phy_power_off,
	.set_mode	= hichip_usb_phy_set_mode,
	.owner		= THIS_MODULE,
};

static int hichip_usb_phy_probe(struct platform_device *pdev)
{
	struct hichip_usb_phy *priv;
	struct phy_provider *provider;
	struct device *dev = &pdev->dev;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->base)) {
		dev_err(dev, "Failed to map registers\n");
		return PTR_ERR(priv->base);
	}

	priv->phy = devm_phy_create(dev, NULL, &hichip_usb_phy_ops);
	if (IS_ERR(priv->phy))
		return PTR_ERR(priv->phy);

	phy_set_drvdata(priv->phy, priv);

	provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);

	return PTR_ERR_OR_ZERO(provider);
}

static const struct of_device_id hichip_usb_phy_of_matches[] = {
	{ .compatible = "hichip,hc16xx-usb-phy", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, hichip_usb_phy_of_matches);

static struct platform_driver hichip_usb_phy_driver = {
	.probe		= hichip_usb_phy_probe,
	.driver		= {
		.name	= "hichip-usb-phy",
		.of_match_table = hichip_usb_phy_of_matches,
	},
};
module_platform_driver(hichip_usb_phy_driver);

MODULE_AUTHOR("Siarhei Volkau <lis8215@gmail.com>");
MODULE_DESCRIPTION("HiChip HC16xx USB PHY driver");
MODULE_LICENSE("GPL");
