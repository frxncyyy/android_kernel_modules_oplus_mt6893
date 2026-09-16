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
