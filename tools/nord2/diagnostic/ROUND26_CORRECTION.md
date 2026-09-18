# Round 26 correction: the port baseline, measured properly

Earlier readings in round 25 were taken while the phone was running **stock 4.19**
(`Linux version 4.19.198-ORIGIN-g6b67e2d84c67-dirty`, `lineage_denniz-userdebug 15`), not the
6.6 port.  Anything sampled in that window describes stock and is void:
`mtk_vcu` bound, `/dev/vcu` present, `/proc/modules` empty, `trusty:gz-main` bound by
`trusty_gz`.  The boot partition did hold the port image (`e243abbb...`, matching
`diagnostic-6.6-padded.img`); the sample was simply taken against a boot that had not come up
on 6.6.

## The real 6.6 port baseline (99-module step-2 image, boot=1, stable)

    kernel 6.6.30-4k-g2a08123e2d84, sys.boot_completed=1
    148 modules in /proc/modules
    /dev/dri/card0            present
    /dev/ion                  present
    /dev/vcu                  ABSENT
    16000000.vcu              no driver bound
    mtk-vcu.ko                NOT in modules.load
    trusty:mtee               UNBOUND
    trusty:gz-main            UNBOUND
    trusty:trusty-virtio      UNBOUND

## What this changes

The whole trusty/gz subtree is unbound on a **working** 6.6 boot, and `card0` is fine.  So
`dispsys_config` does **not** require the `gce_mbox_sec` -> `trusty:mtee` path in this
configuration.  The round-25 story ("vcu needs gce_mbox_sec needs trusty:mtee, therefore
stage the gz chain") was built on the assumption that this path is load-bearing for DRM.  It
is not, on the evidence: DRM works with the entire subtree unbound.

What remains true and evidenced:
  - adding `mtk-vcu.ko` alone to the load list breaks DRM (round 24 bisect),
  - adding the gz chain on top of that still breaks it (round 25),
  - `cmdq_sec_probe` logs `link status 0` (`DL_STATE_DORMANT` at cmdq-sec-mailbox.c:1937)
    when it does run, which is `-EPROBE_DEFER`, not a hard failure.

## Method rule going forward

Verify `uname -r` reports `6.6.30-4k-g2a08123e2d84` before recording any measurement, and
re-verify it after any reset.  A stock boot is reachable and looks superficially similar.

## Round 26 result: the vcu closure, found by symbol resolution

The real blocker was **unresolved symbols**, not the trusty/gz chain order.  Resolving all 126
undefined symbols of `mtk-vcu.ko` against the running kernel's kallsyms left 16 that do not
resolve, and a module with even ONE unresolved symbol fails to load entirely **and silently**:

    cmdq_sec_mbox_enable, cmdq_sec_mbox_disable, cmdq_sec_mbox_switch_normal,
    cmdq_sec_pkt_set_data, cmdq_sec_pkt_set_mtee, cmdq_sec_pkt_set_secid,
    cmdq_sec_pkt_write_reg        <- cmdq-sec-drv.ko
    mtk_vcodec_vcp, vcu_func, dmabuf_to_secure_handle,
    is_disable_map_sec            <- mtk-vcodec-common.ko

`gz_main_mod.ko` registers platform driver `gz_main` against compatible
`mediatek,trusty-mtee-v1` (`geniezone/gz_main.c`), which is what the `trusty:mtee` device
asks for; it has **zero** unresolved symbols.

Load order appended after `mtk_ion`:

    gz_main_mod -> cmdq-sec-drv -> mtk-vcodec-common -> mtk-vcu     (103 modules)

**Result, verified on the device:**

    14116000.dispsys_config -> mediatek-drm     BOUND   (previously UNBOUND)
    10228000.gce_mbox_sec   -> cmdq_sec_mbox    BOUND   (previously UNBOUND)
    16000000.vcu            -> mtk_vcu          BOUND   (previously UNBOUND)
    /dev/dri/card0  /dev/vcu  /dev/video0  /dev/ion

The whole deferred-probe cascade that rounds 21-25 chased is resolved, and `/dev/vcu`,
`/dev/video0` and a working `card0` now coexist for the first time.

**Still open:** `sys.boot_completed` is never set; `init.svc.surfaceflinger` and
`init.svc.vendor.hwcomposer-2-3` stay empty.  With DRM bound and card0 present, the
surfaceflinger failure is now a *separate, downstream* problem rather than a DRM
consequence.
