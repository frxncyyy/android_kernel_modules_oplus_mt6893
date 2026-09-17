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


## Round 25: correction - those CCCI lines are benign, the layout init succeeds

Round 24 said the kernel log "names exactly where it stops".  That was wrong, and
reading the code and the rest of the sequence shows why.

`mtk_ccci_md_smem_layout_init()` in `ccci_util_md_mem.c` starts with

    s_md_gen = 6297;
    ret = mtk_ccci_find_args_val("md_generation", &s_md_gen, sizeof(unsigned int));
    if (s_md_gen < 6295) { pr_info("gen93 bypass init smem layout in util"); return 0; }
    if (ret <= 0) pr_info("get md generation fail(%d)");

so `s_md_gen` is **defaulted to 6297** and "get md generation fail(-1)" only says
the optional `md_generation` boot arg is absent - it leaves a valid default.  The
function then tries `get_md_smem_layout_tbl_from_lk_tag()` (which wants the
`nc_smem_layout_num` / `c_smem_layout_num` / `nc_smem_layout` / `c_smem_layout`
boot args), and on failure falls back to
`get_md_smem_layout_tbl_from_lk_legacy_tag()`, which uses the **built-in static
`gen6295_*` / `gen6297_*` tables**.  That fallback is the designed path for a
board whose LK does not supply those args.

And the log shows it running on past the lines I misread:

    Key[md_bank0_base] not exist            -> bank0 base not found
    Key[md_generation] not exist            -> get md generation fail(-1)
    Key[nc_smem_layout_num] not exist       -> get nc_smem_num fail:-1
                                            -> get md smem layout from tag directly not support(-1)
    Key[smem_align_padding_size] not exist  -> using -1 as align padding size
    ... smem amms pos size:0 ; dfd size:8388608 ; get_udc_nc_size using 0 as udc size
    c_smem_info_parsing ccb: data:8b500000 data_size:33554432

The last line is the legacy path parsing a real cacheable layout, so
`mtk_ccci_md_smem_layout_init()` ran to completion and returned success.  The
`Key[md1_sib_info] not exist` from `get_sib_info_from_tag()` is likewise
non-fatal - it logs and returns.

Two consequences:

* The round-24 direction (diff the LK tag parsing, supply the layout via DT) was
  chasing a non-problem.  The layout comes from the built-in tables and does not
  need to be supplied.
* There is a trap worth avoiding: `if (s_md_gen < 6295) ... return 0;` bypasses
  the shared-memory initialisation entirely.  Forcing `md_generation` below 6295 -
  which looks like an attractive way to silence the warnings - would skip the smem
  mapping the modem needs.  Do not do that.

So the modem's actual failure is **after** the CCCI utility initialisation, in the
image load / FSM path, and that is where the next round should look.  What is now
established from round 23 still stands and is the reliable part: the modules load,
`/dev/ccci*` exists, the ccmni netdevices come up, and the modem reserved memory is
claimed.


## Round 26: the modem is running, and stock userspace is talking to it

Re-reading the round-23 log from the end rather than the start changes the picture
again, this time upward.  The CCCI stack is not merely loaded - it is working, and
the stock userspace is using it:

    [ccci1/mcd]md_cd_get_modem_hw_info, val: mddbgss, 0x2844, l2sram_size: 0
    [ccci1/dpmf][dpmaif_init_register] register: ao_ul=... pd_md_misc=... pd_sram: ...
    [ccci1/fsm]kern_broadcast_md_sate: 1 start
    [ccci1/fsm]kern_broadcast_md_sate: 1/0x0 end
    [ccci1/fsm]command 2 is completed 1 by fsm_main_thread [ccci_md_all]
    [ccci1/chr]port ccci_aud close by HwBinder:640_1 rx_len=0 empty=1
    [ccci1/chr]port ccci_raw_dhl close by emdlogger ...
    [ccci1/chr]port ccci_ccb_ctrl close by emdlogger ...
    [ccci1/cif]total cnt=5317;rxq0 isr_cnt=6;rxq1 isr_cnt=58;rxq4 isr_cnt=4066;rxq5 isr_cnt=1184
    [ccci1/bat][ccci_dpmaif_bat_stop] stop.

