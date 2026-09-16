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
