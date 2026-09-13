// SPDX-License-Identifier: GPL-2.0-only
/* Nord 2 BLP861 telemetry. Charger configuration is intentionally read-only. */
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/power_supply.h>
#include <linux/workqueue.h>

struct nord2_power {
	struct i2c_client *client;
	struct power_supply *psy;
	struct delayed_work poll;
	bool gauge;
};

static int nord2_word(struct nord2_power *p, u8 reg)
{
	return i2c_smbus_read_word_data(p->client, reg);
}

static int nord2_byte(struct nord2_power *p, u8 reg)
{
	return i2c_smbus_read_byte_data(p->client, reg);
}

/* MP2650 REG13 ACOK is active high. The old vendor header reverses its labels. */
static int nord2_charger_status(int status)
{
	if (!(status & BIT(1)))
		return POWER_SUPPLY_STATUS_DISCHARGING;
	switch ((status >> 2) & 3) {
	case 1:
	case 2:
		return POWER_SUPPLY_STATUS_CHARGING;
	case 3:
		return POWER_SUPPLY_STATUS_FULL;
	default:
		return POWER_SUPPLY_STATUS_NOT_CHARGING;
	}
}

static int nord2_charger_health(int fault)
{
	if (fault & BIT(3))
		return POWER_SUPPLY_HEALTH_OVERVOLTAGE;
	if ((fault & 7) == 4 || (fault & 0x30) == 0x20)
		return POWER_SUPPLY_HEALTH_OVERHEAT;
	if ((fault & 7) == 1)
		return POWER_SUPPLY_HEALTH_COLD;
	if (fault & BIT(7))
		return POWER_SUPPLY_HEALTH_WATCHDOG_TIMER_EXPIRE;
	if ((fault & 0x30) == 0x30)
		return POWER_SUPPLY_HEALTH_SAFETY_TIMER_EXPIRE;
	if ((fault & 0x70) || (fault & 7) > 4)
		return POWER_SUPPLY_HEALTH_UNSPEC_FAILURE;
	return POWER_SUPPLY_HEALTH_GOOD;
}

static int nord2_usb_get(struct power_supply *psy,
		enum power_supply_property prop, union power_supply_propval *val)
{
	struct nord2_power *p = power_supply_get_drvdata(psy);
	int data;

	switch (prop) {
	case POWER_SUPPLY_PROP_ONLINE:
	case POWER_SUPPLY_PROP_STATUS:
		data = nord2_byte(p, 0x13);
		if (data < 0)
			return data;
		val->intval = prop == POWER_SUPPLY_PROP_ONLINE ?
			!!(data & BIT(1)) : nord2_charger_status(data);
		return 0;
	case POWER_SUPPLY_PROP_HEALTH:
		data = nord2_byte(p, 0x14);
		if (data < 0)
			return data;
		val->intval = nord2_charger_health(data);
		return 0;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		data = nord2_byte(p, 0x00);
		if (data < 0)
			return data;
		val->intval = (data & 0x7f) * 50000;
		return 0;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = "MP2650";
		return 0;
	default:
		return -EINVAL;
	}
}

static int nord2_read_charger(enum power_supply_property prop,
		union power_supply_propval *val)
{
	struct power_supply *charger = power_supply_get_by_name("usb");
	int ret;

	if (!charger)
		return -ENODEV;
	ret = power_supply_get_property(charger, prop, val);
	power_supply_put(charger);
	return ret;
}

static int nord2_battery_get(struct power_supply *psy,
		enum power_supply_property prop, union power_supply_propval *val)
{
	struct nord2_power *p = power_supply_get_drvdata(psy);
	int data, reg;

	switch (prop) {
	case POWER_SUPPLY_PROP_STATUS:
		return nord2_read_charger(prop, val);
	case POWER_SUPPLY_PROP_HEALTH:
		/* Never report a healthy battery when its temperature cannot be read. */
		data = nord2_word(p, 0x06);
		if (data < 0)
			return data;
		if (data < 2331 || data > 3581)
			return -ENODATA;
		if (data >= 3331) {
			val->intval = POWER_SUPPLY_HEALTH_OVERHEAT;
			return 0;
		}
		return nord2_read_charger(prop, val);
	case POWER_SUPPLY_PROP_PRESENT:
		data = nord2_word(p, 0x08);
		if (data < 0)
			return data;
		if (!data || data == 0xffff)
			return -ENODATA;
		val->intval = 1;
		return 0;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		return 0;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = "BLP861 / ZY0603";
		return 0;
	case POWER_SUPPLY_PROP_CAPACITY:
		reg = 0x2c;
		break;
	case POWER_SUPPLY_PROP_TEMP:
		reg = 0x06;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		reg = 0x08;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		/* ZY0603 uses BQ28Z610 instantaneous current, not BQ27541's 0x14. */
		reg = 0x0c;
		break;
	case POWER_SUPPLY_PROP_CHARGE_COUNTER:
	case POWER_SUPPLY_PROP_CHARGE_NOW:
		reg = 0x10;
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL:
		reg = 0x12;
		break;
	default:
		return -EINVAL;
	}
	data = nord2_word(p, reg);
	if (data < 0)
		return data;
	switch (prop) {
	case POWER_SUPPLY_PROP_CAPACITY:
		if (data > 100)
			return -ENODATA;
		val->intval = data;
		break;
	case POWER_SUPPLY_PROP_TEMP:
		if (data < 2331 || data > 3581)
			return -ENODATA;
		val->intval = data - 2731;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		/* Linux uses microamps, positive while charging. */
		val->intval = (s16)data * 1000;
		break;
	default:
		if (data == 0xffff)
			return -ENODATA;
		/* Real 2S pack voltage/capacity: uV and uAh, without cell scaling. */
		val->intval = data * 1000;
		break;
	}
	return 0;
}