So: the modem hardware info is read, the DPMAIF data path registers, the FSM
reaches state 1 and completes commands, `emdlogger` and a HwBinder client - the
stock vendor daemons - open and close modem ports by name, and the DPMAIF has
serviced **5317 buffers** with thousands of interrupts across its queues.  That is
a live modem interface, not a stack that failed to start.

Note also the timestamps: these are at 240-243 s, the very end of the round, so the
modem was still active when the guard rebooted to recovery after its ~4 minute
budget.  Nothing here says the modem gave up.

`port ccci_0_200 read data fail when md_state = 0` appears early and is the
expected result of a userspace read before the modem is up.

### Revised status, and the honest limit

The earlier framing - "kernel half proven, firmware and userspace still to do" -
was too pessimistic.  The userspace half is stock and demonstrably talking to the
6.6 CCCI stack.  What remains unproven is whether the modem completes RF bring-up
and registration, and **that cannot be settled on this phone as it stands**: there
is no SIM (`gsm.sim.state` is `ABSENT,ABSENT` on stock as well), and no
registration without one.

What is established is now a fairly strong statement: on the 6.6 port the modem
drivers load, the device nodes and data path come up, the modem reaches FSM state
1, and the stock vendor daemons successfully open and use the modem ports.

### Third revision in three rounds

Round 24 called the benign CCCI warnings the blocker; round 25 corrected that;
round 26 corrects the pessimism that remained.  The recurring cause is reading a
log's loudest lines instead of its whole sequence and the code behind it.  For
anyone continuing: read the **end** of the CCCI sequence first, and treat
`Key[...] not exist` lines as absent optional boot args rather than failures.

## Round: post-revert guest boot with a SIM inserted — the modem comes up

First round with a SIM in the device, and the modem brings itself up unaided. The FSM walks the full
sequence and reaches the running state:

    [ccci1/fsm]md_state change from 0 to 2      power on
    [ccci1/mcd][POWER ON]md1_pmic_setting_on start / end
    [ccci1/fsm]md_state change from 2 to 3
    [MDPM] AP2MD1 section, 2G: 0x2b7cefbf0096be33, 3G: 0x1d08ca740012216c
    [ccci1/fsm]md_state change from 3 to 4      running
    [ccci1/fsm]md_state change from 4 to 7
    [ccci1/fsm]md_state change from 7 to 1

The `AP2MD1 section` line is the modem accepting the AP's boot configuration, which is the point at which the
baseband is genuinely alive rather than merely powered. The AP<->MD data path is also moving: `dpmaif-rxq0`
reports `received:84` (the previous round, without a SIM, reported `received:0`) and `dpmaif-txq` shows real
write/read/release movement across queues 0-3.

### The `ccci_fs open fail with EBUSY` flood is not a modem fault

It is by far the loudest line in the log — **22,007 occurrences** in this round alone — and it is a red
herring of exactly the kind this document keeps warning about. `ccci_fs` is opened by the AP side before the
modem has finished coming up; each early attempt gets `-EBUSY` and is retried. The retries stop once
`md_state` reaches 4. It does not indicate a failure, only impatience, and it dominates the ring buffer so
thoroughly that it evicts almost everything else — which is the real cost, not the message itself.

### State at this round

- `md_state` 0 -> 2 -> 3 -> 4 (running) -> 7 -> 1, with the AP2MD section exchanged.
- Modem data path active (`rxq0 received:84`, TX queues moving).
- RF registration still cannot be verified from the kernel log alone; that needs a live Android userspace
  with the radio HAL up, and the diagnostic round hands back to recovery before that point.

---

