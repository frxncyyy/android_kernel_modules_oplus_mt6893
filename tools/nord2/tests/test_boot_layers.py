#!/usr/bin/env python3
"""Check cold-start layer ownership and preserve the existing idle path."""
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
start = text.index("#ifdef CONFIG_MTK_DISP_NO_LK", text.index("void mtk_crtc_first_enable_ddp_config("))
end = text.index("\n\t/*2. Load Round Corner */", start)
cold_start = text[start:end]
start = text.index("#ifdef CONFIG_MTK_DISP_NO_LK", text.index("void mtk_crtc_config_default_path("))
end = text.index("#endif", start) + len("#endif")
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
struct mtk_drm_private { struct chip *data; } priv = { &chip };
struct device { void *dev_private; } dev = { &priv };
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
static void cold(void)
{
    struct mtk_drm_crtc *mtk_crtc=&crtc;
    struct cmdq_pkt *cmdq_handle=&packet;
COLD_START
}
static void cold_default(void)
{
    struct mtk_drm_crtc *mtk_crtc=&crtc;
    struct cmdq_pkt *cmdq_handle=&packet;
    (void)mtk_crtc; (void)cmdq_handle;
COLD_DEFAULT
}
static void reset(void) { for (int i=0;i<6;i++) keep[i]=-1; }
int main(void)
{
    for (int i=0;i<6;i++) components[i].id=i;
    for (int soc=1;soc<=4;soc++) {
        chip.mmsys_id=soc;
        for (int dual=0;dual<2;dual++) {
            crtc.is_dual_pipe=dual; reset(); cold();
            for (int i=0;i<6;i++) {
                if (!dual && i>=3) { assert(keep[i]==-1); continue; }
#ifdef CONFIG_MTK_DISP_NO_LK
                assert(keep[i]==0);
#else
                assert(keep[i]==(i==0 || i==3 || (soc<4 && i<3)));
#endif
            }
            reset(); cold_default();
            for (int i=0;i<6;i++) {
#ifdef CONFIG_MTK_DISP_NO_LK
                assert(keep[i]==((!dual && i>=3) ? -1 : 0));
#else
                assert(keep[i]==-1);
#endif
            }
            /* Idle entry retains its old first-layer policy in both builds. */
            reset(); mtk_crtc_all_layer_off(&crtc,&packet);
            for (int i=0;i<6;i++) {
                if (!dual && i>=3) { assert(keep[i]==-1); continue; }
                assert(keep[i]==(i==0 || i==3 || (soc<4 && i<3)));
            }
        }
    }
}
'''.replace("FUNCTIONS", functions).replace("COLD_START", cold_start).replace("COLD_DEFAULT", cold_default)
with tempfile.TemporaryDirectory(prefix="nord2-boot-layers-") as directory:
    path = Path(directory)
    (path / "test.c").write_text(harness)
    for no_lk in (False, True):
        command = [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Werror", "-Wno-unused-but-set-variable"]
        if no_lk:
            command.append("-DCONFIG_MTK_DISP_NO_LK")
        subprocess.run(command + [str(path / "test.c"), "-o", str(path / "test")], check=True)
        subprocess.run([str(path / "test")], check=True)
print("PASS: cold no-LK start disables every layer on both pipes; LK and idle policies preserved")
