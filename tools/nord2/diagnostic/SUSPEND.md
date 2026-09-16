# Nord 2 system suspend

System suspend has never been exercised by the diagnostic rounds, and round 31
found out why: the boot guard's own wake lock is the suspend blocker, and it is
deliberately held. The framework side is ready, the kernel exposes a suspend
state, and every wake source a resumed system needs is registered.

## Round 31 evidence

The guard is Android's `init` in this port, so it can read the kernel power
interface as root. Round 31 added an opt-in probe (a `nord2-power-probe` file in
the ramdisk, latched before `switch_root`; keys `probe_at`, `alarm_after`,
`drop`) plus `rtc-mt6397.ko`, the driver for the MT6359 PMIC RTC the boot DTB
describes.

- `/sys/power/state` is `freeze mem`. The arm64 PSCI driver registers platform
  suspend ops (`suspend_set_ops(&psci_suspend_ops)` in
  `drivers/firmware/psci/psci.c`), and `/sys/power/mem_sleep` is
  `s2idle [deep]`, so a write of `mem` means deep suspend.
- `/sys/power/wake_lock` contains exactly one name: `nord2-android-diagnostic`,
  the guard's suspend blocker (`active_count=1`, `active_since=109331`). This
  is the reason the HAL never writes `mem`: round 30's "an Android wake lock
  kept the system awake" was right in effect and wrong in attribution - the
  holder is this diagnostic lock, which every round since V5 has held on
  purpose so that a wedged round still returns to recovery.
- Wake sources are registered and none of them is stuck: `pwrkey wakelock`
  (`mtk-pmic-keys.c` arms IRQ wake for `MTK_PMIC_PWRKEY_INDEX`),
  `mt6359p-rtc` and `mt6397-rtc suspend wakelock`, `alarmtimer.0.auto`,
  `kpd wakelock`, and the WLAN/BT/conninfra sources, all with
  `active_count=0`.
- The RTC works end to end. `/sys/class/rtc/rtc0/name` is
  `mt6397-rtc mt6359p-rtc`, `since_epoch` tracks wall time, and an alarm armed
  20 seconds out produced `mt6397-rtc mt6359p-rtc: set al time =
  2026/09/16 11:50:51 (1)`, then the consume message
  `set al time = 1970/01/01 00:00:00 (4)` with `wakealarm` cleared. An RTC
  alarm is therefore available as an autonomous resume source.
- `suspend_stats/success` and `/fail` stayed `0` and no `PM: suspend entry`
  line exists in the round's 1.1 MB expdb boot log, while `dumpsys power`
  reported `mWakefulness=Asleep`. Android asked the system to sleep; the kernel
  was never asked to suspend.
- The kernel-side suspend preparation that does run is healthy:
  `[DISP]CRTC0 release wakelock mtk_drm_crtc_suspend ... cnt(1)` and
  `[wlan] priv_support_driver_cmd: driver cmd "SETSUSPENDMODE 1" on wlan0`.

## What is still unproven

Whether the deep suspend path actually resumes on this SoC. The MediaTek
platform suspend stack is not part of this port: the only callers of
`suspend_set_ops` in the vendor tree are `spm/common_v0/mtk_sleep.c` and
`spm/common_v1/mtk_sleep.c`, gated by `CONFIG_MTK_SPM_V0` and `CONFIG_MTK_SPM`,
and neither is set. The vendor's own MT6893 overlay instead enables
`CONFIG_MTK_LPM_LEGACY=m`, `CONFIG_MTK_LPM_MT6893=m`,
`CONFIG_MTK_TINYSYS_SSPM_SUPPORT=m` with `CONFIG_MTK_TINYSYS_SSPM_V2=y`, and
explicitly disables the newer `CONFIG_MTK_LOW_POWER_MODULE`. None of those
modules are built and shipped yet, so the SoC-specific sleep programming (wake
mask, DDR retention, SSPM handshake) is not in place and PSCI is the only
suspend op. The V4 run once entered a real suspend and never came back; its log
contains no completion and no resume, and it predates the diagnostic wake lock.

## Where to look next

1. Arm the RTC alarm, then release the diagnostic wake lock by building a round
   with `drop=1` in `nord2-power-probe`. Measure suspended time as the
   difference between `CLOCK_BOOTTIME` and `CLOCK_MONOTONIC` across the window;
   the guard records both, plus `suspend_stats/success`, `/fail` and the wake
   source table after the attempt.
2. If the attempt does not resume, the PSCI-only path is not enough on this
   platform and the MediaTek LPM/SPM/SSPM module set has to be built and loaded
   before the alarm is useful - the LPM drives the wake-up event mask.
3. Keep the screen-off path in the loop: `disp_crtc0_wakelock` was still active
   at the 110-second probe in round 31, so a manual screen-off before the probe
   is the honest way to test whether suspend is reachable.

## Related work

The startup path is slow: the framework is fully usable (Wi-Fi associated,
Bluetooth on) well before `sys.boot_completed` appears, and the boot animation
runs long. That is the same "why is Android not idle" question from the other
end and belongs with this work.
