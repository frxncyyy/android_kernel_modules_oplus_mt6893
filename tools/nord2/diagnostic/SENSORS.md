# Sensors on the port (reconnaissance, round 4)

## What the device has

From the live stock ColorOS `dumpsys sensorservice` plus `/sys/bus/i2c/devices`:

| sensor | chip | bus |
| --- | --- | --- |
| accelerometer | `lsm6dso` (STMicro) | i2c |
| gyroscope | `lsm6dso` (STMicro) | i2c |
| magnetometer | `mmc5603` (Memsic) | i2c |
| proximity / ambient light | `tcs3701` (AMS) | i2c |

Stock additionally exposes the MTK synthetic sensors (uncalibrated mag/gyro,
significant motion, step detector/counter, device orientation, stationary
detect).  Those are computed in the hub and need no extra chip driver.

## What the port has

* Userspace is fine.  The port runs the stock vendor partition, which ships
  `sensors.mt6893.so`, `android.hardware.sensors@2.X-subhal-mediatek.so` and
  `/vendor/etc/sensors/hals.conf`.  Nothing to do there.
* Kernel side is missing.  The staging directory ships **no** sensor module at
  all, so `/dev/hf_manager` and the hub character devices never appear.
* The reference chip drivers that do exist in
  `drivers/misc/mediatek/sensors-1.0/` are for other boards - `mc3410-i2c`,
  `ITG1010`, `akm09918`, `cm36558`, `ltr303`.  None of them is a Nord 2 part,
  and `lsm6dso`, `mmc5603` and `tcs3701` do not appear anywhere in either the
  6.6 or the 4.19 source tree.
* That is not automatically fatal, because this port is configured for the
  sensor **2.0** architecture (`CONFIG_MTK_SENSOR_ARCHITECTURE="2.0"`,
  `CONFIG_MTK_SENSOR_SUPPORT=m`), where the chip drivers live in the **hub
  firmware**, not in the kernel.  `drivers/misc/mediatek/sensor/2.0/mtk_nanohub/nanohub/`
  is transport only - `comms.c`, `main.c`, `nanohub-mtk.c` - plus
  `sensor_list.c`, the table the AP side publishes to userspace.  So the stock
  hub firmware already knows how to talk to the lsm6dso, mmc5603 and tcs3701,
  and the kernel only has to reach the hub.

## The actual blocker

`hf_manager.ko` and `nanohub.ko` already build, but nothing loads them and the
hub is unreachable:

* `# CONFIG_MTK_SCP is not set` - nanohub registers its IPI mailbox through
  `scp_ipi_register()` (`mtk_nanohub_ipi.c`), so without the SCP driver there is
  no transport at all.
* `# CONFIG_MTK_SENSORHUB is not set` and `# CONFIG_CUSTOM_KERNEL_SENSORHUB_1_0
  is not set`; the 2.0 tree is reached through `CUSTOM_KERNEL_SENSORHUB`.
* Neither module is copied into the image.

## Next step

Enable the SCP stack (`CONFIG_MTK_SCP`, the tinysys/SCP driver and its
dependencies) so `scp.ko` provides the IPI transport, then ship
`hf_manager.ko`, `nanohub.ko` and the sensor-list module and check
`dumpsys sensorservice` on the port for the four real sensors.  The stock `scp`
and `nanohub` partitions are untouched, so the firmware half is already in
place.

Watch item: the SCP owns some power-domain and low-power sequencing, so this
change has to be re-checked against suspend and the AOD path, not just against
sensors.

Side note worth remembering: stock's `lux_aodhub` - the light sensor feed that
gates AOD brightness - comes from this same stack.  Until the hub is up, AOD
cannot react to ambient light, which is the remaining half of the AOD
calibration item.


## Round 40: the hub is reachable, the SCP itself is not up yet

The four modules were built all along and simply never copied into the image.
Shipping them (`mtk-scpsys.ko`, `scp.ko`, `hf_manager.ko`, `nanohub.ko`, in that
order) changed the picture on the phone:

* `/dev/scp` and `/dev/hf_manager` now exist.
* `scp_ipidev (with 52 IPI) has registered.`
* `[mtk_nanohub]init done, data_unit_t:44, SCP_SENSOR_HUB_DATA:48`
* `dumpsys sensorservice` lists UNCALI_GYRO, UNCALI_ACC and the OPLUS Fusion
  Light / Side Panel Fusion Light virtual sensors.
