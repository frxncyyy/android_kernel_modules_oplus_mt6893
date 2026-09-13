#!/usr/bin/env python3
"""Reject incomplete v2 bindings before either charging probe can initialize hardware."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[3]
drivers = (
    ("mp2650", "charger_ic/oplus_hal_mp2650.c", "\tchg_ic = devm_kzalloc"),
    ("bq27541", "gauge_ic/oplus_hal_bq27541.c", "\tif (bq27541_need_level_shift"),
)
harness = r'''
#include <assert.h>
#include <errno.h>
#include <string.h>
struct device_node { unsigned int present; } node;
struct device { struct device_node *of_node; } dev = { &node };
struct i2c_client { struct device dev; } client;
struct oplus_chg_ic_cfg { int unused; };
enum oplus_chg_ic_type { TEST_BUCK=1 };
static int initialized, reads, errors;
static int of_property_read_u32(struct device_node *node, const char *name, void *out)
{
    unsigned int bit=strcmp(name,"oplus,ic_type")==0 ? 1 : 2;
    reads++;
    if (!(node->present & bit)) return -EINVAL;
    *(unsigned int *)out=bit==1 ? TEST_BUCK : 0;
    return 0;
}
static int dev_err_probe(struct device *dev, int err, const char *message)
{
    (void)dev; (void)message; errors++; return err;
}
static int probe(struct i2c_client *client)
{
PREFIX
    initialized++;
    return 0;
}
int main(void)
{
    client.dev=dev;
    for (unsigned int properties=0; properties<4; properties++) {
        node.present=properties; initialized=reads=errors=0;
        int ret=probe(&client);
        assert(reads==((properties & 1) ? 2 : 1));
        assert(ret==(properties==3 ? 0 : -EINVAL));
        assert(initialized==(properties==3));
        assert(errors==(properties!=3));
    }
}
'''
with tempfile.TemporaryDirectory(prefix="nord2-charging-binding-") as directory:
    path = Path(directory)
    for name, relative, boundary in drivers:
        source = Path(sys.argv[1]) / (name + "-before-dt-validation.c") if len(sys.argv) > 1 else root / "vendor/oplus/kernel/charger/v2" / relative
        text = source.read_text()
        start = text.index("static int " + name + "_driver_probe(")
        start = text.index("\n{", start) + 2
        end = text.index(boundary, start)
        (path / "test.c").write_text(harness.replace("PREFIX", text[start:end]))
        subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Werror",
                        "-Wno-unused-variable", "-Wno-unused-function",
                        str(path / "test.c"), "-o", str(path / "test")], check=True)
        subprocess.run([str(path / "test")], check=True)
        print(f"PASS: {name} validates both required properties before initialization")
