#!/usr/bin/env python3
"""Check inherited-layer ownership on the cold-start and hand-off paths.

The decision is now runtime, not compile time: when the bootloader framebuffer
was adopted the LK logo layer is kept and its address is rewritten to an IOVA;
when it was not, every inherited layer is dropped before the display IOMMU is
enabled, otherwise OVL would scan out bootloader physical addresses.
"""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[3]
source = root / "kernel/kernel_device_modules-6.6/drivers/gpu/drm/mediatek/mediatek_v2/mtk_drm_crtc.c"
text = source.read_text()
start = text.index("static void __mtk_crtc_all_layer_off(", text.index("void mtk_crtc_config_default_path("))
end = text.index("\nvoid mtk_crtc_stop_ddp", start)
functions = text[start:end]
start = text.index("if (priv->fb_info.fb_gem) {", text.index("void mtk_crtc_first_enable_ddp_config("))
end = text.index("\n\t/*2. Load Round Corner */", start)
handoff_start = text[start:end]
start = text.index("if (!priv->fb_info.fb_gem)", text.index("void mtk_crtc_config_default_path("))
end = text.index(";", start) + len(";")
cold_default = text[start:end]
harness = r'''
#include <assert.h>
#include <stdbool.h>
#define MMSYS_MT6761 1
#define MMSYS_MT6765 2
#define MMSYS_MT6768 3
#define MMSYS_MT6885 4
struct mtk_ddp_comp { int id; } components[6];
struct chip { int mmsys_id; } chip;
struct mtk_drm_gem_obj { int unused; } gem;
struct mtk_ddp_fb_info { struct mtk_drm_gem_obj *fb_gem; };
struct mtk_drm_private { struct chip *data; struct mtk_ddp_fb_info fb_info; } priv_store = { &chip };
struct device { void *dev_private; } dev = { &priv_store };
struct mtk_drm_crtc { struct { struct device *dev; } base; bool is_dual_pipe; } crtc = { { &dev }, true };
struct cmdq_pkt { int unused; } packet;
#define for_each_comp_in_cur_crtc_path(comp, crtc, i, j) \
    for (i=0,j=0; i<3 && ((comp)=&components[i]); i++)
#define for_each_comp_in_dual_pipe(comp, crtc, i, j) \
    for (i=3,j=0; i<6 && ((comp)=&components[i]); i++)
#define DDPMSG(...) do {} while (0)
#define DDPINFO(...) do {} while (0)
#define OVL_ALL_LAYER_OFF 1
static int keep[6];
static void mtk_ddp_comp_io_cmd(struct mtk_ddp_comp *comp,
        struct cmdq_pkt *packet, int command, int *preserve)
{
    (void)packet; assert(command==OVL_ALL_LAYER_OFF);
    keep[comp->id]=*preserve;
}
FUNCTIONS
static void handoff(void)
{
    struct mtk_drm_crtc *mtk_crtc=&crtc;
    struct cmdq_pkt *cmdq_handle=&packet;
    struct mtk_drm_private *priv=&priv_store;
    (void)priv;
HANDOFF_START
}
static void cold_default(void)
{
    struct mtk_drm_crtc *mtk_crtc=&crtc;
    struct cmdq_pkt *cmdq_handle=&packet;
    struct mtk_drm_private *priv=&priv_store;
    (void)mtk_crtc; (void)cmdq_handle; (void)priv;
COLD_DEFAULT
}
static void reset(void) { for (int i=0;i<6;i++) keep[i]=-1; }
int main(void)
{
    for (int i=0;i<6;i++) components[i].id=i;
    for (int soc=1;soc<=4;soc++) {
        chip.mmsys_id=soc;
        for (int dual=0;dual<2;dual++) {
            crtc.is_dual_pipe=dual;
            for (int have_fb=0;have_fb<2;have_fb++) {
                priv_store.fb_info.fb_gem = have_fb ? &gem : 0;

                /* LK hand-off keeps the logo layer; cold start drops all. */
                reset(); handoff();
                for (int i=0;i<6;i++) {
                    if (!dual && i>=3) { assert(keep[i]==-1); continue; }
                    if (have_fb)
                        assert(keep[i]==(i==0 || i==3 || (soc<4 && i<3)));
                    else
                        assert(keep[i]==0);
                }

                /* Default-path enable only clears layers when nothing was
                 * inherited; with a mapped LK framebuffer it must not wipe
                 * the live logo. */
                reset(); cold_default();
                for (int i=0;i<6;i++) {
                    if (have_fb || (!dual && i>=3)) { assert(keep[i]==-1); continue; }
                    assert(keep[i]==0);
                }
            }
            /* Idle entry retains its old first-layer policy in both modes. */
            reset(); mtk_crtc_all_layer_off(&crtc,&packet);
            for (int i=0;i<6;i++) {
                if (!dual && i>=3) { assert(keep[i]==-1); continue; }
                assert(keep[i]==(i==0 || i==3 || (soc<4 && i<3)));
            }
        }
    }
}
'''.replace("FUNCTIONS", functions).replace("HANDOFF_START", handoff_start).replace("COLD_DEFAULT", cold_default)
with tempfile.TemporaryDirectory(prefix="nord2-boot-layers-") as directory:
    path = Path(directory)
    (path / "test.c").write_text(harness)
    command = [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Werror", "-Wno-unused-but-set-variable"]
    subprocess.run(command + [str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
print("PASS: hand-off keeps the LK layer, cold start clears inherited layers on both pipes")
