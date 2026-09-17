# Touchscreen (Focaltech FT3518)

## The firmware-class regression: a haptics fix that killed touch

Touch worked throughout the v100-v117 rounds. It stopped working in the v118 rounds, and the cause was not
in the touch driver at all — it was a one-token addition to the kernel command line made while fixing haptics.

### Symptom

The touchscreen was dead, with the FT3518 controller left in bootloader mode. The log showed the panel being
**reflashed on every boot** and the reflash not sticking:

    read chip id:0x5452                      <- controller alive and correct
    fts_tp_probe, probe normal end           <- probe succeeds
    Need update, force(0)/fwver:Host(0x7e),TP(0x30)
    **********erase now**********
    **********write data to flash**********
    ecc in tp:51, host:51                    <- checksum passes
    upgrade success, reset to normal boot
    read chip id:0x00ef                      <- WRONG (0x5452 expected)
    fw is invalid, need read boot id
    boot id:0x545c, fw abnormal
    Need update, ...                         <- and round again

Each pass erases the controller, writes, and leaves it invalid; the loop repeats several times per boot.

### Root cause

The haptics work staged `aw8697_haptic_170.bin` into `/lib/firmware` — a correct fix, since the loader
searches `/lib/firmware` unconditionally while the diagnostic boot never mounts `/odm`, where the file really
lives. Alongside it, as "belt and braces", the cmdline was extended:

    firmware_class.path=/vendor/firmware                     <- before
    firmware_class.path=/vendor/firmware,/odm/firmware       <- after

That second path changed firmware resolution for the **touch** driver, which requests
`tp/20817/FW_FT3518_SAMSUNG.img`. With the load no longer resolving, the driver treated the panel firmware as
stale and performed the destructive erase-and-rewrite above, on every boot.

The tell is a single token in the kernel command line, printed during boot. Comparing the two rounds:

    v117 (touch fine):  firmware_class.path=/vendor/firmware
    v118 (touch dead):  firmware_class.path=/vendor/firmware,/odm/firmware

`base.dtb` carries only `/vendor/firmware`; the extra path came from the packager's literal cmdline string in
`work/android-boot/package_v118.py`. The kernel stores comma-separated paths in `fw_path_para[]` in order
(`firmware_loader/main.c:473-488`, via the `firmware_class.path` module param at `:536-548`).

### The fix

Remove `,/odm/firmware` from the cmdline. The `/lib/firmware` staging is sufficient on its own and is the
mechanism the round actually depends on. Verified on the device afterwards:

    probe normal end        1
    read chip id:0x5452     1
    Need update             0     (was 4)
    fw abnormal             0     (was 4)
    chip id:0x00ef          0     (was 2)
    upgrade success         0

No erase, no reflash, controller stays in normal mode.

### What this cost, and the lesson

Touch was broken for two rounds and the first reading of the evidence blamed it on the panel firmware being
absent from the image, which was wrong — the firmware was never in the image and touch worked anyway. The
regression was an unrelated side effect of a change made for a different subsystem, and it was found only by
diffing the **kernel command line** between a working round and a broken one.

When touching `firmware_class.path`, or any global firmware/DT search order, re-check the other subsystems
that load firmware: touch (FT3518), haptics (AW8697), GPU (valhall), connectivity (conninfra). A change that
fixes one loader can starve another, and the symptom appears in whichever subsystem is unlucky, not in the one
being edited.