* AOD did not regress: zero `ESD check failed` events in the round.

But the four real sensors are still absent, and the log says why:

    scp 10500000.scp: invalid resource (null)      (x10 at boot)

That string comes from `lib/devres.c`, i.e. `devm_ioremap_resource()` was handed
a NULL resource.  The caller is `init_scp()` in
`drivers/soc/mediatek/mtk-scpsys.c`, which does
`res = platform_get_resource(pdev, IORESOURCE_MEM, 0)` and gets nothing back.
Until that resource resolves, the SCP never comes out of reset, the hub firmware
never runs, and no lsm6dso / mmc5603 / tcs3701 ever appears.  The virtual
sensors above are enumerated from the static sensor list, so their presence is
not evidence of hardware.

The `scp@10700000` DT node itself looks complete - `compatible =
"mediatek,scp"`, `status = "okay"`, seven `reg`/`reg-names` pairs, seven
interrupts, the send/recv tables, `scp_mem_key` pointing at the
`mediatek,reserve-memory-scp_share` node which is present with a 0x320000 size.
So the next step is to find which driver claims `mediatek,scp` on this tree and
why `platform_get_resource(..., 0)` comes back NULL for it - the DT has the
resources, so the mismatch is between that driver's expectations and this node's
layout, not a missing node.


## Round 6: the kernel side now matches stock

Following the `invalid resource (null)` lead to the end changed the conclusion.

* It is **not fatal to the SCP probe**.  The ten new lines come from
  `devm_ioremap_resource()` in the SCP helper's register mappings, but the probe
  never prints its own `[SCP] scpreg.sram error` and carries on: the log shows
  `[SCP] scp_reg_base_phy = 0x10700000`, `[SCP] loader image mem: ...`,
  `scp_dvfs probe done`, `scp_ipidev (with 52 IPI) has registered`.
* The same string appears on SPMI and MCUPM, and it is **pre-existing** -
  rounds 36 to 39 logged six of them (five `10027000.spmi`, one
  `10301000.mcupm`) before any SCP module was shipped.  Round 40 added the ten
  SCP ones and nothing else.  Those devices work, so the message alone says
  nothing about health.
* The port now exposes exactly what stock does.  Stock has `/dev/scp` and
  `/dev/hf_manager` and a `10500000.scp` platform device with `mtk-scpsys`,
  `scp` and `scp_dvfs` bound; the port now has `/dev/scp`, `/dev/hf_manager`
  and the same device name.
* The real defect in that window is a **shadow-call-stack unwinder warning** -
  `WARNING: CPU: 7 at arch/arm64/kernel/patch-scs.c:144
  scs_handle_fde_frame`, raised while unwinding out of
  `mt_scp_dvfs_pdrv_probe` - not an SCP failure.  It is worth chasing on its own
  (it fires for any trace that walks through these modules) but it does not stop
  the SCP.

So the remaining gap is above the kernel: `/sys/class/sensors` is still empty on
the port while stock enumerates four chips, which means the hub firmware is not
reporting its sensor list, or userspace is not reaching it.  The next check is
whether the hub answers at all on the port - its firmware/version string, and
the sensor list it hands back - rather than more work on the SCP driver.


## Round 41: the userspace ABI was the blocker, and it is fixed

The `hf_manager` command numbers are computed from the packet size, so the packet
layouts are ABI.  This ROM's sensor HAL was built against 4.19, where **every**
command was `_IOW/_IOWR('a', nr, struct ioctl_packet)` - a 4-byte header plus a
64-byte payload union, 68 bytes in total.  The 6.6 tree had reshaped the packets
(`common_packet` 8 bytes, `info_packet` 44, `cust_packet` 68, `debug_packet`
24), which changed every ioctl number.  The HAL's startup traffic is
`REGISTER_STATUS` and `READY_STATUS`, and it arrived as `0xc0446101` and
`0xc0446108` - 68-byte `_IOWR('a',1)` and `_IOWR('a',8)` - matching no case in
the driver's switch, which answered `Unknown command` for every one of them.

