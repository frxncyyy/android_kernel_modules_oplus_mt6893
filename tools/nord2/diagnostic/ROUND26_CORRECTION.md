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
