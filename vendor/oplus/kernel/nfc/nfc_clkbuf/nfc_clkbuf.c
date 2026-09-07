// SPDX-License-Identifier: GPL-2.0
/*
 * nfc_clkbuf - force-enable the MT6359P XO_NFC clock buffer for the 6.6 port.
 *
 * The 4.19 tree enables XO_NFC through the in-tree clkbuf driver, which binds
 * the DTB node "mediatek,pmic_clock_buffer" (CONFIG_MTK_CLKBUF_NFC; there
 * CLK_BUF3 == XO_NFC == CLOCK_BUFFER_SW_CONTROL).  On 6.6 the replacement
 * clkbuf driver only matches the new-schema "mediatek,mtXXXX-clkbuf" nodes, so
 * nothing drives our legacy node and XO_NFC is left off.
 *
 * The NXP SN100T ROM runs on its internal oscillator (so the FW-version read
 * succeeds), but the NCI firmware needs the external 26 MHz reference; without
 * XO_NFC every NCI i2c transaction fails (-ETIMEDOUT / -ENXIO) and NFC stays
 * stuck at "turning on".
 *
 * This shim binds the legacy node and forces EXTBUF3 (XO_NFC) into SW mode with
 * EN_M = 1 -- the SW-controlled/enabled state the 4.19 driver leaves it in.
 * Register layout matches the MT6359P DCXO driver in the 6.6 tree
 * (drivers/misc/mediatek/clkbuf/src/dcxo-6359p.c).
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/delay.h>
#include <linux/mfd/mt6397/core.h>

/* MT6359P DCXO registers (see dcxo-6359p.c) */
#define DCXO_CW00		0x788
#define XO_EXTBUF3_MODE_MASK	0x3
#define XO_EXTBUF3_MODE_SHIFT	6
#define XO_EXTBUF3_EN_M_MASK	0x1
#define XO_EXTBUF3_EN_M_SHIFT	8

#define XO_BUF_CTL2		0x550	/* XO_NFC vote register (read for diag) */

/*
 * AUXOUT: real hardware buffer-on status.  Write sel=6 to XO_STATIC_AUXOUT_SEL
 * (DCXO_CW16[5:0]) then read XO_STATIC_AUXOUT (DCXO_CW17); the enable bits for
 * SOC/WCN/NFC/CEL show up at bit13/11/9/7 respectively.
 */
#define DCXO_CW16		0x7b0
#define DCXO_CW17		0x7b2
#define XO_AUXOUT_SEL_MASK	0x3f
#define XO_EN_AUXOUT_SEL	6
#define AUXOUT_SOC_EN_BIT	13
#define AUXOUT_WCN_EN_BIT	11
#define AUXOUT_NFC_EN_BIT	9
#define AUXOUT_CEL_EN_BIT	7

/*
 * EXTBUF mode field (2 bits).  Empirically on this MT6359P the always-on
 * buffers (EXTBUF1/SOC and EXTBUF4/CEL) sit at mode=1,en=1, and EXTBUF2/WCN
 * runs at mode=1,en=0 driven purely by the connsys HW vote.  So mode=1 is the
 * HW/vote mode and EN_M=1 is an unconditional SW "always on" request that ORs
 * with the votes.  NFC has no autonomous HW voter, so it needs EN_M=1.
 * Defaults match the proven SOC/CEL pattern; both are module params so the
 * enable can be retuned with an insmod arg instead of a rebuild.
 */
static int mode = 1;
module_param(mode, int, 0644);
MODULE_PARM_DESC(mode, "EXTBUF3 mode field value (0-3), default 1");

static int en = 1;
module_param(en, int, 0644);
MODULE_PARM_DESC(en, "EXTBUF3 EN_M value (0/1), default 1");

static struct regmap *nfc_clkbuf_get_pmic_regmap(struct device *dev)
{
	struct device_node *np;
	struct platform_device *pmic_pdev;
	struct mt6397_chip *chip;
	struct regmap *regmap = NULL;

	np = of_find_compatible_node(NULL, NULL, "mediatek,mt6359-pmic");
	if (!np) {
		dev_info(dev, "mt6359-pmic node not found\n");
		return NULL;
	}

	pmic_pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!pmic_pdev)
		return NULL;

	/*
	 * mt6397-core stores mt6397_chip as the mt6359-pmic device drvdata and
	 * borrows its regmap from the pwrap parent.  Prefer the chip pointer,
	 * fall back to the parent regmap directly.
	 */
	chip = platform_get_drvdata(pmic_pdev);
	if (chip && chip->regmap)
		regmap = chip->regmap;
	else
		regmap = dev_get_regmap(pmic_pdev->dev.parent, NULL);

	put_device(&pmic_pdev->dev);
	return regmap;
}

