#!/usr/bin/env python3
"""Reproduce synchronous component binding before TE pinctrl setup."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[3]
driver = root / "kernel/kernel_device_modules-6.6/drivers/gpu/drm/mediatek/mediatek_v2/mtk_drm_drv.c"
source = driver.read_text()
end = source.index("\n\tfor (i = 0 ; i < MAX_CRTC", source.index("static int mtk_drm_probe("))
start = source.rindex("\tplatform_set_drvdata(pdev, private);", 0, end)
harness = r'''
#include <assert.h>
struct private { int pinctrl_ready; } storage;
static int pinctrl_result, bind_result, bind_count;
static int mtk_drm_ops;
#define DDPINFO(...) do {} while (0)
static void platform_set_drvdata(void *dev, void *data) { (void)dev; (void)data; }
static int disp_dts_gpio_init(void *dev, struct private *private)
{
    (void)dev;
    if (!pinctrl_result) private->pinctrl_ready=1;
    return pinctrl_result;
}
static int component_master_add_with_match(void *dev, void *ops, void *match)
{
    (void)dev; (void)ops; (void)match;
    /* Binding immediately requests ESD IRQ and restores the TE mux. */
    assert(storage.pinctrl_ready);
    bind_count++;
    return bind_result;
}
static int probe(void)
{
    struct private *private=&storage;
    void *dev=0, *pdev=0, *match=0;
    int ret;
BLOCK
    return 0;
err_pm:
    return ret;
}
int main(void)
{
    int errors[]={0,-517,-19};
    for (unsigned int i=0;i<3;i++) for (unsigned int j=0;j<3;j++) {
        pinctrl_result=errors[i]; bind_result=errors[j];
        bind_count=storage.pinctrl_ready=0;
        assert(probe()==(pinctrl_result ? pinctrl_result : bind_result));
        assert(bind_count==(pinctrl_result==0));
    }
}
'''.replace("BLOCK", source[start:end])
with tempfile.TemporaryDirectory(prefix="nord2-display-bind-") as directory:
    path = Path(directory)
    (path / "test.c").write_text(harness)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Werror",
                    str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
print("PASS: synchronous binding has pinctrl; setup errors defer/abort before binding")