## Service restart loops (round 11) - `fuelgauged` and `vpud`

`fuelgauged` and `vpud` restart every 5.00s on the port and are stable on stock.  Measured
on stock in the same session, with the device fully booted:

| service      | stock              | port                        |
|--------------|--------------------|-----------------------------|
| `fuelgauged` | running, 0 kills   | restarting, 105 kills       |
| `vpud`       | running, 0 kills   | restarting, 88 kills        |
| `fps_hal`    | running            | restarting                  |

The interval is an exact 5.00s (5.00, 5.02, 5.01, 4.99, 5.03 measured across a run), which
is a restart loop, not a crash.

### What actually happens

The service does not crash - it exits cleanly and immediately:

```
init: starting service 'fuelgauged'...
init: ... started service 'fuelgauged' has pid 2001
init: Service 'fuelgauged' (pid 2001) exited with status 0
init: Sending signal 9 to service 'fuelgauged' (pid 2001) process group...
```

`status 0` ruled out a fault, so the cause had to be inside the program.  `/vendor/bin/fuelgauged`
is a 5084-byte ELF that is only a loader: it `dlopen`s `/vendor/lib/libfgauge_gm30.so`, resolves
`libfgauge_setup`, and exits 0 on any failure, logging to `/dev/kmsg` as `MTK_FG_FUEL`.  The
relevant strings are `load 'libfgauge_setup' error: %s` and `init failed, return!`, so an
exit 0 is that error path.

The library is present and loads; its own dependencies include `libmtk_drvb.so` (present) and
`libbh_gm30.so`, which exists nowhere on the device - **including stock, where fuelgauged runs
fine** - so that absence is not the differentiator.

`ft3518`-style hardware problems were ruled out: the kernel side is healthy on the port.
`/sys/class/power_supply/battery/capacity` reads 98 and `voltage_now` 4371, matching stock,
and the `battery_thread` (pid 381) keeps printing normal `GM3log` records.

### SELinux denials are present on BOTH builds - not the cause

`fuelgauged` logs `GM3 disable, nl handler rev data` and takes netlink denials:

```
avc: denied { create } for comm="fuelgauged" tclass=netlink_socket permissive=1
avc: denied { bind }   for comm="fuelgauged" tclass=netlink_socket permissive=1
```

Stock shows the same 4 denials and the same 2 `GM3 disable` messages, so this is normal
behaviour on this device and not the regression.  Beware of it as a red herring: it looks
like a policy bug, but it is identical on the working build.

### Method note - why this was invisible before

53.6% of one captured boot log was the single repeated line
`[ccci1/chr]port ccci_fs open fail with EBUSY` (21,899 of 40,834 lines), which evicted the
services' own output from the ring buffer.  `port_proxy.c:315` logged every occurrence at
`CCCI_ERROR_LOG` level; it is a benign startup race (two clients reach a port before the
first finishes registering, and the retry succeeds).  Ratelimiting it to the first 8 per
boot cut its share to 20%, and that is what let the 5s loop become visible at all.

Second method note: the guard's recovery window was 240s, which cut the boot off before
userspace settled.  Extending it to 360s with service-state dumps at 120s/210s/300s is what
produced the `getprop` states above.  A conclusion drawn at 120s is premature.

### Still open

The exact reason `libfgauge_setup` fails on the port is not yet identified.  The next step is
to run the loader with its logging visible during a port boot (`MTK_FG_FUEL` writes to
`/dev/kmsg`, so it should appear in the expdb capture) - the round-11 capture did not contain
those lines, so the guard needs to grep for `MTK_FG_FUEL`/`fgauge` explicitly rather than
relying on the default message set.

### Root cause found (round 11/15): the port kernel never receives the LK `atag,*` DT nodes

The gauge and vpu services run stable on stock and restart every 5.00s on the port because
the port kernel cannot see the bootloader's `atag,*` device-tree nodes.  Measured directly:

