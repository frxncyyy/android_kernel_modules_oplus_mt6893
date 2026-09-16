# Modem and calls on the Nord 2 port

Status: **not started, reconnaissance done** (round 20).  Nothing has been flashed
for the modem yet.

## What the port has

The complete MTK CCCI modem stack is present in the module tree **and built**:

    drivers/misc/mediatek/ccci_util/ccci_util_lib.ko
    drivers/misc/mediatek/eccci/ccci_md_all.ko      <- the core, 97 exported ccci globals
    drivers/misc/mediatek/eccci/hif/ccci_ccif.ko
    drivers/misc/mediatek/eccci/hif/ccci_cldma.ko
    drivers/misc/mediatek/eccci/hif/ccci_dpmaif.ko  <- 89 globals
    drivers/misc/mediatek/eccci/fsm/ccci_fsm_scp.ko
    drivers/misc/mediatek/eccci/ccci_auxadc.ko
    drivers/misc/mediatek/mddp/mddp.ko

**None of them is in the load plan.**  `package_v47.py` and every stage manifest
were grepped for ccci/mddp/modem and there is not one hit, so the port boots with
no modem driver at all.  The stock 4.19 DTB that the port uses does carry the
nodes: `md@1000f000`, `md@10019000`, `md@1021d000`, `ccci`, `ccci_ccif`,
`modem_buck_reg`, `modem_oc`, `modem_temp_share`.

So this starts in the same place the sensor stack did - drivers built, nothing
loading them - with one important difference: the sensor investigation showed
that guessing at the provider/consumer relationships costs many rounds, so the
dependency order below was derived from the symbols first.

## Derived load order

From `llvm-nm -u` on each module and `--defined-only` for the providers:
`ccci_md_all.ko` is the core (it exports `ccci_alloc_skb`, `ccci_dump_write`,
`ccci_fsm_get_md_state`, `ccci_get_per_md_data`, `ccci_hif_register`, ...), and
`ccci_ccif`/`ccci_cldma`/`ccci_dpmaif`/`ccci_fsm_scp` all *need* those, so they
load after it.  `ccci_dpmaif` additionally exports 89 globals of its own.

    ccci_util_lib -> ccci_md_all -> ccci_ccif, ccci_cldma, ccci_dpmaif,
                                   ccci_fsm_scp -> mddp

## Plan

1. Ship that set in that order (the same shape as the round-46 fmeter fix: an
   explicit copyfile plus an ordered group in the packager), then read back:
   does `ccci_md_all` probe, does the modem image load, do the `/dev/ccci*`
   nodes appear, and does the kernel log a modem state transition.
2. Only then look above the kernel: the modem firmware (`md1img`) and the vendor
   userspace (`mdinit`/`rild`/`ccci_mdinit`) that stock uses to bring the modem
   up and expose calls.
3. Calls specifically need the audio path as well, so expect the modem and the
   audio front end to be separate failures.

## Evidence standard

As with the display and sensor work: nothing counts until the phone shows it.
For the modem that means the modem reaching a ready state and a call or at least
a registered SIM - not a sysfs attribute and not a log line on its own.


## Round 21: the dependency guard rejected the first shipping attempt, and named the gaps

Shipping the CCCI set straight away was rejected by the preflight's module
dependency check before anything was flashed - which is the guard doing exactly
its job, and in one round rather than ten:

    {'ccci_cldma.ko':  ['ccmni_ops', 'ccmni_set_cur_speed'],
     'ccci_dpmaif.ko': ['ccmni_ops', 'ccmni_set_cur_speed', 'ccmni_set_init_rps',
                        'ccmni_set_tcp_is_need_gro', 'dvfsrc_get_required_opp_peak_bw',
                        'set_ccmni_rps'],
     'ccci_md_all.ko': ['ccmni_ops', 'mtk_smpu_md_handling_register',
                        'smpu_clear_md_violation']}