Unions of each payload with the 4.19 `int8_t byte[64]` restore the size, the
numbers and the data area at once (`sensor_info` is 40 bytes, `custom_cmd` 64,
the debug packet 24, so all fit).  Round 41 verified it: **`Unknown command`
dropped from continuous spam to zero**, and the HAL's startup exchange now
reaches the driver - `hf_manager_ioctl_request_ready` runs instead of falling
through.

## The next layer down

With the ABI fixed, the HAL gets an answer it can act on, and the answer is:

    [hf_manager]Device:mtk_nanohub not ready        (repeating)

`hf_manager_ioctl_request_ready()` walks its device list and fails the whole
request if any registered device has `ready == false`.  That flag is set in
`hf_manager.c` only after a device's support list has been processed, and the
only thing that gets it there is `mtk_nanohub_create_manager()` - which is the
**last step of `mtk_nanohub_power_up_loop()`** (mtk_nanohub.c:1968), after the
reset, the firmware download, the wait for init and the config/sensor restore.

The hub does log `[mtk_nanohub]init done` and `notify cmd SCP_INIT_DONE`, but the
manager is never created, so the power-up loop is not reaching its last step.
`/sys/class/sensordrv` and `/sys/class/oplus_sensor` stay empty on the port while
stock has both, which is the same fact seen from userspace.

Next: instrument or read `mtk_nanohub_power_up_loop()` to find which step it
stalls on and make it complete.  Note `mtk_nanohub_create_manager()` also
returns early once `create_manager_first_boot` is set, so a second call after a
failed first one is a silent no-op.


## Round 8: the AP to hub IPI direction is dead

Tracing the power-up loop down one level puts the stall before the manager:

    wait_event(power_reset_wait,
               READ_ONCE(scp_system_ready) && READ_ONCE(scp_chre_ready));

`scp_chre_ready` is set by the `SCP_INIT_DONE` IPI, and the port does log
`[mtk_nanohub]notify cmd SCP_INIT_DONE`, so that half arrives.  `scp_system_ready`
is set only in `mtk_nanohub_ready_event()` on `SCP_EVENT_READY` (registered with
`scp_A_register_notify`).  There is **no `SCP power up` line in the whole round**,
so the wait was never satisfied and the loop never ran - which is why
`mtk_nanohub_create_manager()` never runs and `hf_manager` keeps answering
`Device:mtk_nanohub not ready`.

What the log shows instead is the interesting part:

    [mtk_nanohub]notify event:1                      (repeated)
    [mtk_nanohub_ipi] IPI_SENSOR transfer timeout!
    [mtk_nanohub]mtk_nanohub_send_timestamp_wake_locked fail!

So the hub-to-AP direction works - `SCP_INIT_DONE` reaches the driver - while the
AP-to-hub direction does not: the sensor IPI never completes and the timestamp
wake that rides it fails every time.  That asymmetry points at the **send**
path: the mailbox/IPI the 6.6 driver picks for the sensor channel, not the hub
firmware being absent.  The firmware is demonstrably alive, since it sent
`SCP_INIT_DONE`.

Next: compare the IPI/mailbox the driver selects for `SENSOR_HUB` against the
`send_table`/`recv_table` in the DT node (the node carries both tables and
`mbox_count = 5`), and against what the 4.19 driver used for the same firmware.
`mtk_nanohub_send_timestamp_wake_locked()` is the cheapest place to start, since
it fails on every attempt.


## Round 9: the naming theory is wrong, and that is worth knowing

The obvious suspect looked perfect.  This ROM's DTB is the stock 4.19 one, which
spells the SCP tables `send_table`, `recv_table` and `scp_mem_tbl`, while the
6.6 tree's own `mt6893.dtsi` spells them `send-table`, `recv-table` and
`scp-mem-tbl`, and 4.19 read the underscore forms.  That is the same class of
mismatch as the hf_manager ABI, and the `scp_helper.c:2539` caller really does
pass `"send-table"`.

It is not the bug.  The `scp_dt_*` helpers in `scp_helper.h` all retry with an
alternative name (`scp_dt_alt_name`) when the first lookup fails, so the
underscore properties are found, and the driver would otherwise have printed
`[SCP] scp send table not found`.  Round 41's log contains no such line - the
only "table" message is Mali's unrelated OPP complaint.  The mailbox tables are
being parsed; the IPI ids are mapped.

So the AP-to-hub timeout is not a missing table.  What is left, in order of
likelihood:

* the IPI id nanohub sends `SENSOR_HUB` traffic on versus the id the hub
  firmware services (`SCP_INIT_DONE` proves the firmware is up, but that message
  may come from the SCP core rather than the sensor hub half);
* whether the sensor hub part of the firmware is running at all, or only the SCP
  core - the loader buffer the driver allocates is 8 KiB, far too small to be
  the whole image, so something else brings the rest up;
* the sensor share DRAM handshake (`scp_get_reserve_mem_virt(SENS_MEM_ID)`),
  which the power-up loop touches before any IPI.

`mtk_nanohub_send_timestamp_wake_locked()` remains the cheapest probe: it fails
on every attempt, so instrumenting the send path there - which IPI id, which
mailbox, and what the mbox registers say afterwards - separates "wrong channel"
from "nothing listening".


## Round 10: the whole IPI software path matches 4.19

Everything between nanohub and the mailbox tables was compared against the stock
tree this round, and it all matches.  Worth writing down so it is not re-checked:

* `mtk_nanohub_ipi.c` `ipi_txrx_bufs()` is byte-identical to 4.19 - same
  `scp_ipi_send(IPI_SENSOR, ..., 0, SCP_A_ID)`, same `SCP_IPI_ERROR` /
  `SCP_IPI_BUSY` retry loop, same 1000-retry bound.
* Both trees use the same `IPI_SENSOR` / `IPI_SENSOR_INIT_START` enum ids and
  register `mtk_nanohub_ipi_handler` on `IPI_SENSOR`.
* `scp_ipi_table_init()` in 6.6 reads `mbox-count` (the `scp_dt_*` helper
  supplies the `mbox_count` fallback, and the DTB has `mbox_count = 5`), and its
  `send_item_num = 3` / `recv_item_num = 4` match the 4.19 literals exactly.
  The DTB has no extra `#recv-cells-mode`, so the driver takes the same 4-element
  recv layout 4.19 used.
* Consequently the driver logs neither `[SCP] mbox count not found` nor
  `[SCP] scp send table not found` or `scp recv table not found`, and indeed
  round 41's log has none of them.

So the timeout is no longer a software mismatch in the nanohub driver, the IPI
id, the send flags or the table interpretation.  What remains is below that:

1. the 6.6 hard-IPI/mailbox layer itself (`mtk-mbox`, `mtk_tinysys_ipi`) against
   the 4.19 implementation the firmware was built for;
2. the ten tolerated `devm_ioremap_resource()` NULL failures on the SCP device at
   boot.  There are thirteen `devm_ioremap_resource()` sites in `scp_helper.c`;
   the ones that matter abort the probe and print `scpreg.<name> error`, and none
   of those lines appear.  That leaves sites that either ignore failure or run
   outside the probe, and a register window left unmapped on a path that only the
   send direction uses would produce exactly this asymmetry - ACKs arriving
   inbound while outbound traffic times out.

Instrumenting `mtk_nanohub_send_timestamp_wake_locked()` is therefore still the
right next move, but the question it should answer is now narrower: whether
`scp_ipi_send()` reaches the mailbox at all, and if it does, why the peer never
completes the transfer.


## Round 11: the send dies in pre_cb, before the mailbox