Stock `/proc/device-tree/chosen` contains the bootloader-injected nodes (16 of them),
including the 884-byte `atag,devinfo`, plus `atag,boot`, `atag,chipid`, `atag,masp`,
`atag,ptp`, `atag,mem`, `atag,mdinfo`, `atag,imix_r`, `atag,fg_swocv_i/v`, and
`atag,shutdown_time`.

The port's `/chosen` has only `bootargs`, `kaslr-seed` and `phandle` - none of the `atag,*`
nodes.  The port's own boot log names the miss from both sides:

```
ufshcd-mtk 11270000.ufshci: cannot find atag,ufs
ufshcd-mtk 11270000.ufshci: failed to get atag,boot
```

`atag,devinfo` appears **0** times anywhere in the port's captured boot.

### Why that stops `fuelgauged`

`libmtk_drvb.so` (identical binary on stock and port - it ships in `/vendor/lib`, which is
the same image in both) reads exactly one thing:

```
/proc/device-tree/chosen/atag,devinfo
```

and exposes it as `sec_drv_base_check` / `sec_drv_info_update`.  `libfgauge_gm30.so` imports
both symbols, so the chain is:

```
fuelgauged (5084B loader)
  -> dlopen /vendor/lib/libfgauge_gm30.so
  -> needs sec_drv_base_check / sec_drv_info_update from libmtk_drvb.so
  -> reads /proc/device-tree/chosen/atag,devinfo   (ABSENT on the port)
  -> init fails -> loader exits 0 ("init failed, return!")
  -> init restarts it -> 5.00s loop
```

An exit status of 0 with an immediate exit is the loader's own error path, which is why this
is a restart loop and not a crash - and why searching for a signal or tombstone found nothing.

### What is NOT the cause (each verified)

- **SELinux.** Stock shows the same 4 netlink denials for `fuelgauged` and the same 2
  `GM3 disable` messages.  Identical on the working build, so not the regression.
- **A missing `libbh_gm30.so`.** It exists nowhere on the device, including stock, where the
  service runs - the loader tolerates it.
- **`/mnt/vendor/nvcfg/fg`.** The library references it, but it is absent on stock too.
- **The kernel battery path.** Healthy on the port: `capacity` 98 and `voltage_now` 4371,
  matching stock, with `battery_thread` logging normal `GM3log` records.
- **The binaries.** `libfgauge_gm30.so` and `libmtk_drvb.so` are byte-identical between
  stock and port (`1af80685...`, `493e7ba7...`); both live on `/vendor`.

This also explains `vpud` and `fps_hal` restarts, which share the same `libmtk_drvb` /
atag-based device-identity path rather than a per-service fault.

### Where this fits in the port

The `atag,*` nodes are injected by LK into the kernel DT at boot.  The port hands the kernel
a different DT (`base.dtb` carries only `bootargs`/`kaslr-seed`/`phandle` under `/chosen`),
so the injection point is lost.  Fixing this means letting LK's atag additions land on the
port's `/chosen` - relevant to the flash/hand-off path, not to any module.

---

## GPU blur/transparency (round 16) - driver is fine, GED DT lookups fail

The kernel GPU driver is **healthy** and is the right one for this hardware.  Measured on
the running port:

```
/sys/class/misc/mali0/device/gpuinfo -> Mali-G77 9 cores r0p1 0x09000800
dmesg: mali 13000000.mali: Kernel DDK version r49p1-03bet0
dmesg: mali 13000000.mali: GPU identified as 0x0 arch 9.0.8 r0p1 status 1
```

`arch 9.0.8` is Valhall G77 and `status 1` means the driver bound successfully.  An earlier
hypothesis - that the port ships the wrong Mali generation - is **wrong** and should not be
revisited: the prebuilt `mali_kbase_mt6893_r49.ko` contains `arm,mali-valhall`, `Mali-G77`
and the `valhall-1691526.wa` workaround name, and `mali_avalon/` is only MediaTek's
directory name, not the GPU architecture.  The `valhall-1691526.wa` firmware the driver
wants is shipped in the ramdisk, so the "WA blob missing - driver probe will be failed"
path is not taken.