So the CCCI set needs three more providers, all of which are built:

    ccmni_ops, ccmni_set_*, set_ccmni_rps   -> drivers/misc/mediatek/ccmni/ccmni.ko
    dvfsrc_get_required_opp_peak_bw         -> drivers/soc/mediatek/mtk-dvfsrc.ko
    mtk_smpu_md_handling_register           -> drivers/memory/mediatek/emi_legacy/emi-dummy.ko

Note the emi-dummy path: it is under `drivers/memory/mediatek/emi_legacy/`, not
`drivers/misc/mediatek/emi/submodule/` where it was first looked for.  Loading
providers before consumers means the group becomes:

    mtk_dvfsrc, emi_dummy, ccmni, ccci_util_lib, ccci_auxadc, ccci_md_all,
    ccci_ccif, ccci_cldma, ccci_dpmaif, ccci_fsm_scp, mddp

### State at the end of round 21

Nothing was flashed for the modem.  The round-48 bundle was rejected by the
preflight, and the phone was rebooted back to ColorOS and verified
(`sys.boot_completed=1`) with all partitions intact.

The round-49 packaging attempt then failed in the harness rather than on the
device: the `emi-dummy.ko` path in that script was wrong, and
`build_boot_dtb.py` also returned non-zero while packaging the v49 bundle, so the
preflight never ran.  `work/private/android-v49/` should be treated as unusable -
rebuild the bundle from `android-v47` (which is known good) and apply the module
additions above.


## Round 22: the guard walks the closure one layer at a time

Two things happened.  First, the round-49 packaging failures were my own mistake,
not a harness breakage: the packager runs `build_boot_dtb.py` with
`work/tools/sysroot/usr/bin` prepended to PATH and needs `fdtget`, which comes
from `. work/build-env.sh`; running the packager without sourcing the build env
gives `fdtget ... exit status 127`.  Recorded so it is not misdiagnosed again -
`work/private/android-v49/` was a casualty of it, not a real fault.

With the build env sourced, the dependency guard accepted the three providers
from round 21 (ccmni, mtk-dvfsrc, emi-dummy) and moved on to name exactly one
more:

    {'ccmni.ko': ['set_rps_map']},

which is provided by

    drivers/misc/mediatek/rps/rps_perf.ko

So the closure is nearly complete.  The group to ship, providers first:

    emi_dummy, mtk_dvfsrc, rps_perf, ccmni, ccci_util_lib, ccci_auxadc,
    ccci_md_all, ccci_ccif, ccci_cldma, ccci_dpmaif, ccci_fsm_scp, mddp

Nothing has been flashed for the modem yet.  The phone was rebooted back to
ColorOS and verified (`sys.boot_completed=1`) after each rejected attempt, and
the preflight is the reason no bad bundle ever reached the phone.

Worth noting how differently this has gone from the sensor work: the dependency
guard converts each missing provider into one named symbol per round instead of a
boot loop to debug.  Rounds 21 and 22 cost two attempts and produced an exact
list; the equivalent sensor detour cost roughly ten rounds.


## Round 23: the modem kernel stack loads and creates its devices

With `rps_perf` added, the preflight accepted the closure (`disallowed_imports:
{}`, `duplicate_exports: {}`, image 27568128 bytes) and the bundle was flashed.
First result: the kernel-side modem stack is up.

`/dev` now contains the full CCCI node set:

    ccci_0_200  ccci_0_202  ccci_0_204  ccci_aud  ccci_bip
    ccci_c2k_agps  ccci_c2k_ppp  ccci_ccb_ctrl  ccci_ccb_dhl  ccci_ccb_md_monitor

the ccmni netdevices initialise (`ccmni_dev_init MODEM_CAP_HWTXCSUM`), and the
modem reserved memory is claimed (`mblock-25-ccci`, 54656 KiB, and
`mblock-22-ccci_tag_mem`).  Sensors were re-checked in the same round and still
enumerate (lsm6dso accelerometer/gyroscope, mmc5603 magnetometer, tcs3701), so the
modem modules did not disturb the SCP/sensor path.

Also seen, worth following up: `consys_emi_get_md_shared_emi_mt6893` logs
"ECCCI Driver is not supported", a connectivity/modem shared-EMI dependency.

### What this does and does not prove

