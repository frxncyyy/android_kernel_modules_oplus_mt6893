#!/usr/bin/env python3
"""Exercise the LK handoff decision, the DSI probe gate and the idle gate.

LK lit the panel, so the CRTC keeps LK's framebuffer *and* the DSI inherits
LK's output/clock/panel state: this DDIC is a command-mode panel that scans the
splash frame out of its own RAM, and it is the only thing keeping the screen lit
between the hand-off and the first Android frame.

That inherited DDIC state cannot survive the display idle manager's first DSI
ULPS cycle - it stops reporting TE, the trigger loop parks on EVENT_TE, the next
config packet hits the 1 s CMDQ timeout and only a full panel re-init recovers
(V13-V16).  mtk_dsi_lk_state_in_use() therefore holds the idle manager off the
DSI until the kernel has re-initialised the panel itself, after which the same
cycles are harmless (V17+).

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

# The probe adopts the whole state LK left behind: that inherited frame in the
# DDIC RAM is the only thing that can keep the screen lit between the hand-off
# and the boot animation, because the CRTC commits no frame in that window
# (measured on V12/V20).  The cost is that this DDIC stops reporting TE after
# the first idle-manager ULPS cycle, so the idle manager has to stay away from
# the DSI until the kernel has run its own panel bring-up.
probe = dsi[dsi.index("static int mtk_dsi_probe("):]
for inherited in ["dsi->output_en = true;", "dsi->clk_refcnt = 1;",
                  "dsi->panel->prepared = true;", "dsi->panel->enabled = true;",
                  "dsi->lk_adopted = true;"]:
    assert inherited in probe, inherited
assert "dsi->ext->is_connected = mtk_dsi_lk_adopted(dsi, alias);" in probe

enable = dsi[dsi.index("static void mtk_output_dsi_enable("):]
assert "dsi->lk_adopted = false;" in enable
# a slave DSI shares the master's panel bring-up and may never see its own
# enable call, so the flag has to be cleared together with the master's
assert "dsi->slave_dsi->lk_adopted = false;" in enable

lowpower = (v2 / "mtk_drm_lowpower.c").read_text()
assert "mtk_dsi_lk_state_in_use(priv)" in lowpower
assert "idle entry held off" in lowpower
assert "bool mtk_dsi_lk_state_in_use(struct mtk_drm_private *priv)" in dsi
# only a panel-bearing DSI may hold the idle manager off
assert "dsi->lk_adopted = true;" in probe
assert probe.index("if (dsi->panel) {") < probe.index("dsi->lk_adopted = true;")

harness = r'''
#include <assert.h>
#include <stdbool.h>
#define BIT(n) (1U << (n))
#define MTK_DSI 1
struct drm_crtc { unsigned int index; };
struct device { int id; };
struct mtk_ddp_comp { int id; struct device *dev; } comp;
struct mtk_drm_crtc { struct drm_crtc base; } crtc;
struct mtk_dsi { bool is_slave; bool lk_adopted; } dsi;
struct mtk_drm_private { struct mtk_ddp_comp *ddp_comp[2]; } priv;
#define DDP_COMPONENT_DSI0 0
#define DDP_COMPONENT_DSI1 1
static void *dev_get_drvdata(struct device *d) { (void)d; return &dsi; }
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
    /* The idle gate only follows the inherited-state flag. */
    priv.ddp_comp[DDP_COMPONENT_DSI0] = &comp;
    dsi.lk_adopted = false;
    assert(!mtk_dsi_lk_state_in_use(&priv));
    dsi.lk_adopted = true;
    assert(mtk_dsi_lk_state_in_use(&priv));
    dsi.lk_adopted = false;
    priv.ddp_comp[DDP_COMPONENT_DSI0] = 0;
    assert(!mtk_dsi_lk_state_in_use(&priv));
    assert(!mtk_dsi_lk_state_in_use(0));
    priv.ddp_comp[DDP_COMPONENT_DSI0] = &comp;
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
print("PASS: handoff requires a videolfb with this display's bit; probe adopts "
      "LK's DSI/panel state and idlemgr is held off until the kernel owns it")