`CONFIG_MTK_GPU_VERSION` is empty on the port while stock has `"mali valhall r32p1"`, so the
source-tree driver is not built and the round falls back to the prebuilt r49 module.  That
is a real difference, but it is **not** what breaks the compositor, because the prebuilt
module drives the G77 correctly.

### What IS wrong

The GED (GPU Energy Driver) fails its device-tree lookups on the port, and stock does not:

| | stock | port |
|---|---|---|
| `ged_pdrv_probe` | clean, no errors | ~10 errors |
| `No gpueb node` | absent | present |
| `No fdvfs node` | absent | present |
| `fail to read APO policy (-22)` | absent | present |
| `Failed to find gpu_dcs node` | absent | present |
| `Failed to find async_dvfs_node` | absent | present |
| `Failed to init core mask table` | absent | present |

Stock's `/proc/device-tree` has `ged`, `gpufreq`, `gpueb`, `dvfsrc@10012000`,
`mali_dvfs_hint@13fbb000` and `eemgpu_fsm@1100b000`.  The port's packaged `base.dtb` has
**none** of them under `/`, yet the running kernel still drives Mali, which means the port
boots with a MediaTek DT from the LK hand-off rather than from `base.dtb`.

This is the same shape as the `atag,devinfo` problem: nodes the driver probes for are not
present in the tree the port kernel ends up with.  GED drives GPU DVFS and core-mask
selection, so a partial failure there is consistent with the symptom - basic rendering
works, while features that change GPU workload shape (blur, layered transparency) do not.

`ged_segment_id_init` logging `mt6985_efuse_segment_cell` is a **red herring**: the literal
is shared across SoCs in `ged_main.c:665`, and the failure is handled gracefully by setting
`g_ged_segment_id = NO_SEGMENT`.  It is not the cause.

### Next step

Diff the live `/proc/device-tree` between a stock boot and a port boot, focusing on
`ged`, `gpueb`, `gpufreq`, `dvfsrc` and `mali_dvfs_hint`.  Whatever is missing there is what
GED is failing to find, and it is the same fix that would supply `atag,devinfo` for the
fuel-gauge services - so the two remaining items probably share one root cause.

### Fix attempt (round 17): package the real MDP driver instead of the stub

`mdp_drv_dummy.ko` (22KB) was the only MDP module in the round.  It exports just
`mdp_dpc_register` and `mdp_set_resource_callback` and never creates the compositor's
device nodes - while stock's own boot has both:

```
crw-r----- system system 245,  0 /dev/mtk_mdp
crw-r----- system system  10, 54 /dev/mdp_sync
```

MDP is the MediaTek hardware compositor for blur, colour conversion and overlay blending, so
a missing compositor matches the reported symptom exactly: ordinary rendering works, while
blur and layered transparency silently do nothing.

`mdp_drv_mt6893.ko` (2.9MB) already builds from `CONFIG_MTK_MDP_MT6893=m`, which the
defconfig sets, so this is a packaging fix rather than a kernel change.  Round 31 kept the
stub only to test whether removing it broke the boot; that experiment was never concluded,
and nothing depends on the stub.  `cmdq_helper_inf` exports the same
`mdp_set_resource_callback`, so the two cannot coexist and the stub had to be removed rather
than kept alongside its replacement.

Three things had to be right, each of which silently yields a packaged module that never
loads:

1. **Copy before `index` is built.**  `index` and `modules.load` are derived from `moddir`,
   so a module copied later cannot be named in `modules.load` and stays unloaded.
2. **Name it in `modules.load`.**  Presence in the ramdisk is not enough; the packager only
   loads what `order` lists.