The log has been carrying the answer all along, in a line that is easy to skim
past because it does not say "sensor":

    Error: IPI [scp_ipidev_ipi#17] pre_cb fail        (repeating)

In the tinysys IPI framework each `scp_ipidev_ipi#N` has a `pre_cb` that runs
*before* the transfer is handed to the mailbox.  For the SCP device that callback
is set in `scp_ipi_table.h`:

    .pre_cb = (ipi_tx_cb_t)scp_awake_lock,

so the failing callback is `scp_awake_lock()` - the call that holds the SCP awake
so a transfer can be delivered.  When it fails, `scp_ipi_send()` never reaches
the mailbox and the sender times out.  That is precisely the asymmetry round 8
recorded: the hub-to-AP direction needs nothing from `scp_awake_lock` and works,
while every AP-to-hub transfer fails.

This also revises round 10's conclusion.  The ten tolerated
`devm_ioremap_resource()` NULL failures on the SCP device were called benign
there, on the grounds that SPMI and MCUPM show the same string and work.  That
reasoning was about *other* devices.  `scp_awake_lock()` walks SCP register
windows, so a window left NULL on the SCP device is exactly the kind of fault
that would make it return an error while leaving everything else looking healthy.

Two things also cleared this round, both by direct comparison with stock: the
mbox probe loop is identical (same `mtk_mbox_probe`, `enable_irq_wake`,
`mbox_setup_pin_table(i)` and the same `mbox%d probe fail` diagnostics, none of
which appear in the log), and `scp_ipidev (with 52 IPI) has registered` confirms
the IPI device came up.

Next: identify which SCP register windows are NULL, by instrumenting the
`devm_ioremap_resource()` sites or reading the resource indices they request, and
confirm `scp_awake_lock()` is returning an error because of one.  That is now a
much smaller and more concrete job than "the mailbox layer differs".


## Round 12: scp_ready never gets set, so scp_awake_lock refuses

Round 11 blamed the NULL register windows.  That was wrong, and the log says so
plainly once the right string is searched for:

    scp_awake_lock: SCP A not enabled
    [SCP] scp_crash_dump: awake scp fail, scp id=0

`scp_awake_lock()` fails at its *first* check:

    if (is_scp_ready(scp_id) == 0) {
            pr_notice("%s: %s not enabled\n", __func__, core_id);
            return ret;              /* -1 */
    }

and `is_scp_ready()` is just `scp_ready[id]`.  The register windows are never
touched, so the NULL ioremaps round 11 pointed at are not the cause of this.

The full chain is now complete and every link is evidenced:

1. the SCP firmware runs and talks to the AP - `SCP_INIT_DONE` reaches nanohub at
   about 2.3 s;
2. but the SCP's **notify** never reaches `scp_A_notify_ws()`
   (scp_helper.c:713), which is the only place that sets
   `scp_ready[SCP_A_ID] = 1` (scp_helper.c:739), so it stays 0;
3. `scp_awake_lock()` returns -1 at `is_scp_ready()`;
4. the tinysys IPI `pre_cb` fails - `Error: IPI [scp_ipidev_ipi#17] pre_cb fail`;
5. every AP-to-hub transfer dies before it reaches the mailbox, which is why the
   hub-to-AP direction works and outbound never does;
6. nanohub's power-up loop never completes, `hf_manager` keeps answering
   `Device:mtk_nanohub not ready`, and no sensor enumerates.

`scp_A_notify_work` is scheduled from exactly one place, scp_helper.c:850-857,
which also cancels the boot-timeout timer.  It sets `flags = 1` and calls
`scp_schedule_work()`; the `if (scp_notify_flag)` guard in the work handler then
takes the branch that sets `scp_ready`.

Next: identify that function and why it never runs - which IPI id carries the SCP
notify, whether the 6.6 driver registers that id, and whether the boot-timeout
monitor fired first and put the SCP into the reset/crash-dump loop the same log
lines hint at.


## Round 13: the ready handler looks correctly registered, so the likely fault is a race

`scp_A_set_ready()` has exactly one caller in the rv build: `scp_A_ready_ipi_handler()`
(scp_helper.c:889, calling at :895).  That handler is registered in two places,
for `IPI_IN_SCP_READY_0` and `IPI_IN_SCP_READY_1`, each guarded by
`mbox_check_recv_table()` and each logging `Dosen't support IPI_IN_SCP_READY_0/1`
when the guard fails.

Two comparisons came out clean:

* `IPI_IN_SCP_READY_0 = 9` in **both** trees, identically (scp_rv.h), so the id
  the driver listens on is the id stock listened on;
* the probe block also has a legacy branch - `if
  (mbox_check_recv_table(IPI_IN_SCP_MPOOL_0)) scp_legacy_ipi_init(); else
  pr_info("Skip legacy ipi init");`.  The port logs neither `Skip legacy ipi
  init` nor `Dosen't support IPI_IN_SCP_READY_0/1`, which is consistent with the
  legacy branch being taken (that branch is silent) and with the ready handler
  being registered successfully.

So the handler is on the right id and is registered, and yet `scp_ready` never
becomes 1.  That points at the notification being **missed rather than
mishandled**, and there is a natural race: the SCP is already running when the
kernel probes it (the same conclusion the `SCP_INIT_DONE` traffic at 2.3 s pushes
towards), so a "ready" notification sent early can be delivered before the AP has
registered the handler, and nothing re-sends it.  The boot-timeout monitor then
fires, and the log's `scp_crash_dump: awake scp fail` and repeated
`scp_awake_lock: SCP A not enabled` are the recovery loop that follows.

This is a hypothesis, not a measurement.  The way to settle it is to log
`scp_A_ready_ipi_handler()` entry and the `mbox_check_recv_table()` outcomes in
`scp_ipi_table_init()` on the next flashing round, rather than reading more code.
If the handler never fires while the SCP is demonstrably up, the fix is to treat
"SCP already running at probe" as ready - the state stock evidently reached -
instead of waiting for a notification that has already been and gone.


## Round 14: measured - the ready path works, the SCP is in a reset loop

Four `pr_info` probes went into `scp_helper.c` and a round was flashed to read
them (`b1-scp-log-1.txt` is empty because adb shell cannot read `dmesg`; the guard's
expdb log is where kernel console output actually lands, which is why all the
earlier SCP evidence came from there).  The probes answer the round-13 question
and kill its hypothesis:

    [SCP] nord2-dbg recv_table: mpool0=1 ready0=1 ready1=1
    [SCP] nord2-dbg ready_ipi fired: id=22 scp_ready=0 size=0x180000     t=3.2s
    [SCP] nord2-dbg notify_ws running: flag=1
    ...
    [SCP] nord2-dbg ready_ipi fired: id=22 scp_ready=1 size=0x180000     t=106.8s
    [SCP] nord2-dbg ready_ipi fired: id=22 scp_ready=0 size=0x180000     t=110.8s
    [SCP] nord2-dbg ready timeout fired: times=0                          every ~50s

So the notification is **not** missed and the handler is **not** badly registered:
`scp_A_ready_ipi_handler()` fires, `scp_A_notify_ws()` runs with `flag=1`, and
`scp_ready[SCP_A_ID]` really does reach 1.  Round 13's race hypothesis was wrong -
recorded here so it is not revived.

The real fault is that `scp_ready` does not *stay* 1.  It is set, then a reset
clears it again, and the cycle repeats: the boot-timeout monitor fires every ~50
seconds (`ready timeout fired: times=0`, and `scp_timeout_times` is reset to 0 by
the notify work, so it never climbs toward its limit), each firing resets the SCP,
and `scp_awake_lock: SCP A not enabled` at 43.5 s and 53.7 s lands inside one of
those windows.  The `IPI_SENSOR transfer timeout` and `pre_cb fail` errors are
downstream of this, not the cause.

Also worth having measured: the ready IPI arrives as **id 22**, not id 9, even
though the handler is registered on `IPI_IN_SCP_READY_0`.  All three
`mbox_check_recv_table()` results are 1, so the legacy branch is taken exactly as
round 13 inferred from the absence of `Skip legacy ipi init`.

Next: find what resets the SCP every ~50 s.  Candidates are the boot-timeout
monitor arming itself repeatedly (and never being cancelled because the
`del_timer` sits behind `SCP_BOOT_TIME_OUT_MONITOR`), a genuine SCP watchdog
timeout, or the recovery path re-arming.  `scp_send_reset_wq(RESET_TYPE_TIMEOUT)`
in `scp_wait_ready_timeout()` is the call that fires, so that function's timer
setup is the place to start.


## Round 15: the fix attempt failed, and it named the real culprit

The round-14 fix was to stop `scp_wait_ready_timeout()` resetting an SCP that is
already up.  It did not fire: the log has no `nord2: SCP already ready, skipping
boot-timeout reset` line, which means `scp_ready` is **already 0** by the time the
monitor runs.  The monitor is a consequence, not the cause.  Recorded because the
guard is still a defensible thing to have, but it does not fix this.

Following the writers of `scp_ready[SCP_A_ID] = 0` gives the answer.  There are
two, and the interesting one is `scp_sys_reset_ws()` (scp_helper.c:2190), reached
only through `scp_send_reset_wq()`, which has three callers:

* `RESET_TYPE_WDT` from `scp_A_notify_ws()` at :726 and :759
* `RESET_TYPE_TIMEOUT` from the boot-timeout monitor at :883

The TIMEOUT firings all come *after* the awake failures, so they are downstream.
The reset that actually kills the session is the **WDT** one, and the code around
it is specific: `scp_A_notify_ws()` sets `scp_ready[SCP_A_ID] = 1` at :739 and
then, if `scp_dvfs_feature_enable()`, runs

    while (!sync_ulposc_cali_data_to_scp()) {
            pr_notice("[SCP] cali #%d fail\n", ++cali_times);
            msleep(2000);
            if (... || cali_times >= 20) {
                    scp_send_reset_wq(RESET_TYPE_WDT);
                    return;
            }
    }

Twenty iterations of `msleep(2000)` is about **40 seconds**, and the first
`scp_awake_lock: SCP A not enabled` lands at **43.5 s** - the SCP becomes ready,
the ULPOSC calibration data fails to sync for ~40 s, the recovery path fires a
WDT reset, the reset clears `scp_ready`, and from then on every
`scp_awake_lock()` fails.  That is the whole sensor failure, measured end to end.

So the culprit is `sync_ulposc_cali_data_to_scp()` failing on this port, not the
IPI layer, not the mailbox, not `scp_ready` handling and not the timeout monitor.
Next: read that function and find why the sync never completes - the DVFS/ULPOSC
calibration source it depends on is the likely gap, since it is clearly not
failing for a reason the SCP's liveness would explain (the SCP is up and sending
ready IPIs throughout).  Making a calibration failure non-fatal is the fallback
if the source cannot be brought up, but understanding it comes first.


