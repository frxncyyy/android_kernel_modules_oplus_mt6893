// SPDX-License-Identifier: GPL-2.0
/*
 * MT6360 LDO bank regulator driver (legacy MediaTek binding).
 *
 * Copyright (C) 2021 MediaTek Inc.
 * Ported to 6.6 from the 4.19 vendor driver
 * drivers/misc/mediatek/pmic/mt6360/v1/ldo/{mt6360_ldo_i2c,mt6360_ldo_regmap}.c
 *
 * The MT6360 answers on four i2c slave addresses and 4.19 drove each of them
 * with a *separate*, independent i2c driver: mt6360_pmu @0x34 (charger, ADC,
 * flash, RGB), mt6360_pmic @0x1a (bucks), mt6360_ldo @0x64 (LDO1/2/3/5) and
 * usb_type_c @0x4e.  6.6 only ships the upstream MFD (mt6360-core.c +
 * mt6360-regulator.c), which binds "mediatek,mt6360" -- a compatible this
 * board's 4.19 DTB does not have.  Its i2c5 children are
 * "mediatek,subpmic{,_pmic,_ldo}" and "mediatek,usb_type_c", so nothing in the
 * 6.6 tree binds any of them.
 *
 * That matters because LDO2 is the touchscreen's analog rail: the FT3518 at
 * i2c0 0-0038 asks for "vdd_2v8" and the DTB points it at
 * /i2c5@11F00000/mt6360_ldo_dts/ldo2, regulator-name "vtp".  Without a
 * provider regulator_get() fails, the rail stays off and every touch i2c
 * transfer NAKs ("read chip id:0xfbfb").
 *
 * This is the LDO bank *only*.  It writes nothing outside slave 0x64; the one
 * access to another bank is a single read of PMU register 0x00 to check the
 * chip ID, exactly as 4.19 did.  It deliberately does not register the MFD,
 * the charger, the Type-C port or the shared interrupt controller.
 *
 * Deltas from the 4.19 original:
 *
 *  - Richtek rt_regmap is dropped.  4.19 built with CONFIG_RT_REGMAP unset on
 *    this platform, in which case mt6360_ldo_regmap.c compiles to empty stubs
 *    and every accessor already fell through to the raw CRC8 helpers below.
 *    Keeping it would have meant depending on rt-regmap, which in 6.6 is
 *    compiled into tcpc_class.ko.
 *  - The eight LDO over-current / power-good interrupt handlers are dropped.
 *    Their interrupt-parent is mt6360_pmu_dts, an interrupt-controller that
 *    only the (unported) PMU driver provides, so of_irq_to_resource_table()
 *    could never map them.  4.19's code path tolerated that; there is no
 *    point carrying it.
 *  - struct regulator_linear_range became struct linear_range (5.11), the i2c
 *    probe lost its id argument (6.3) and .remove returns void (6.3).
 */

#include <linux/crc8.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>

static bool dbg_log_en;
module_param(dbg_log_en, bool, 0644);