3. **Know which "dependencies" to skip.**  `modinfo -F depends` lists `cmdq-sec-drv` and
   `mtk_sec_heap`, but both are optional secure-memory providers whose own symbols
   (`KREE_*`, `trusted_mem_api_*`, `is_pkvm_enabled`, `tmem_type2sec_id`) resolve from
   neither the kernel nor any packaged module.  A symbol-level check confirms MDP imports
   none of them, so including them would add two modules that cannot load while
   contributing nothing.  Every symbol MDP does import resolves cleanly.

The 2.78MB driver also pushed the boot image 0.25MB past its 32MB limit.  The 8.90MB
`/nord2-late` camera staging paid for it: those modules are from the abandoned camera
bring-up, a full port boot references neither the directory nor any of the module names, and
the block's own comment recorded that the round-60 probe found the directory unreadable
("nord2-late dir FAIL, 0 bytes read").  Image is now 28.04MB with room to spare.

### Round 17 result: the compositor is live

`mdp_drv_mt6893` loads and stays loaded, with `mediatek_drm` depending on it:

```
mdp_drv_mt6893        512000  2
cmdq_helper_inf        49152  2 mdp_drv_mt6893,mediatek_drm
mtk_cmdq_drv_ext      397312 17 mdp_drv_mt6893,mediatek_drm,cmdq_helper_inf,...
```

Both compositor device nodes now exist, and `dmesg` has **no** MDP errors:

```
crw-r----- system system  10, 114 /dev/mdp_sync
crw-r----- system system 489,   0 /dev/mtk_mdp
```

Stock's own nodes are `245,0` for `/dev/mtk_mdp` and `10,54` for `/dev/mdp_sync`; the major
numbers differ because they are dynamically allocated, which is expected and not meaningful.

The GPU side is unchanged and healthy: `Mali-G77 9 cores r0p1 0x09000800`, and zero
GED failures in dmesg - so the earlier GED errors were a symptom of the missing compositor
rather than an independent fault.

Two things this round did NOT fix, both still open:

* `fuelgauged` and `vpud` still restart on their 5s cycle (`init.svc.*` = `restarting`).  That
  is the separate `/chosen/atag,devinfo` root cause, not MDP.
* Blur and transparency need on-device visual confirmation.  The nodes existing proves the
  compositor is registered and reachable, which is necessary but not sufficient - only the
  owner can confirm the control-centre blur actually renders.

Packaging notes that cost time and are worth remembering:

* `mdp_drv_dummy.ko` had to be removed rather than kept alongside the real driver, because
  both export `mdp_set_resource_callback`.
* The modules must be copied into `moddir` *before* `index` is built, and named in
  `modules.load`, or they are packaged but never loaded.
* `CONFIG_MTK_MDP_MTEE_SUPPORT` had to be disabled; see the commit for the dependency chain
  it dragged in.

### Fix (round 18): package the hardware codec stack

Hardware encode/decode was broken for the same reason MDP was: the drivers are configured
and built, they were simply never staged into the boot image, so Android fell back to
software codecs.

The port already sets `CONFIG_DEVICE_MODULES_VIDEO_MEDIATEK_VCODEC=m`,
`CONFIG_DEVICE_MODULES_VIDEO_MEDIATEK_JPEG=m` and `CONFIG_VIDEO_MEDIATEK_VCU=m`, and
`mtk-vcodec-dec-v2.ko`, `mtk-vcodec-enc-v2.ko`, `mtk-vcodec-common.ko`, `mtk_jpeg.ko` and
`mtk-vcu.ko` all build.  Stock runs the same drivers as platform devices (`mtk-vcodec-dec`,
`mtk-vcodec-enc`, `mtk-jpeg`, `mtk_vcu`) built into the kernel, which is why its
`/proc/modules` shows nothing for them while `/dev/video0-4` exist.

So, as with MDP, this is a packaging fix and not a kernel change.

The dependency analysis is the interesting part, because `modinfo -F depends` is actively
misleading here and following it almost triples the image:

