#!/usr/bin/env python3
"""Exercise the LK handoff decision and the DSI probe gate.

Only the CRTC inherits LK's display: it keeps the bootloader framebuffer so the
boot logo stays on screen until the first Android frame is committed.  The DSI,
its PHY and the panel are deliberately brought up by the kernel on the first
encoder enable.  Inheriting LK's DSI/panel state leaves the AMS643YE05 DDIC
unable to report TE after the display idle manager's first DSI power/ULPS
cycle: the trigger loop parks on EVENT_TE, the next config packet hits the 1 s
CMDQ timeout and the ESD workaround has to re-initialise the panel, killing
surfaceflinger during boot.

Both entry points still share one rule: /chosen/atag,videolfb exists *and* its
per-display islcmfound bit is set means LK lit this output.  The old "display
count is zero, so assume DSI0" shortcut also matched a boot that carried no
videolfb at all, which made a cold boot inherit a DSI that had never been
programmed.
"""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[3]
v2 = root / "kernel/kernel_device_modules-6.6/drivers/gpu/drm/mediatek/mediatek_v2"

drv = (v2 / "mtk_drm_drv.c").read_text()
start = drv.index("static bool mtk_drm_is_enable_from_lk(")
end = drv.index("\nstatic bool mtk_atomic_skip_plane_update", start)
handoff = drv[start:end]

dsi = (v2 / "mtk_dsi.c").read_text()
start = dsi.index("static bool mtk_dsi_lk_adopted(")
end = dsi.index("\nstatic int mtk_dsi_probe(", start)
adopted = dsi[start:end]

# The probe may only inherit the connector's present bit.  Adopting the DSI
# output, the panel state or the clock reference is what broke TE.
probe = dsi[dsi.index("static int mtk_dsi_probe("):]
for inherited in ["dsi->output_en = true;", "dsi->clk_refcnt = 1;",
                  "dsi->panel->prepared = true;", "dsi->panel->enabled = true;"]:
    assert inherited not in probe, inherited
assert "dsi->ext->is_connected = mtk_dsi_lk_adopted(dsi, alias);" in probe

harness = r'''
#include <assert.h>
#include <stdbool.h>
#define BIT(n) (1U << (n))
#define MTK_DSI 1
struct drm_crtc { unsigned int index; };
struct mtk_ddp_comp { int id; } comp;
struct mtk_drm_crtc { struct drm_crtc base; } crtc;
struct mtk_dsi { bool is_slave; } dsi;
static unsigned int bits, alias;
static bool connected, slave, fb;
static struct mtk_drm_crtc *to_mtk_crtc(struct drm_crtc *c)
{ return (struct mtk_drm_crtc *)c; }
static struct mtk_ddp_comp *mtk_ddp_comp_request_output(struct mtk_drm_crtc *c)
{ (void)c; return connected ? &comp : 0; }
static int mtk_ddp_comp_get_type(int id) { return id; }
static unsigned int mtk_ddp_comp_get_alias(int id) { (void)id; return alias; }
static bool mtk_drm_lk_fb_present(void) { return fb; }
static unsigned int mtk_disp_bits_from_atag(void) { return bits; }
static unsigned int panel_connection_from_atag(void) { return bits; }
#define NULL ((void *)0)
HANDOFF
ADOPTED
int main(void)
{
    for (bits=0;bits<4;bits++) for (alias=0;alias<2;alias++)
    for (unsigned int index=0;index<2;index++)
    for (int type=0;type<2;type++) for (int present=0;present<2;present++)
    for (int have_fb=0;have_fb<2;have_fb++) for (int is_slave=0;is_slave<2;is_slave++) {
        crtc.base.index=index; comp.id=type; connected=present;
        fb=have_fb; slave=is_slave; dsi.is_slave=is_slave;
        bool expected = present && type==MTK_DSI && have_fb &&
            (bits & BIT(alias));
        assert(mtk_drm_is_enable_from_lk(&crtc.base)==expected);
        /* Cold boot (no videolfb at all): neither entry point may adopt. */
        bool adopted_expected = is_slave || (bits & BIT(alias));
        assert(mtk_dsi_lk_adopted(&dsi, alias)==adopted_expected);
    }
    assert(!mtk_drm_is_enable_from_lk(NULL));
    /* bits==0 with a framebuffer present must not silently mean "DSI0 on". */
    bits=0; connected=true; comp.id=MTK_DSI; fb=true; alias=0; crtc.base.index=0;
    assert(!mtk_drm_is_enable_from_lk(&crtc.base));
    /* No videolfb: a set bit cannot arrive, and a frame-less boot never adopts. */
    bits=1; fb=false;
    assert(!mtk_drm_is_enable_from_lk(&crtc.base));
}
'''.replace("HANDOFF", handoff).replace("ADOPTED", adopted)
with tempfile.TemporaryDirectory(prefix="nord2-lk-handoff-") as directory:
    path = Path(directory)
    (path / "test.c").write_text(harness)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Werror",
                    str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
print("PASS: handoff requires a videolfb with this display's bit; the DSI probe "
      "inherits only the connector's present bit")