static void nfc_clkbuf_log_state(struct device *dev, const char *tag,
				 unsigned int cw00)
{
	dev_info(dev, "%s: DCXO_CW00=0x%04x (EXTBUF3 mode=%u en=%u)\n", tag, cw00,
		 (cw00 >> XO_EXTBUF3_MODE_SHIFT) & XO_EXTBUF3_MODE_MASK,
		 (cw00 >> XO_EXTBUF3_EN_M_SHIFT) & XO_EXTBUF3_EN_M_MASK);
}

/* Read the real hardware buffer-on status via AUXOUT and log all four extbufs. */
static void nfc_clkbuf_log_auxout(struct device *dev, struct regmap *regmap)
{
	unsigned int aux = 0;

	regmap_update_bits(regmap, DCXO_CW16, XO_AUXOUT_SEL_MASK,
			   XO_EN_AUXOUT_SEL);
	regmap_read(regmap, DCXO_CW17, &aux);
	dev_info(dev,
		 "auxout=0x%04x -> SOC_en=%u WCN_en=%u NFC_en=%u CEL_en=%u\n",
		 aux,
		 (aux >> AUXOUT_SOC_EN_BIT) & 1, (aux >> AUXOUT_WCN_EN_BIT) & 1,
		 (aux >> AUXOUT_NFC_EN_BIT) & 1, (aux >> AUXOUT_CEL_EN_BIT) & 1);
}

static int nfc_clkbuf_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct regmap *regmap;
	unsigned int cw00 = 0, ctl2 = 0;
	int ret;

	regmap = nfc_clkbuf_get_pmic_regmap(dev);
	if (!regmap) {
		/* PMIC MFD may not have probed yet; let the driver core retry. */
		dev_info(dev, "pmic regmap not ready, deferring probe\n");
		return -EPROBE_DEFER;
	}

	regmap_read(regmap, DCXO_CW00, &cw00);
	regmap_read(regmap, XO_BUF_CTL2, &ctl2);
	nfc_clkbuf_log_state(dev, "before", cw00);
	dev_info(dev, "XO_BUF_CTL2(NFC vote)=0x%04x\n", ctl2);
	nfc_clkbuf_log_auxout(dev, regmap);

	/* EXTBUF3 -> configured mode + EN_M in a single masked update. */
	ret = regmap_update_bits(regmap, DCXO_CW00,
				 (XO_EXTBUF3_MODE_MASK << XO_EXTBUF3_MODE_SHIFT) |
				 (XO_EXTBUF3_EN_M_MASK << XO_EXTBUF3_EN_M_SHIFT),
				 ((mode & XO_EXTBUF3_MODE_MASK) << XO_EXTBUF3_MODE_SHIFT) |
				 ((en & XO_EXTBUF3_EN_M_MASK) << XO_EXTBUF3_EN_M_SHIFT));
	if (ret) {
		/* Never fail probe: a first-stage init failure panics the boot. */
		dev_err(dev, "failed to enable XO_NFC: %d\n", ret);
		return 0;
	}

	/* Allow the buffer output to settle (clkbuf waits after SW enable). */
	usleep_range(500, 1000);

	regmap_read(regmap, DCXO_CW00, &cw00);
	nfc_clkbuf_log_state(dev, "after ", cw00);
	nfc_clkbuf_log_auxout(dev, regmap);
	dev_info(dev, "XO_NFC clock buffer enabled\n");

	return 0;
}

static int nfc_clkbuf_remove(struct platform_device *pdev)
{
	/* Leave XO_NFC on across unload so NFC keeps running. */
	return 0;
}

static const struct of_device_id nfc_clkbuf_of_match[] = {
	{ .compatible = "mediatek,pmic_clock_buffer", },
	{ },
};
MODULE_DEVICE_TABLE(of, nfc_clkbuf_of_match);

static struct platform_driver nfc_clkbuf_driver = {
	.probe	= nfc_clkbuf_probe,
	.remove	= nfc_clkbuf_remove,
	.driver	= {
		.name		= "nfc_clkbuf",
		.of_match_table	= of_match_ptr(nfc_clkbuf_of_match),
	},
};
module_platform_driver(nfc_clkbuf_driver);

MODULE_DESCRIPTION("Force-enable MT6359P XO_NFC clock buffer (op6893 6.6 NFC bring-up)");
MODULE_LICENSE("GPL v2");
