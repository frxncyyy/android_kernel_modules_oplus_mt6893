#!/usr/bin/env python3
"""Run the actual power getters with injected I2C values and failures."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

source = Path(__file__).resolve().parents[3] / 'kernel/kernel_device_modules-6.6/drivers/power/supply/nord2-power.c'
text = source.read_text()
functions = text[text.index('static int nord2_word'):text.index('static enum power_supply_property nord2_battery_props')]
props = sorted(set(re.findall(r'POWER_SUPPLY_PROP_\w+', functions)))
constants = sorted(set(re.findall(r'POWER_SUPPLY_(?:STATUS|HEALTH|TECHNOLOGY)_\w+', functions)))
harness = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
typedef uint8_t u8;
typedef int16_t s16;
#define BIT(n) (1U << (n))
ENUMS
union power_supply_propval { int intval; const char *strval; };
struct i2c_client { int unused; };
struct nord2_power { struct i2c_client *client; } gauge, charger;
struct power_supply { struct nord2_power *data; } battery = { &gauge }, usb = { &charger };
static int words[256], bytes[256], bus_error, refs, charger_present = 1;
static struct nord2_power *power_supply_get_drvdata(struct power_supply *p) { return p->data; }
static int i2c_smbus_read_word_data(struct i2c_client *c, u8 reg) { (void)c; return bus_error ?: words[reg]; }
static int i2c_smbus_read_byte_data(struct i2c_client *c, u8 reg) { (void)c; return bus_error ?: bytes[reg]; }
static struct power_supply *power_supply_get_by_name(const char *name)
{ assert(!strcmp(name, "usb")); if (!charger_present) return NULL; refs++; return &usb; }
static void power_supply_put(struct power_supply *p) { assert(p == &usb); refs--; }
static int nord2_usb_get(struct power_supply *, enum power_supply_property, union power_supply_propval *);
static int power_supply_get_property(struct power_supply *p, enum power_supply_property prop, union power_supply_propval *v)
{ return nord2_usb_get(p, prop, v); }
FUNCTIONS
static int value(enum power_supply_property prop)
{ union power_supply_propval v = {0}; assert(nord2_battery_get(&battery, prop, &v) == 0); assert(refs == 0); return v.intval; }
int main(void)
{
    union power_supply_propval v;
    words[0x06] = 3065; words[0x08] = 8395; words[0x0c] = 157;
    words[0x10] = 1534; words[0x12] = 2023; words[0x2c] = 76;
    assert(value(POWER_SUPPLY_PROP_VOLTAGE_NOW) == 8395000);
    assert(value(POWER_SUPPLY_PROP_CURRENT_NOW) == 157000);
    words[0x0c] = (uint16_t)-321;
    assert(value(POWER_SUPPLY_PROP_CURRENT_NOW) == -321000);
    assert(value(POWER_SUPPLY_PROP_TEMP) == 334);
    assert(value(POWER_SUPPLY_PROP_CAPACITY) == 76);
    assert(value(POWER_SUPPLY_PROP_CHARGE_FULL) == 2023000);
    assert(value(POWER_SUPPLY_PROP_CHARGE_COUNTER) == 1534000);
    assert(value(POWER_SUPPLY_PROP_PRESENT) == 1);
    for (int charge = 0; charge < 4; charge++) {
        int expected[] = {POWER_SUPPLY_STATUS_NOT_CHARGING, POWER_SUPPLY_STATUS_CHARGING, POWER_SUPPLY_STATUS_CHARGING, POWER_SUPPLY_STATUS_FULL};
        bytes[0x13] = 2 | charge << 2;
        assert(value(POWER_SUPPLY_PROP_STATUS) == expected[charge]);
        assert(nord2_usb_get(&usb, POWER_SUPPLY_PROP_ONLINE, &v) == 0 && v.intval == 1);
        bytes[0x13] &= ~2;
        assert(value(POWER_SUPPLY_PROP_STATUS) == POWER_SUPPLY_STATUS_DISCHARGING);
        assert(nord2_usb_get(&usb, POWER_SUPPLY_PROP_ONLINE, &v) == 0 && v.intval == 0);
    }
    bytes[0] = 10;
    assert(nord2_usb_get(&usb, POWER_SUPPLY_PROP_CURRENT_MAX, &v) == 0 && v.intval == 500000);
    assert(value(POWER_SUPPLY_PROP_HEALTH) == POWER_SUPPLY_HEALTH_GOOD);
    int faults[] = {8,4,1,128,32,48,16,64,7};
    int health[] = {POWER_SUPPLY_HEALTH_OVERVOLTAGE, POWER_SUPPLY_HEALTH_OVERHEAT, POWER_SUPPLY_HEALTH_COLD, POWER_SUPPLY_HEALTH_WATCHDOG_TIMER_EXPIRE, POWER_SUPPLY_HEALTH_OVERHEAT, POWER_SUPPLY_HEALTH_SAFETY_TIMER_EXPIRE, POWER_SUPPLY_HEALTH_UNSPEC_FAILURE, POWER_SUPPLY_HEALTH_UNSPEC_FAILURE, POWER_SUPPLY_HEALTH_UNSPEC_FAILURE};
    for (unsigned i = 0; i < sizeof(faults)/sizeof(*faults); i++) {
        bytes[0x14] = faults[i]; assert(value(POWER_SUPPLY_PROP_HEALTH) == health[i]);
    }
    bytes[0x14] = 0; words[0x06] = 3331;
    assert(value(POWER_SUPPLY_PROP_HEALTH) == POWER_SUPPLY_HEALTH_OVERHEAT);
    words[0x06] = 0;
    assert(nord2_battery_get(&battery, POWER_SUPPLY_PROP_TEMP, &v) == -ENODATA);
    assert(nord2_battery_get(&battery, POWER_SUPPLY_PROP_HEALTH, &v) == -ENODATA);
    words[0x2c] = 101;
    assert(nord2_battery_get(&battery, POWER_SUPPLY_PROP_CAPACITY, &v) == -ENODATA);
    words[0x08] = 65535;
    assert(nord2_battery_get(&battery, POWER_SUPPLY_PROP_PRESENT, &v) == -ENODATA);
    assert(nord2_battery_get(&battery, POWER_SUPPLY_PROP_VOLTAGE_NOW, &v) == -ENODATA);
    enum power_supply_property measurements[] = { POWER_SUPPLY_PROP_TEMP, POWER_SUPPLY_PROP_VOLTAGE_NOW, POWER_SUPPLY_PROP_CURRENT_NOW, POWER_SUPPLY_PROP_CAPACITY, POWER_SUPPLY_PROP_PRESENT, POWER_SUPPLY_PROP_HEALTH, POWER_SUPPLY_PROP_STATUS, POWER_SUPPLY_PROP_CHARGE_COUNTER, POWER_SUPPLY_PROP_CHARGE_FULL };
    bus_error = -EREMOTEIO;
    for (unsigned i = 0; i < sizeof(measurements)/sizeof(*measurements); i++)
        assert(nord2_battery_get(&battery, measurements[i], &v) == -EREMOTEIO);
    assert(nord2_usb_get(&usb, POWER_SUPPLY_PROP_ONLINE, &v) == -EREMOTEIO);
    assert(nord2_usb_get(&usb, POWER_SUPPLY_PROP_CURRENT_MAX, &v) == -EREMOTEIO);
    assert(refs == 0);
    charger_present = 0;
    assert(nord2_battery_get(&battery, POWER_SUPPLY_PROP_STATUS, &v) == -ENODEV);
    assert(refs == 0);
}
'''
harness = harness.replace('ENUMS', 'enum power_supply_property {' + ','.join(props) + '};\nenum {' + ','.join(constants) + '};').replace('FUNCTIONS', functions)
with tempfile.TemporaryDirectory() as directory:
    d = Path(directory)
    (d/'test.c').write_text(harness)
    subprocess.run([os.environ.get('CC','cc'),'-std=gnu11','-Wall','-Werror',str(d/'test.c'),'-o',str(d/'test')],check=True)
    subprocess.run([str(d/'test')],check=True)
print('PASS: gauge units/sign/ranges, ACOK/charge states/faults, absent charger and I2C error propagation')