* Modinfo lists a long roll-call of optional collaborators - fpsgo, sspm, the OPPO sched
  and cpufreq modules, mtk_game, cm_mgr, the legacy and other-SoC devapc drivers.
* `task_turbo` sits at the root of that subtree, so a transitive walk drags all of it in:
  33 modules and 26.5MB unstripped.
* But only mtk-vcodec-enc-v2 imports anything from task_turbo (`enforce_ct_to_vip`), and
  `mtk-vcodec-common` exports that same symbol.  Task_turbo exports nothing else any
  packaged module imports, so dropping it collapses the whole subtree and the list falls
  from 33 modules to 9.

The real closure is 9 modules: the five codec drivers plus a genuine TEE chain.
`mtk-vcu` imports seven `cmdq_sec_*` symbols, so `cmdq-sec-drv` must ship and needs `KREE_*`
from `gz_tz_system`, which needs TIPC from `gz_ipc_mod`, which needs `get_smcnr_dev` from
`gz_trusty_mod`.  Unlike MDP, VCU's use of the secure path is not config-gated, so the chain
has to come along rather than be disabled.

Four more modules looked required but are not, and each would have caused a duplicate-export
failure the preflight rejects:

| Symbol | Already packaged provider | Rejected addition |
| --- | --- | --- |
| `register_devapc_vio_callback` | `device-apc-common` | legacy, mt6761, mt6765 |
| `is_disable_map_sec` | `mtk-vcodec-common` | `iommu_gz` |
| `mc_close_device` | `mcDrvModule` | `mcDrvModule-ffa` |
| `enforce_ct_to_vip` | `mtk-vcodec-common` | `task_turbo` |

`iommu_gz` is the instructive one: modinfo records it as a dependency of both codec drivers,
but a symbol-level check shows they import zero gz/tee symbols from it - the name match came
from symbols other modules happen to share.  The same was true of `mtk_sec_heap` and
`trusted_mem`, which nothing packaged imports from at all.

### Round 18 result: the kernel codec stack is live

All five codec devices bind, and the v4l2 nodes match stock exactly:

| Node | Port name | Stock name |
| --- | --- | --- |
| `/dev/video1` | `mtk-vcodec-dec` | `mtk-vcodec-dec` |
| `/dev/video2` | `mtk-vcodec-enc` | `mtk-vcodec-enc` |
| `/dev/video3`, `/dev/video4` | `mtk-jpeg-enc` | `mtk-jpeg-enc` |
| `/dev/vcu` | (present) | (present) |

Bound devices, matching stock device-for-device:

```
1602f000.vdec  -> mtk-vcodec-dec
17020000.venc  -> mtk-vcodec-enc
17030000.jpgenc, 17830000.jpgenc -> mtk-jpeg
16000000.vcu   -> mtk_vcu
```

The probe log shows the full hardware path linked - SMMU `1411a000.m4u`, the `1602f000`/`17000000` syscons, and the vdec/venc SMI larbs - with **zero** codec errors in dmesg:

```
platform 1602f000.vdec: Linked as a consumer to 1411a000.m4u
mtk-smi-larb 1600d000.smi_larb5: SMI5 CLK1:vdec-soc-larb
mtk-smi-larb 17010000.smi_larb7: SMI7 CLK1:venc-set1
```

Both kernel codec threads are alive: `[mtk-vcodec-dec]` and `[mtk-vcodec-enc]`.

The device names are unchanged from stock, so the DT was never the problem - the port's
`vdec@16000000` / `venc@17000000` / `vcu@16000000` / `jpgenc@17030000` nodes carry the same
compatibles as stock's (`mediatek,mt6885-vcodec-dec`/`-enc`, `mediatek-vcu`,
`mediatek,jpgenc`), and `mt6885-vcodec-dec` is in the driver's own match table.  Two earlier
readings in this investigation were wrong and are recorded here so they are not repeated:
`dtc` cannot parse the boot image's DTB because it carries a 64-byte vendor-table prefix
before the `d0 0d fe ed` magic, which made the codec nodes look absent when they were
present; and an over-aggressive `grep -vE` on the driver directory made the devices look
unbound when all five were bound.