static enum power_supply_property nord2_battery_props[] = {
	POWER_SUPPLY_PROP_STATUS, POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_PRESENT, POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CAPACITY, POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_VOLTAGE_NOW, POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CHARGE_COUNTER,
	POWER_SUPPLY_PROP_CHARGE_NOW, POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_MODEL_NAME,
};

static enum power_supply_property nord2_usb_props[] = {
	POWER_SUPPLY_PROP_ONLINE, POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_HEALTH, POWER_SUPPLY_PROP_CURRENT_MAX,
	POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT, POWER_SUPPLY_PROP_MODEL_NAME,
};

static const struct power_supply_desc nord2_battery_desc = {
	.name = "battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = nord2_battery_props,
	.num_properties = ARRAY_SIZE(nord2_battery_props),
	.get_property = nord2_battery_get,
};

static const struct power_supply_desc nord2_usb_desc = {
	.name = "usb",
	.type = POWER_SUPPLY_TYPE_USB,
	.properties = nord2_usb_props,
	.num_properties = ARRAY_SIZE(nord2_usb_props),
	.get_property = nord2_usb_get,
};

static void nord2_poll(struct work_struct *work)
{
	struct nord2_power *p = container_of(to_delayed_work(work),
			struct nord2_power, poll);

	power_supply_changed(p->psy);
	schedule_delayed_work(&p->poll, 5 * HZ);
}

static void nord2_stop(void *data)
{
	struct nord2_power *p = data;

	cancel_delayed_work_sync(&p->poll);
}

static int nord2_power_probe(struct i2c_client *client)
{
	const struct power_supply_desc *desc = i2c_get_match_data(client);
	struct power_supply_config config = {};
	struct nord2_power *p;
	int ret;

	/* The vendor DTBO creates these generic nodes on several phone models. */
	if (!of_machine_is_compatible("oneplus,denniz"))
		return -ENODEV;
	if (!i2c_check_functionality(client->adapter,
			I2C_FUNC_SMBUS_BYTE_DATA | I2C_FUNC_SMBUS_WORD_DATA))
		return -EOPNOTSUPP;
	p = devm_kzalloc(&client->dev, sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	p->client = client;
	p->gauge = desc == &nord2_battery_desc;
	i2c_set_clientdata(client, p);
	if (p->gauge) {
		/* Device-type query only: no reset, unseal or calibration commands. */
		ret = i2c_smbus_write_word_data(client, 0x00, 0x0001);
		if (ret < 0)
			return ret;
		usleep_range(1000, 1500);
		ret = nord2_word(p, 0x00);
		if (ret < 0)
			return ret;
		if (ret != 0xa5ff)
			return dev_err_probe(&client->dev, -ENODEV,
					"Expected ZY0603, got device type %#x\n", ret);
	} else {
		ret = nord2_byte(p, 0x15);
		if (ret < 0)
			return ret;
		if (ret == 0xff)
			return -ENODEV;
	}
	config.drv_data = p;
	config.of_node = client->dev.of_node;
	p->psy = devm_power_supply_register(&client->dev, desc, &config);
	if (IS_ERR(p->psy))
		return PTR_ERR(p->psy);
	INIT_DELAYED_WORK(&p->poll, nord2_poll);
	ret = devm_add_action_or_reset(&client->dev, nord2_stop, p);
	if (ret)
		return ret;
	schedule_delayed_work(&p->poll, HZ);
	dev_info(&client->dev, "%s telemetry registered; charger settings unchanged\n",
		 desc->name);
	return 0;
}

static int nord2_power_suspend(struct device *dev)
{
	nord2_stop(dev_get_drvdata(dev));
	return 0;
}

static int nord2_power_resume(struct device *dev)
{
	struct nord2_power *p = dev_get_drvdata(dev);

	schedule_delayed_work(&p->poll, 0);
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(nord2_power_pm,
			nord2_power_suspend, nord2_power_resume);

static const struct of_device_id nord2_power_of_match[] = {
	{ .compatible = "oplus,bq27541-battery", .data = &nord2_battery_desc },
	{ .compatible = "oplus,mp2650-charger", .data = &nord2_usb_desc },
	{}
};
MODULE_DEVICE_TABLE(of, nord2_power_of_match);

static struct i2c_driver nord2_power_driver = {
	.driver = {
		.name = "nord2-power",
		.of_match_table = nord2_power_of_match,
		.pm = pm_sleep_ptr(&nord2_power_pm),
	},
	.probe = nord2_power_probe,
};
module_i2c_driver(nord2_power_driver);

MODULE_DESCRIPTION("Nord 2 ZY0603 battery and MP2650 status telemetry");
MODULE_LICENSE("GPL");