## Round 16: the calibration failure is confirmed, and made non-fatal

The log names the calibration fault directly:

    [scp_dvfs]: [mt_scp_dts_fmeter_get] Can't read fmeter-args-u2-cali
    [scp_dvfs]: [_get_ulposc_clk_by_fmeter_wrapper]: mt_get_fmeter_freq(36, 1) return 0, pls check CCF configs
    WARNING: ... at scp_dvfs.c:1566 _get_ulposc_clk_by_fmeter_wrapper

`mt_get_fmeter_freq(36, 1)` returns 0, so `ulposc_cali_process()` fails and sets
`cali_failed`, and `sync_ulposc_cali_data_to_scp()` then returns false
*immediately and forever* from that sticky flag (scp_dvfs.c:1495).  The caller's
loop of twenty `msleep(2000)` iterations could therefore never succeed - it just
burned ~40 s and then fired `scp_send_reset_wq(RESET_TYPE_WDT)`, which cleared
`scp_ready` and killed every AP-to-SCP transfer.  This also explains the SCS
unwinder warning that had been sitting on the backlog: it is this fmeter path
(scp_dvfs.c:1566), not `patch-scs.c` itself.

Fix applied: the twenty-iteration bound now logs
`ULPOSC cali unavailable, continuing without it` and breaks instead of resetting
the SCP.  A genuine SCP death still takes the `RESET_STATUS_START_WDT` branch.

