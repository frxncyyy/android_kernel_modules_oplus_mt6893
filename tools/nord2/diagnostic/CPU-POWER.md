# Nord 2 CPU policies and battery telemetry

Validated on the DN2103 with its installed ColorOS 15 port and retained
MT6893 vendor device tree. These changes do not complete the charging or
thermal-control port.

## CPU frequency policies

The first Android tests reached boot completion but `system_server` then
crashed in `OplusCpuInfoStore.parseCpuFreqType`: no cpufreq policy directories
existed. The CPU driver was built, but its module closure was not loaded and
the legacy `/mt_cpufreq` node lacked four regulator supply references.

`tools/nord2/prepare_dtb.py` resolves the actual `regulator-name` properties
for `6_vbuck1`, `7_vbuck3`, `vsram_proc1` and `vsram_proc2`, and adds their
phandles as supplies on `/mt_cpufreq`. It rejects missing, duplicate and
conflicting bindings before replacing the output. Do not copy numeric
phandles between device trees. The input must be a plain, device-owned DTB,
not the outer Android DT table.

```sh
python3 tools/nord2/prepare_dtb.py original.dtb prepared.dtb --power
```

`--power` explicitly identifies this Nord 2 tree with `oneplus,denniz` while
retaining the original root compatible strings. Use it only for a verified
Nord 2. The bootloader's vendor DTBO supplies the actual charger/gauge child
nodes, so changing those children in the base tree alone is insufficient.
Device trees can contain authentication material: keep all inputs, outputs
and boot images private. The output file is created with mode 0600.

Include `modules.cpu` and its full depmod closure, with PMIC/regulator and
I2C infrastructure loaded first. This brings in CPU_DVFS, MT6315, devinfo,
PPM/Upower, the MTK mailbox/IPI layer and MCUPM. The diagnostic registered
all three energy models successfully and exposed:

| Policy | CPUs | Maximum frequency |
| --- | --- | --- |
| policy0 | 0–3 | 2.0 GHz |
| policy4 | 4–6 | 2.6 GHz |
| policy7 | 7 | 3.0 GHz |

All three policies changed frequency under schedutil. The existing voltage
and OPP tables were retained. This is initial DVFS evidence, not validation
of every voltage, idle state, thermal limit or sustained load.

## I2C controllers without DMA

The charger is on legacy I2C11 at 0x5c. MT6893 controllers 10–12 have only a
controller register window and main clock, with `mediatek,fifo_only` in DT.
The 6.6 driver unconditionally required the absent DMA resource and rejected
all three controllers with -EINVAL.

The driver now honors that binding during resource/clock acquisition,
reset, register dumps and transfer selection. FIFO controllers expose their
eight-byte transfer limits and reject oversized or coalesced writes without
touching DMA. The ordinary DMA path remains available on the other buses.
I2C11 and its MP2650 client subsequently probed on the phone.

## Battery and charger status

`CONFIG_NORD2_POWER=m` builds `nord2-power.ko`. Include `modules.power` in the
normal boot module plan. The driver probes only a root tree identified as
`oneplus,denniz`. It uses the existing Oplus gauge/charger node aliases and
requires the gauge's read-only device-type query to return ZY0603 (0xa5ff).
It does not reset, unseal, calibrate, authenticate or flash the gauge.

The power-supply interfaces expose real BLP861 two-cell pack measurements:
voltage in microvolts, signed instantaneous current in microamps (positive
into the battery), charge in microamp-hours and temperature in tenths of a
degree Celsius. The ZY0603 instantaneous-current register is 0x0c, unlike the
older BQ27541 map. The 2S pack is not presented as a virtual single cell.
The installed 4.19 ROM supplied millivolts through `voltage_now`, resulting
in Android's standard health service reporting only 4 mV; that unit error
is not reproduced here.

MP2650 ACOK is REG13 bit 1, active high, as documented in the
[manufacturer's datasheet](https://www.monolithicpower.com/jp/documentview/productdocument/index/version/2/document_type/Datasheet/lang/en/sku/MP2650GV/document_id/9664/).
The legacy vendor header labels its polarity backwards. Charger phase,
faults and configured input-current limit are read directly. I2C failures
propagate instead of becoming a fabricated healthy or empty battery.

Combined CPU/power diagnostics reported 78%, 33.4°C, approximately 8.42 V,
positive charging current and 500 mA input limit, with both I2C clients bound.
The source getters and FIFO reset/length checks also pass host regressions.

**Charging control remains pending.** This driver reports status and leaves
charger configuration untouched. It does not implement JEITA limits, charge
termination policy, watchdog servicing, USB-PD/VOOC, gauge authentication or
board-wide thermal policy. Proprietary Oplus battery fields which overload
standard attributes still need a separate userspace compatibility audit.

## Android startup evidence

V4 reached `sys.boot_completed=1` by the host's 83-second sample. The same
system_server and SurfaceFlinger process IDs remained through the final
158-second sample, without the earlier CPU-frequency exception. Android
reported a present battery, 78%, USB powered, valid 2S voltage and temperature.
The owner's photograph showed full-screen pixel corruption. ADB stopped
when Android entered system suspend at approximately 149 seconds of kernel
uptime, and a physical restart was required. The saved kernel log contained
5,105 consecutive records with no sequence gaps; it did not record the
completion of suspend or a resume. Thus V4 does not prove suspend stability.

`android_boot_guard.c` subsequently gained a verified diagnostic wake lock,
a CLOCK_BOOTTIME deadline, and explicit log-overrun/truncation counters. It
mounts debugfs for display state captures. Holding this wake lock is only a
bounded test measure, not a production fix for power management.

V5 repeated normal Android with the integrated build and this wake lock.
Boot completion was observed by 60 seconds; system_server PID 1397 stayed
unchanged from the first 39-second sample through 256 seconds. No captured
fatal system-process exception or `parseCpuFreqType` exception occurred.
Battery reporting continued through 256 seconds (78%, 39.2°C in the final
sample). The guard requested recovery at its 240-second deadline; original
recovery reached the host at 296 seconds. Its 5,362 kernel records are
consecutive, with zero recorded overruns or truncated bytes. Original BOOT,
all three vendor blocks, expdb and the full super hash were restored/verified.
This validates the observed awake startup interval only.

Display diagnostics still show command-queue timeouts and stale compositor
output. The first captured plane is ABGR2101010, 1080×2400, pitch 4352 at
90 Hz. The legacy `0x0a00000000000001` modifier marks premultiplied alpha;
it does not by itself establish AFBC compression. Format, compression and
mode-switch handling need further isolation against the working linear RGB
diagnostic. The photograph alone cannot identify which of these is wrong.
