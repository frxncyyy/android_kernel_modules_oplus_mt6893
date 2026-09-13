#!/usr/bin/env python3
"""Exercise the actual LK handoff decision with and without the header flag."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[3]
driver = root / "kernel/kernel_device_modules-6.6/drivers/gpu/drm/mediatek/mediatek_v2/mtk_drm_drv.c"
source = driver.read_text()
start = source.index("static bool mtk_drm_is_enable_from_lk(")
end = source.index("\nstatic bool mtk_atomic_skip_plane_update", start)
harness = r'''
#include <assert.h>
#include <stdbool.h>
#define BIT(n) (1U << (n))
#define MTK_DSI 1
struct drm_crtc { unsigned int index; };
struct mtk_ddp_comp { int id; } comp;
struct mtk_drm_crtc { struct drm_crtc base; } crtc;
static unsigned int mask, alias;
static bool connected;
static struct mtk_drm_crtc *to_mtk_crtc(struct drm_crtc *c)
{ return (struct mtk_drm_crtc *)c; }
static struct mtk_ddp_comp *mtk_ddp_comp_request_output(struct mtk_drm_crtc *c)
{ (void)c; return connected ? &comp : 0; }
static int mtk_ddp_comp_get_type(int id) { return id; }
static unsigned int mtk_ddp_comp_get_alias(int id) { (void)id; return alias; }
static unsigned int mtk_disp_num_from_atag(void) { return mask; }
static unsigned int drm_crtc_index(struct drm_crtc *c) { return c->index; }
#define NULL ((void *)0)
FUNCTION
int main(void)
{
    for (mask=0;mask<4;mask++) for (alias=0;alias<2;alias++)
    for (unsigned int index=0;index<2;index++)
    for (int type=0;type<2;type++) for (int present=0;present<2;present++) {
        crtc.base.index=index; comp.id=type; connected=present;
        bool expected = present && type==MTK_DSI &&
            ((mask & BIT(alias)) || (mask==0 && index==0));
#ifdef CONFIG_MTK_DISP_NO_LK
        expected=false;
#endif
        assert(mtk_drm_is_enable_from_lk(&crtc.base)==expected);
    }
    assert(!mtk_drm_is_enable_from_lk(NULL));
}
'''.replace("FUNCTION", source[start:end])
with tempfile.TemporaryDirectory(prefix="nord2-lk-handoff-") as directory:
    path = Path(directory)
    for no_lk in (False, True):
        # The driver's flag is an empty header define, not a Kconfig integer.
        flag = "#define CONFIG_MTK_DISP_NO_LK\n" if no_lk else ""
        (path / "test.c").write_text(flag + harness)
        subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Werror",
                        str(path / "test.c"), "-o", str(path / "test")], check=True)
        subprocess.run([str(path / "test")], check=True)
print("PASS: no-LK always cold-starts; legacy handoff preserves its mask/fallback policy")