**Still open: `vpud` does not run on the port.**  Stock runs `/vendor/bin/vpud -f` as a
`class main` service (`user media`, `group system media drmrpc`) and it is alive as PID 1470;
on the port `init.svc.vpud` does not even exist.  The kernel side is therefore complete -
the v4l2 nodes exist and the IPI protocol already speaks the 4.19 numbering this daemon
expects (commit bdd9dfc0) - but nothing consumes them, so hardware encode/decode cannot
complete a real workload yet.

That is the same `/chosen/atag,devinfo` root cause that stops `fuelgauged`, not a codec
fault: both are vendor `class main` services that the port's init never starts.  Fixing the
atag nodes should start `vpud` as a side effect, so the atag work is the next step rather
than any further codec change.

### Round 19: why vpud dies - the kernel has no ION

`vpud` is **not** missing on the port.  An earlier reading in this file said `init.svc.vpud`
did not exist; that was wrong, and the expdb capture from round 15 shows the truth:

```
init: ... started service 'vpud' has pid 1784
init: Service 'vpud' (pid 1784) exited with status 0
```

`init.svc.vpud` reads `restarting`, and init kills and restarts it on a ~5.00s cycle -
exactly like `fuelgauged`.  So vpud starts, and returns **status 0 about 4ms later**.
A clean, immediate exit like that is a deliberate early return, not a crash.

The cause is a missing kernel driver, not the service definition.  `vpud` pulls in
`libvpud_vcodec.so`, which calls `ion_share`/`ion_import`; those live in `libion.so`,
whose only failure path is:

```
open /dev/ion failed: %s
```

Stock has the node and the port does not:

| | stock | port |
| --- | --- | --- |
| `/dev/ion` (10,60) | present | **absent** |
| `/dev/vcu` (487,0) | present | present |
| `/dev/vpu` | absent | absent (not needed) |
| `libion.so` `/system/lib` | present | present |
| `libion_mtk.so` `/vendor/lib` | present | present |
| `libvpud_vcodec.so` | present | present |
| `vpud` process | running (pid 1449) | restarting |

Every userspace piece is already on the device.  The one missing thing is the kernel side:
stock is `CONFIG_ION=y` with `CONFIG_MTK_ION=y`, while the port has **no ION at all** -
neither `CONFIG_ION` nor `CONFIG_MTK_ION` appears in the port config, and no `ion` directory
exists in the 6.6 tree (`drivers/staging/android/` there holds only `ashmem.c`).

This is not an oversight in the port so much as the shape of the upstream kernel: ION was
removed from mainline and replaced by dma-buf heaps, which the port does have
(`CONFIG_DMABUF_HEAPS=y`, `DEVICE_MODULES_DMABUF_HEAPS_SYSTEM=m`).  But the two are not
interchangeable here - `vpud` and `libion_mtk.so` speak the ION ioctl ABI
(`ION_IOC_{ALLOC,FREE,MAP,SHARE,IMPORT,SYNC,CUSTOM}` on magic `'I'`), which dma-buf heaps
does not implement, and stock's `libion.so` only ever opens `/dev/ion`.

The MTK ION driver does exist in this workspace, but only for 4.19:
`work/src/kernel-4.19/drivers/staging/android/{ion,mtk_ion,aosp_ion}` - 36 files, ~628KB of
source, of which `mtk_ion` alone is 32 files.  Bringing that to 6.6 is the work item.

So: `vpud` is blocked on a kernel driver that has to be ported, not on packaging, and not on
the `atag,devinfo` issue that stops `fuelgauged` - those are two separate faults that happen
to present as the same 5-second restart loop.  The atag work will not fix vpud.