`gsm.version.baseband` reads `M_V3_P10` and `gsm.sim.state` reads `ABSENT,ABSENT`.
By the evidence standard set above, **neither is proof of a working modem** - the
baseband string is a vendor property that is present whether or not the modem is
running, and the SIM being absent is exactly what an unstarted modem looks like.
What is proven is the kernel half: the drivers load, the nodes exist, the data
path network devices come up, and the memory is reserved.  The modem firmware and
the vendor userspace that starts it are still the open half of this item.

### Process notes (my mistakes this round, recorded so they are not repeated)

Two harness slips, neither a port problem:

* `run_v51.py` was made with `cp run_v50.py run_v51.py` instead of the usual
  `sed`, so it kept a stale version string and wrote this round's captures into
  `work/private/test-android-v48/`.  The evidence is real, just misfiled; always
  `sed` the round scripts.
* `restore_round.py 51` then failed on the missing `expdb-after.img`.  That is its
  post-restore hash check only - the actual restore (its `flash_boot_diagnostic.py
  restore` step) had already run and succeeded, and the phone was verified back on
  ColorOS (`sys.boot_completed=1`).  A later manual restore attempt asserted
  `id -u == 0 && ro.product.device == denniz`, which is that script's safety guard
  refusing to operate on Android rather than a fault.


## Round 24: the modem cannot learn its shared-memory layout

The userspace half turns out **not** to be a port problem at all.  The port boots
the stock vendor Android, and on stock the modem services are already present and
running: `/vendor/bin/ccci_mdinit`, `ccci_rpcd`, `md_monitor`, `emdlogger`, with
`init.svc.ccci_mdinit`, `init.svc.vendor.ccci_rpcd`, `init.svc.emdlogger` and
`init.svc.vendor.ril-daemon-mtk` all **running**.  So there is no userspace to
port; what matters is whether the stock userspace can talk to the 6.6 kernel's
CCCI interface.

The kernel log names exactly where it stops.  The CCCI driver cannot find the
modem shared-memory layout:

    ccci: mtk_ccci_md_smem_layout_init :get md generation fail(-1)
    ccci: mtk_ccci_md_smem_layout_init :get md smem layout from tag directly not support(-1)
    ccci: mtk_ccci_find_args_val(line:84): Key[md1_sib_info] not exist

`mtk_ccci_find_args_val("md1_sib_info", ...)` is used at
`ccci_util_lib_fo.c:401` and `ccci_util_md_mem.c:520`, and that file also carries
`get_md_smem_layout_tbl_from_lk_tag()` and a `..._lk_legacy_tag()` variant.  So the
6.6 driver expects the layout to arrive as an **LK boot tag**; this board's LK
supplies it in a form the 6.6 driver reads as "not support", and no DT fallback
covers it either - the port's `base.dtb`/`boot-table.dtb` contain no
`md1_sib_info`/`md_smem`/`sib_info` strings, and neither does the stock DTB (nor
the stock kernel command line).  The 4.19 driver evidently understood the tag
this LK produces; the 6.6 driver does not.

That is a specific, named incompatibility of the same family as the hf_manager
ABI and the fmeter provider - not a mystery.  Candidate directions for the next
round, in order:

1. Read the 4.19 `ccci_util_md_mem.c` tag parsing against the 6.6 version and see
   which tag structure the LK actually emits (`get_md_smem_layout_tbl_from_lk_tag`
   vs the legacy variant, and the generation field that returns -1).
2. If the layout can be supplied through the DT instead, add it to
   `build_boot_dtb.py`'s `--power` path from values that can be read off the stock
   device, rather than reverse-engineering the tag.
3. `Key[md1_sib_info] not exist` is a boot-argument lookup: check whether the LK
   boot table the port reuses simply does not carry it, and whether adding it is
   possible from the prepared DTB.

### Test-design constraint: this phone has no SIM

`gsm.sim.state` reads `ABSENT,ABSENT` **on stock as well**, so there is no SIM
inserted.  "The SIM registers" is therefore not a usable acceptance test here.  A
reachable target is the modem booting to a ready state and RIL coming up against
it; a call would additionally need a SIM and a network.
