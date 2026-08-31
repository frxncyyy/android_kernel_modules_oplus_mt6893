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
 * With reg= it instead does an SMBus byte read of reg..reg+count-1 on each
 * address, which is a write of the register index followed by a read.  That
 * writes only the index byte, never a data byte, so no device state changes.
 *
 *   insmod i2cscan.ko bus=5 addrs=0x34 reg=0 count=16
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

static int reg = -1;
module_param(reg, int, 0);
MODULE_PARM_DESC(reg, "first register to read, or -1 for a bare address probe");

static int count = 1;
module_param(count, int, 0);
MODULE_PARM_DESC(count, "number of consecutive registers to read");

static void i2cscan_read_regs(struct i2c_adapter *adap, unsigned short addr)
{
	struct i2c_client *client;
	int i;

	/* struct i2c_client embeds a struct device -- too big for the stack */
	client = kzalloc(sizeof(*client), GFP_KERNEL);
	if (!client)
		return;
	client->adapter = adap;
	client->addr = addr;

	for (i = 0; i < count; i++) {
		int ret = i2c_smbus_read_byte_data(client, reg + i);

		if (ret < 0)
			pr_info("i2cscan: 0x%02x reg 0x%02x -> error %d\n",
				addr, reg + i, ret);
		else
			pr_info("i2cscan: 0x%02x reg 0x%02x = 0x%02x\n",
				addr, reg + i, ret);
	}
	kfree(client);
}

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

		if (reg >= 0) {
			i2cscan_read_regs(adap, addrs[i]);
			continue;
		}

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