#define mt_dbg(dev, fmt, ...) \
	do { \
		if (dbg_log_en) \
			dev_info(dev, fmt, ##__VA_ARGS__); \
	} while (0)

enum {
	MT6360_LDO_LDO1,
	MT6360_LDO_LDO2,
	MT6360_LDO_LDO3,
	MT6360_LDO_LDO5,
	MT6360_LDO_MAX,
};

/* LDO bank register map (slave 0x64), only what is used here */
#define MT6360_LDO_LDO5_CTRL0		(0x0C)

struct mt6360_ldo_info {
	struct i2c_client *i2c;
	struct device *dev;
	struct regulator_dev *rdev[MT6360_LDO_MAX];
	struct mutex io_lock;
	u8 crc8_table[CRC8_TABLE_SIZE];
	u8 chip_rev;
	bool sdcard_det_en;
	bool sdcard_hlact;
};

struct mt6360_regulator_desc {
	const struct regulator_desc desc;
	unsigned int enst_reg;
	unsigned int enst_mask;
	unsigned int mode_reg;
	unsigned int mode_mask;
	unsigned int moder_reg;
	unsigned int moder_mask;
};

/*
 * The PMIC and LDO banks do not expose a flat register file: the register
 * index carries the access width in its top two bits and every transfer is
 * covered by a CRC8 (polynomial 0x07, MSB first) computed over the i2c
 * address byte, the encoded index and the payload.  This is why the rail
 * cannot be poked from userspace with i2cset.
 */
static int mt6360_ldo_read_device(struct i2c_client *i2c, u32 addr,
				  int len, void *dst)
{
	struct mt6360_ldo_info *mli = i2c_get_clientdata(i2c);
	u8 chunk[8] = {0};
	int ret;

	if ((addr & 0xc0) != 0 || len > 4 || len <= 0) {
		dev_err(&i2c->dev, "not support addr [%x], len [%d]\n",
			addr, len);
		return -EINVAL;
	}
	chunk[0] = ((i2c->addr & 0x7f) << 1) + 1;
	chunk[1] = (addr & 0x3f) | (((u8)(len - 1)) << 6);
	ret = i2c_smbus_read_i2c_block_data(i2c, chunk[1], len + 1, chunk + 2);
	if (ret < 0)
		return ret;
	chunk[7] = crc8(mli->crc8_table, chunk, 2 + len, 0);
	if (chunk[2 + len] != chunk[7])
		return -EINVAL;
	memcpy(dst, chunk + 2, len);
	return 0;
}

static int mt6360_ldo_write_device(struct i2c_client *i2c, u32 addr,
				   int len, const void *src)
{
	struct mt6360_ldo_info *mli = i2c_get_clientdata(i2c);
	u8 chunk[8] = {0};

	if ((addr & 0xc0) != 0 || len > 4 || len <= 0) {
		dev_err(&i2c->dev, "not support addr [%x], len [%d]\n",
			addr, len);
		return -EINVAL;
	}
	chunk[0] = (i2c->addr & 0x7f) << 1;
	chunk[1] = (addr & 0x3f) | ((u32)(len - 1) << 6);
	memcpy(chunk + 2, src, len);
	chunk[2 + len] = crc8(mli->crc8_table, chunk, 2 + len, 0);
	/*
	 * len + 2 -- payload, CRC and one trailing byte.  The part expects the
	 * extra byte; 4.19 sent it too.
	 */
	return i2c_smbus_write_i2c_block_data(i2c, chunk[1], len + 2, chunk + 2);
}

static int mt6360_ldo_reg_read(struct mt6360_ldo_info *mli, u8 addr)
{
	u8 data = 0;
	int ret;

	mutex_lock(&mli->io_lock);
	ret = mt6360_ldo_read_device(mli->i2c, addr, 1, &data);
	mutex_unlock(&mli->io_lock);
	mt_dbg(mli->dev, "%s: reg[%02x] = %02x (%d)\n", __func__,
	       addr, data, ret);
	return ret < 0 ? ret : data;
}

static int mt6360_ldo_reg_update_bits(struct mt6360_ldo_info *mli, u8 addr,
				      u8 mask, u8 data)
{
	u8 org = 0;
	int ret;

	mt_dbg(mli->dev, "%s: reg[%02x] mask[%02x] data[%02x]\n", __func__,
	       addr, mask, data);
	mutex_lock(&mli->io_lock);
	ret = mt6360_ldo_read_device(mli->i2c, addr, 1, &org);
	if (ret < 0)
		goto out;
	org &= ~mask;
	org |= (data & mask);
	ret = mt6360_ldo_write_device(mli->i2c, addr, 1, &org);
out:
	mutex_unlock(&mli->io_lock);
	return ret;
}

static int mt6360_ldo_enable(struct regulator_dev *rdev)
{
	struct mt6360_ldo_info *mli = rdev_get_drvdata(rdev);
	const struct regulator_desc *desc = rdev->desc;
	int id = rdev_get_id(rdev), ret;

	mt_dbg(&rdev->dev, "%s, id = %d\n", __func__, id);

	/* Enable SDCARD_DET before LDO5 enables. */
	if (id == MT6360_LDO_LDO5 && mli->sdcard_det_en) {
		ret = mt6360_ldo_reg_update_bits(mli, MT6360_LDO_LDO5_CTRL0,
						 0x80,
						 mli->sdcard_hlact ? 0xff : 0);
		if (ret < 0) {
			dev_info(&rdev->dev, "%s: sdcard_hlact fail (%d)\n",
				 __func__, ret);
			return ret;
		}
		ret = mt6360_ldo_reg_update_bits(mli, MT6360_LDO_LDO5_CTRL0,
						 0x40, 0xff);
		if (ret < 0) {
			dev_info(&rdev->dev, "%s: en sdcard_det fail (%d)\n",
				 __func__, ret);
			return ret;
		}
	}

	ret = mt6360_ldo_reg_update_bits(mli, desc->enable_reg,
					 desc->enable_mask, 0xff);
	if (ret < 0)
		dev_info(&rdev->dev, "%s: fail (%d)\n", __func__, ret);
	return ret;
}

static int mt6360_ldo_disable(struct regulator_dev *rdev)
{
	struct mt6360_ldo_info *mli = rdev_get_drvdata(rdev);
	const struct regulator_desc *desc = rdev->desc;
	int id = rdev_get_id(rdev), ret;

	mt_dbg(&rdev->dev, "%s, id = %d\n", __func__, id);
	ret = mt6360_ldo_reg_update_bits(mli, desc->enable_reg,
					 desc->enable_mask, 0);
	if (ret < 0) {
		dev_err(&rdev->dev, "%s: fail (%d)\n", __func__, ret);
		return ret;
	}
	/* when LDO5 disable, disable SDCARD_DET */
	if (id == MT6360_LDO_LDO5 && mli->sdcard_det_en) {
		ret = mt6360_ldo_reg_update_bits(mli, MT6360_LDO_LDO5_CTRL0,
						 0x40, 0);
		if (ret < 0) {
			dev_err(&rdev->dev, "%s: di sdcard_det fail (%d)\n",
				__func__, ret);
			return ret;
		}
	}
	return 0;
}

static int mt6360_ldo_is_enabled(struct regulator_dev *rdev)
{
	struct mt6360_ldo_info *mli = rdev_get_drvdata(rdev);
	const struct mt6360_regulator_desc *desc =
		(const struct mt6360_regulator_desc *)rdev->desc;
	int ret;

	ret = mt6360_ldo_reg_read(mli, desc->enst_reg);
	if (ret < 0)
		return ret;
	return ((u8)ret & desc->enst_mask) ? 1 : 0;
}

static int mt6360_ldo_set_voltage_sel(struct regulator_dev *rdev,
				      unsigned int sel)
{
	struct mt6360_ldo_info *mli = rdev_get_drvdata(rdev);
	const struct regulator_desc *desc = rdev->desc;
	u32 shift = ffs(desc->vsel_mask) - 1;
	int ret;

	mt_dbg(&rdev->dev, "%s, sel %d\n", __func__, sel);
	ret = mt6360_ldo_reg_update_bits(mli, desc->vsel_reg,
					 desc->vsel_mask, sel << shift);
	if (ret < 0)
		dev_err(&rdev->dev, "%s: fail (%d)\n", __func__, ret);
	return ret;
}

static int mt6360_ldo_get_voltage_sel(struct regulator_dev *rdev)
{
	struct mt6360_ldo_info *mli = rdev_get_drvdata(rdev);
	const struct regulator_desc *desc = rdev->desc;
	u32 shift = ffs(desc->vsel_mask) - 1, sel;
	int ret;

	ret = mt6360_ldo_reg_read(mli, desc->vsel_reg);
	if (ret < 0)
		return ret;
	sel = ret;
	sel &= desc->vsel_mask;
	sel >>= shift;
	return sel;
}

static int mt6360_ldo_set_mode(struct regulator_dev *rdev, unsigned int mode)
{
	struct mt6360_ldo_info *mli = rdev_get_drvdata(rdev);
	const struct mt6360_regulator_desc *desc =
		(const struct mt6360_regulator_desc *)rdev->desc;
	u32 shift = ffs(desc->mode_mask) - 1;
	int ret;
	u8 val;

	mt_dbg(&rdev->dev, "%s, mode = %d\n", __func__, mode);
	if (!mode)
		return -EINVAL;
	switch (1 << ((u8)(ffs(mode) - 1))) {
	case REGULATOR_MODE_NORMAL:
		val = 0;
		break;
	case REGULATOR_MODE_IDLE:
		val = 2;
		break;
	case REGULATOR_MODE_STANDBY:
		val = 1;
		break;
	default:
		return -ENOTSUPP;
	}
	ret = mt6360_ldo_reg_update_bits(mli, desc->mode_reg,
					 desc->mode_mask, val << shift);
	if (ret < 0)
		dev_err(&rdev->dev, "%s: fail (%d)\n", __func__, ret);
	return ret;
}

static unsigned int mt6360_ldo_get_mode(struct regulator_dev *rdev)
{
	struct mt6360_ldo_info *mli = rdev_get_drvdata(rdev);
	const struct mt6360_regulator_desc *desc =
		(const struct mt6360_regulator_desc *)rdev->desc;
	int shift = ffs(desc->moder_mask) - 1;
	int ret;

	ret = mt6360_ldo_reg_read(mli, desc->moder_reg);
	if (ret < 0)
		return ret;
	ret &= desc->moder_mask;
	ret = (u8)ret >> shift;
	switch (ret) {
	case 0:
		return REGULATOR_MODE_NORMAL;
	case 2:
		return REGULATOR_MODE_IDLE;
	case 3:
		return REGULATOR_MODE_STANDBY;
	}
	return -EINVAL;
}

static const struct regulator_ops mt6360_ldo_regulator_ops = {
	.list_voltage = regulator_list_voltage_linear_range,
	.enable = mt6360_ldo_enable,
	.disable = mt6360_ldo_disable,
	.is_enabled = mt6360_ldo_is_enabled,
	.set_voltage_sel = mt6360_ldo_set_voltage_sel,
	.get_voltage_sel = mt6360_ldo_get_voltage_sel,
	.set_mode = mt6360_ldo_set_mode,
	.get_mode = mt6360_ldo_get_mode,
};

static const struct linear_range ldo_volt_ranges1[] = {
	REGULATOR_LINEAR_RANGE(1200000, 0x00, 0x09, 10000),
	REGULATOR_LINEAR_RANGE(1300000, 0x0a, 0x10, 0),
	REGULATOR_LINEAR_RANGE(1310000, 0x11, 0x19, 10000),
	REGULATOR_LINEAR_RANGE(1400000, 0x1a, 0x1f, 0),
	REGULATOR_LINEAR_RANGE(1500000, 0x20, 0x29, 10000),
	REGULATOR_LINEAR_RANGE(1600000, 0x2a, 0x2f, 0),
	REGULATOR_LINEAR_RANGE(1700000, 0x30, 0x39, 10000),
	REGULATOR_LINEAR_RANGE(1800000, 0x3a, 0x40, 0),
	REGULATOR_LINEAR_RANGE(1810000, 0x41, 0x49, 10000),
	REGULATOR_LINEAR_RANGE(1900000, 0x4a, 0x4f, 0),
	REGULATOR_LINEAR_RANGE(2000000, 0x50, 0x59, 10000),
	REGULATOR_LINEAR_RANGE(2100000, 0x5a, 0x60, 0),
	REGULATOR_LINEAR_RANGE(2110000, 0x61, 0x69, 10000),
	REGULATOR_LINEAR_RANGE(2200000, 0x6a, 0x70, 0),
	REGULATOR_LINEAR_RANGE(2210000, 0x71, 0x79, 10000),
	REGULATOR_LINEAR_RANGE(2300000, 0x7a, 0x7f, 0),
	REGULATOR_LINEAR_RANGE(2700000, 0x80, 0x89, 10000),
	REGULATOR_LINEAR_RANGE(2800000, 0x8a, 0x90, 0),
	REGULATOR_LINEAR_RANGE(2810000, 0x91, 0x99, 10000),
	REGULATOR_LINEAR_RANGE(2900000, 0x9a, 0xa0, 0),
	REGULATOR_LINEAR_RANGE(2910000, 0xa1, 0xa9, 10000),
	REGULATOR_LINEAR_RANGE(3000000, 0xaa, 0xb0, 0),
	REGULATOR_LINEAR_RANGE(3010000, 0xb1, 0xb9, 10000),
	REGULATOR_LINEAR_RANGE(3100000, 0xba, 0xc0, 0),
	REGULATOR_LINEAR_RANGE(3110000, 0xc1, 0xc9, 10000),
	REGULATOR_LINEAR_RANGE(3200000, 0xca, 0xcf, 0),
	REGULATOR_LINEAR_RANGE(3300000, 0xd0, 0xd9, 10000),
	REGULATOR_LINEAR_RANGE(3400000, 0xda, 0xe0, 0),
	REGULATOR_LINEAR_RANGE(3410000, 0xe1, 0xe9, 10000),
	REGULATOR_LINEAR_RANGE(3500000, 0xea, 0xf0, 0),
	REGULATOR_LINEAR_RANGE(3510000, 0xf1, 0xf9, 10000),
	REGULATOR_LINEAR_RANGE(3600000, 0xfa, 0xff, 0),
};

static const struct linear_range ldo_volt_ranges2[] = {
	REGULATOR_LINEAR_RANGE(2700000, 0x00, 0x09, 10000),
	REGULATOR_LINEAR_RANGE(2800000, 0x0a, 0x10, 0),
	REGULATOR_LINEAR_RANGE(2810000, 0x11, 0x19, 10000),
	REGULATOR_LINEAR_RANGE(2900000, 0x1a, 0x20, 0),
	REGULATOR_LINEAR_RANGE(2910000, 0x21, 0x29, 10000),
	REGULATOR_LINEAR_RANGE(3000000, 0x2a, 0x30, 0),
	REGULATOR_LINEAR_RANGE(3010000, 0x31, 0x39, 10000),
	REGULATOR_LINEAR_RANGE(3100000, 0x3a, 0x40, 0),
	REGULATOR_LINEAR_RANGE(3110000, 0x41, 0x49, 10000),
	REGULATOR_LINEAR_RANGE(3200000, 0x4a, 0x4f, 0),
	REGULATOR_LINEAR_RANGE(3300000, 0x50, 0x59, 10000),
	REGULATOR_LINEAR_RANGE(3400000, 0x5a, 0x60, 0),
	REGULATOR_LINEAR_RANGE(3410000, 0x61, 0x69, 10000),
	REGULATOR_LINEAR_RANGE(3500000, 0x6a, 0x70, 0),
	REGULATOR_LINEAR_RANGE(3510000, 0x71, 0x79, 10000),
	REGULATOR_LINEAR_RANGE(3600000, 0x7a, 0x7f, 0),
};

#define LDO1_VOLT_RANGES	(ldo_volt_ranges1)
#define LDO1_N_VOLT_RANGES	(ARRAY_SIZE(ldo_volt_ranges1))
#define LDO1_VOUT_CNT		(256)
#define LDO2_VOLT_RANGES	(ldo_volt_ranges1)
#define LDO2_N_VOLT_RANGES	(ARRAY_SIZE(ldo_volt_ranges1))
#define LDO2_VOUT_CNT		(256)
#define LDO3_VOLT_RANGES	(ldo_volt_ranges1)
#define LDO3_N_VOLT_RANGES	(ARRAY_SIZE(ldo_volt_ranges1))
#define LDO3_VOUT_CNT		(256)
#define LDO5_VOLT_RANGES	(ldo_volt_ranges2)
#define LDO5_N_VOLT_RANGES	(ARRAY_SIZE(ldo_volt_ranges2))
#define LDO5_VOUT_CNT		(128)

/*
 * of_match is the uppercase "LDOn" the DTB's ldo1..ldo5 child nodes carry in
 * their (deprecated but still honoured) regulator-compatible property, not the
 * node names -- see regulator_of_get_init_node().
 */
#define MT6360_LDO_DESC(_name, vreg, vmask, enreg, enmask, enstreg,	\
			enstmask, modereg, modemask, moderreg, modermask, \
			offon_delay)					\
{									\
	.desc = {							\
		.name = #_name,						\
		.id =  MT6360_LDO_##_name,				\
		.owner = THIS_MODULE,					\
		.ops = &mt6360_ldo_regulator_ops,			\
		.of_match = of_match_ptr(#_name),			\
		.linear_ranges = _name##_VOLT_RANGES,			\
		.n_linear_ranges = _name##_N_VOLT_RANGES,		\
		.n_voltages = _name##_VOUT_CNT,				\
		.type = REGULATOR_VOLTAGE,				\
		.vsel_reg = vreg,					\
		.vsel_mask = vmask,					\
		.enable_reg = enreg,					\
		.enable_mask = enmask,					\
		.off_on_delay = offon_delay,				\
	},								\
	.enst_reg = enstreg,						\
	.enst_mask = enstmask,						\
	.mode_reg = modereg,						\
	.mode_mask = modemask,						\
	.moder_reg = moderreg,						\
	.moder_mask = modermask,					\
}

/*
 * Register writes this driver can make at probe time, for the record:
 *
 * regulator_register() -> machine_constraints_voltage() reads each rail's
 * current vsel and only writes when it falls outside the DT's
 * [min,max].  LDO1/2/3 declare the full 1.2-3.6 V hardware range, so they can
 * never need one.  LDO5 declares min == max == 3.3 V, so if VMCH happens to
 * sit elsewhere its vsel (0x0f) gets written -- inert, since VMCH is the SD
 * card rail, is disabled, and has no consumer in this DTB.  Nothing here
 * enables a rail; the touchscreen's regulator_enable() does that.
 */
static const struct mt6360_regulator_desc mt6360_ldo_descs[] = {
	MT6360_LDO_DESC(LDO1, 0x1b, 0xff, 0x17, 0x40,
			0x17, 0x04, 0x17, 0x30, 0x17, 0x03, 0),
	MT6360_LDO_DESC(LDO2, 0x15, 0xff, 0x11, 0x40,
			0x11, 0x04, 0x11, 0x30, 0x11, 0x03, 0),
	MT6360_LDO_DESC(LDO3, 0x09, 0xff, 0x05, 0x40,
			0x05, 0x04, 0x05, 0x30, 0x05, 0x03, 120),
	MT6360_LDO_DESC(LDO5, 0x0f, 0x7f, 0x0b, 0x40,
			0x0b, 0x04, 0x0b, 0x30, 0x0b, 0x03, 120),
};

/*
 * The only access outside slave 0x64: read PMU register 0x00 (slave 0x34) to
 * confirm an MT6360 is really there.  Plain SMBus byte read, no CRC, no write.
 */
static int mt6360_pmic_chip_id_check(struct i2c_client *i2c)
{
	struct i2c_client pmu_client;
	int ret;

	memcpy(&pmu_client, i2c, sizeof(*i2c));
	pmu_client.addr = 0x34;
	ret = i2c_smbus_read_byte_data(&pmu_client, 0x00);
	if (ret < 0)
		return ret;
	if (((u8)ret & 0xf0) != 0x50)
		return -ENODEV;
	return ret & 0x0f;
}

/*
 * The i2c client node (subpmic_ldo@64) carries only reg/compatible; the
 * regulator descriptions live in a detached "mt6360_ldo_dts" node under the
 * same i2c bus.  4.19 re-pointed the device at it before registering, and the
 * DTB's aliases (mt_pmic_vtp_ldo_reg = ".../mt6360_ldo_dts/ldo2") assume that
 * layout, so do the same.
 */
static void mt6360_config_of_node(struct device *dev, const char *name)
{
	struct device_node *np;

	np = of_find_node_by_name(NULL, name);
	if (np) {
		dev_info(dev, "using %s for the regulator nodes\n", name);
		dev->of_node = np;
	}
}

static int mt6360_ldo_i2c_probe(struct i2c_client *client)
{
	struct mt6360_ldo_info *mli;
	struct regulator_config config = {};
	struct regulation_constraints *constraints;
	int i, ret;

	ret = mt6360_pmic_chip_id_check(client);
	if (ret < 0) {
		dev_err(&client->dev, "no device found\n");
		return ret;
	}

	mli = devm_kzalloc(&client->dev, sizeof(*mli), GFP_KERNEL);
	if (!mli)
		return -ENOMEM;
	mli->i2c = client;
	mli->dev = &client->dev;
	mli->chip_rev = (u8)ret;
	/* 4.19's def_platform_data */
	mli->sdcard_det_en = true;
	mli->sdcard_hlact = true;
	crc8_populate_msb(mli->crc8_table, 0x7);
	mutex_init(&mli->io_lock);
	i2c_set_clientdata(client, mli);
	dev_info(&client->dev, "chip_rev [%02x]\n", mli->chip_rev);

	if (client->dev.of_node)
		mt6360_config_of_node(&client->dev, "mt6360_ldo_dts");

	config.dev = &client->dev;
	config.driver_data = mli;
	for (i = 0; i < ARRAY_SIZE(mt6360_ldo_descs); i++) {
		mli->rdev[i] = devm_regulator_register(&client->dev,
						&mt6360_ldo_descs[i].desc,
						&config);
		if (IS_ERR(mli->rdev[i])) {
			ret = PTR_ERR(mli->rdev[i]);
			dev_err(&client->dev, "failed to register %s: %d\n",
				mt6360_ldo_descs[i].desc.name, ret);
			return ret;
		}
		/* allow change mode */
		constraints = mli->rdev[i]->constraints;
		constraints->valid_ops_mask |= REGULATOR_CHANGE_MODE;
		constraints->valid_modes_mask = REGULATOR_MODE_NORMAL |
						REGULATOR_MODE_IDLE |
						REGULATOR_MODE_STANDBY;
	}

	dev_info(&client->dev, "%s: successfully probed\n", __func__);
	return 0;
}

/*
 * No .remove: everything is devm-managed.  4.19 destroyed io_lock here, but
 * the devm_regulator_register() releases run *after* remove() returns, so
 * tearing the lock down at that point would be the wrong order if any of them
 * ever reached back into the accessors.  A mutex needs no explicit teardown.
 */

static const struct of_device_id mt6360_ldo_of_id[] = {
	{ .compatible = "mediatek,mt6360_ldo", },
	{ .compatible = "mediatek,subpmic_ldo", },
	{}
};
MODULE_DEVICE_TABLE(of, mt6360_ldo_of_id);

static const struct i2c_device_id mt6360_ldo_i2c_id[] = {
	{ "mt6360_ldo", 0 },
	{}
};
MODULE_DEVICE_TABLE(i2c, mt6360_ldo_i2c_id);

static struct i2c_driver mt6360_ldo_i2c_driver = {
	.driver = {
		.name = "mt6360_ldo",
		.of_match_table = mt6360_ldo_of_id,
	},
	.probe = mt6360_ldo_i2c_probe,
	.id_table = mt6360_ldo_i2c_id,
};
module_i2c_driver(mt6360_ldo_i2c_driver);

MODULE_AUTHOR("CY_Huang <cy_huang@richtek.com>");
MODULE_DESCRIPTION("MT6360 LDO bank regulator driver (legacy binding)");
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0.0");
