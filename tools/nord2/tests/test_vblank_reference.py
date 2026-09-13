#!/usr/bin/env python3
"""Exercise the real atomic-begin event block with counted DRM references."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[3]
source = root / "kernel/kernel_device_modules-6.6/drivers/gpu/drm/mediatek/mediatek_v2/mtk_drm_crtc.c"
text = source.read_text()
anchor = text.index("mtk_crtc_state->base.event->pipe = index;")
start = text.rfind("\tif (mtk_crtc_state->base.event) {", 0, anchor)
end = text.index("\n\t/*Msync 2.0:", anchor)
block = text[start:end]
assert start >= 0
harness = r'''
#include <assert.h>
#include <stddef.h>
struct event { int pipe; } event;
struct state { struct { struct event *event; } base; } state;
struct controller { struct event *event; } controller;
static int calls, refs, errors, reports, answer, reported;
static int drm_crtc_vblank_get(void *crtc)
{
    (void)crtc; calls++;
    if (!answer) refs++;
    return answer;
}
#define DDPAEE(fmt, function, line, error, crtc) do { errors++; reported=(error); } while (0)
#define display_exception_trackpoint_report(fmt, error) do { reports++; reported=(error); } while (0)
static void run(void)
{
    struct state *mtk_crtc_state=&state;
    struct controller *mtk_crtc=&controller;
    void *crtc=&controller;
    int index=2;
BLOCK
}
int main(void)
{
    for (int present=0; present<2; present++) {
        for (int fail=0; fail<2; fail++) {
            calls=refs=errors=reports=reported=0;
            answer=fail ? -22 : 0;
            state.base.event=present ? &event : NULL;
            controller.event=NULL; event.pipe=-1;
            run();
            assert(calls==present);
            assert(refs==present*(1-fail));
            assert(errors==present*fail);
#ifdef OPLUS_TRACKPOINT_REPORT
            assert(reports==present*fail);
#else
            assert(reports==0);
#endif
            assert(reported==((present&&fail) ? -22 : 0));
            assert(controller.event==(present ? &event : NULL));
            assert(state.base.event==NULL);
            if (present) assert(event.pipe==2);
        }
    }
    return 0;
}
'''.replace("BLOCK", block)
with tempfile.TemporaryDirectory(prefix="nord2-vblank-") as directory:
    path = Path(directory)
    (path / "test.c").write_text(harness)
    for trackpoint in (False, True):
        command = [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Werror"]
        if trackpoint:
            command.append("-DOPLUS_TRACKPOINT_REPORT")
        subprocess.run(command + [str(path / "test.c"), "-o", str(path / "test")], check=True)
        subprocess.run([str(path / "test")], check=True)
print("PASS: one vblank acquisition per event; conditional, side-effect-free error reporting")
