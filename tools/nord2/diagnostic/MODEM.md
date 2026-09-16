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
