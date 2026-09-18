# Reliable boot capture (expdb mirror)

## Problem
Failing boots rebooted at t~2.5s and no readable log survived: pstore/ramoops came back
empty or already consumed, and the expdb region the bootguard wrote was read back stale.
Diagnosis was being done against logs from the wrong boot.

## Fix
`android_boot_guard.c` now keeps expdb open as a permanent **mirror**, independent of the
primary sink (which migrates to /metadata and is invisible from recovery):
- mirror lives at `EXPDB_MIRROR_OFF` = 32 MiB into the expdb partition
- header `NORD2-EXPDB-MIRROR-V1` + used length at +32, body at +4096
- written on **every** loop pass (100 ms), not once per second

Read it from recovery:
    dd if=/dev/block/by-name/expdb of=/tmp/m.bin bs=1M skip=32 count=8
    # magic at [0:21], used at [32:40] (u64 LE), body at [4096:4096+used]

## What it captured (first working use)
2180 lines, complete through t=2.466s - previously the log was cut at 1.993s, so the
entire final second (where the failure lands) had been invisible.

## Round 123: what the mirror revealed about the vcu/cmdq blocker

The first build that used the reliable capture (114 modules, cmdq chain placed at 38-46)
still bootlooped, but for the first time the failure was fully visible.

### cmdq is what stops the codec stack

    [1.354231] [cmdq] cmdq_drv_init enter
    [1.354245] [cmdq] cmdq_util_init begin
    [1.354741] [cmdq] cmdq_util_init end
    [1.355637] [cmdq] cmdq_probe support_mpu:0 support_epu:0 support_hwmbox:0 support_aux:0
    [1.356032] [cmdq][err] no default tokens:-22   @cmdq_config_default_token,3271
    [1.356045] [cmdq][err] not support hwmbox        @cmdq_config_hwmboxes,3294
    [1.356207] mtk_cmdq_mbox 10228000.gce_mbox: register mailbox successfully
    [1.357295] [cmdq] cmdq_util_get_hw_id cmdq_platform->util_hw_id is NULL
    [1.357304] [cmdq][err] channel request fail:-19 idx:0 @cmdq_mbox_create,504
    [1.358152] [cmdq][err] channel request fail:-19 idx:1 @cmdq_mbox_create,504

`16000000.vcu` binds through `10228000.gce_mbox_sec`, so every vcu channel request fails and
the codec stack never comes up.

### Mechanism

`cmdq_util_set_fp()` is what installs `cmdq_platform` (and with it `util_hw_id`):

    drivers/misc/mediatek/cmdq/mailbox/cmdq-util.c:239   void cmdq_util_set_fp(...)
    drivers/misc/mediatek/cmdq/mailbox/cmdq-util.c:257   EXPORT_SYMBOL(cmdq_util_set_fp);

`cmdq-util.o` links into **mtk-cmdq-drv-ext** (`Makefile:49`), and its only caller is

    cmdq-platform-mt6893.c:245-250
        static int __init cmdq_platform_init(void) { cmdq_util_set_fp(&platform_fp); return 0; }
        module_init(cmdq_platform_init);

`cmdq-platform-mt6893.c` has **no `of_device_id` table and no `platform_driver`** - unlike
every other module in this port it cannot be probed by a device. It has to be loaded
explicitly and early, and in this boot its `module_init` did not run before
`mtk_cmdq_mbox` probed.

`cmdq_drv_init` (mtk-cmdq-mailbox-ext.c:3955) does **not** install a default fp, so nothing
compensates.

### Load-order work done this round (kept)

`modules.load` previously omitted the whole cmdq dependency chain; `modprobe` was pulling
`mtk-cmdq-drv-ext` in on demand from `modules.dep`, after the platform module had already
been handled. Now placed ahead of it:

    38 irq-dbg            44 mtk-cmdq-drv-ext
    39 mmprofile          45 cmdq_helper_inf
    40 iommu_debug        46 cmdq-platform-mt6893
    41 mtk-smi-dbg
    42 device-apc-common
    43 mrdump

`cmdq_platform_mt6893` was also removed from `diagnostic-assets-v5/modules.display`, which
had been pinning it near the top of the image, and the gpu `load_plan` and cpu-v1 manifest
now hold it back so the chain governs its position.

### Not yet explained

Only 5 `init: Loading module ...` lines appear (`mtk_wdt`, `reboot-mode`,
`syscon-reboot-mode`, `mrdump`, `aee_aed`), all before t=0.73s, yet ~50 modules show as
live in the "Modules linked in" line. So most modules are pulled by device probes rather
than by walking `modules.load`, and `cmdq-platform-mt6893` - which has no device - is never
pulled at all. Forcing that one load is the next step.