Measured effect (round 43 -> 44, same round script):

    cali fail, do recovery              6 -> 0
    scp_crash_dump                     10 -> 5
    IPI_SENSOR transfer timeout         4 -> 1
    continuing without it               0 -> 5

So the cali-driven reset is gone, and it was worth landing.  **It is not
sufficient.** `scp_awake_lock: SCP A not enabled` is still ~2700 occurrences and
`Device:mtk_nanohub not ready` is unchanged at 20, and the sequence shows why:

    t=3.2s   ready_ipi fired scp_ready=0 ; notify_ws running flag=1
    t=43.5s  ULPOSC cali unavailable, continuing without it
    t=45.7s  ready_ipi fired scp_ready=0 ; notify_ws running flag=1
    t=86.0s  ULPOSC cali unavailable, continuing without it

`scp_ready` is 0 at every ready IPI, and the cycle is ~42 s - the 40 s cali loop
plus a reset right after it.  Something still resets the SCP at the end of that
window.  Note the boot-timeout monitor no longer fires at all now, so it is not
that; the likely remaining source is the WDT/recovery branch at the top of
`scp_A_notify_ws()`.

Next, two things: skip the cali loop on the first failure instead of spending 40 s
on a sticky flag that cannot change (the parenthesised comment in the caller says
as much: "the scp seems stop again, try to wait WDT" is the wrong reading when
`cali_failed` is permanent), and find what resets the SCP immediately after that
loop so `scp_ready` stops being cleared.
