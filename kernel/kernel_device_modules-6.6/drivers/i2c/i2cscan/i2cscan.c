// SPDX-License-Identifier: GPL-2.0
/*
 * Throwaway bring-up probe: does an i2c bus ACK an address at all?
 *
 * Read-only by construction.  For each address it issues one I2C_M_RD message
 * of a single byte, which is what i2cdetect does on a bus where a plain
 * address-only write would be ambiguous.  Nothing is ever written to any
 * device, so this is safe to run against a PMIC whose register state survives
 * a reboot -- it cannot change a rail, a charger setting or a Type-C role.
 *
 * The kernel this runs on has CONFIG_I2C_CHARDEV=n, so there is no /dev/i2c-N
 * to do this from userspace with.
 *
 *   insmod i2cscan.ko bus=5 addrs=0x1a,0x34,0x4e,0x64
 *
 * Always fails to load (-ENODEV) once it has printed its results: there is
 * nothing to keep resident.
 */

#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/module.h>

static int bus = -1;
module_param(bus, int, 0);
MODULE_PARM_DESC(bus, "i2c adapter number to scan");

static unsigned short addrs[16];
static int addrs_count;
module_param_array(addrs, ushort, &addrs_count, 0);
MODULE_PARM_DESC(addrs, "7-bit addresses to probe");

static int __init i2cscan_init(void)
{
	struct i2c_adapter *adap;
	int i;

	if (bus < 0 || addrs_count <= 0) {
		pr_err("i2cscan: need bus= and addrs=\n");
		return -EINVAL;
	}

	adap = i2c_get_adapter(bus);
	if (!adap) {
		pr_err("i2cscan: no adapter %d\n", bus);
		return -ENODEV;
	}

	pr_info("i2cscan: bus %d (%s)\n", bus, adap->name);

	for (i = 0; i < addrs_count; i++) {
		struct i2c_msg msg;
		u8 byte = 0;
		int ret;

		msg.addr = addrs[i];
		msg.flags = I2C_M_RD;
		msg.len = 1;
		msg.buf = &byte;

		ret = i2c_transfer(adap, &msg, 1);
		if (ret == 1)
			pr_info("i2cscan: 0x%02x ACK, read 0x%02x\n",
				addrs[i], byte);
		else
			pr_info("i2cscan: 0x%02x no ACK, ret %d\n",
				addrs[i], ret);
	}

	i2c_put_adapter(adap);
	return -ENODEV;
}

module_init(i2cscan_init);
MODULE_DESCRIPTION("read-only i2c address probe for bring-up");
MODULE_LICENSE("GPL");
