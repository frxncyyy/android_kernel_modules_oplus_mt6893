# Nord 2 system suspend

System suspend works. Round 32 entered a real suspend and resumed, and round 33
ran 43 suspend/resume cycles without a failure. The blocker was never Android or
the kernel: it was the diagnostic boot guard's own wake lock.

## Round 31: the blocker

The guard is Android's `init` in this port, so it can read the kernel power
interface as root. Round 31 added an opt-in probe (a `nord2-power-probe` file in
the ramdisk, latched before `switch_root`; keys `probe_at`, `alarm_after`,
`mem_sleep`, `drop`, `hold_open`) plus `rtc-mt6397.ko`, the driver for the MT6359
PMIC RTC the boot DTB describes.

- `/sys/power/state` is `freeze mem`, so a write of `mem` is a real platform
  suspend, and `/sys/power/mem_sleep` defaults to `[deep]`.
- `/sys/power/wake_lock` contained exactly one name: `nord2-android-diagnostic`,
  the guard's suspend blocker, held since V5 so that a wedged round still returns
  to recovery. Round 30's "an Android wake lock kept the system awake" was right
  in effect and wrong in attribution.
- Every wake source a resumed system needs was already registered and idle:
  `pwrkey wakelock` (`mtk-pmic-keys.c` arms IRQ wake for `MTK_PMIC_PWRKEY_INDEX`),
  `mt6359p-rtc`, `alarmtimer.0.auto`, `kpd wakelock` and the WLAN/BT/conninfra
  sources. `disp_crtc0_wakelock` showed `active_since=0`, so the display was not
  holding the system awake either.
- The RTC works: `/sys/class/rtc/rtc0/name` is `mt6397-rtc mt6359p-rtc`, and an
  alarm armed 20 seconds out produced `set al time = 2026/09/16 11:50:51 (1)`
  followed by the consume message `set al time = 1970/01/01 00:00:00 (4)`.

## Round 32: the first real suspend

With `mem_sleep=s2idle`, the guard armed an RTC alarm 25 seconds out, released
the diagnostic lock at 105 seconds of uptime and was frozen by the freezer:

```
[  108.221120] PM: suspend entry (s2idle)
[  108.262489] [DISP]Disabling CRTC wakelock
[  108.278833] Freezing user space processes completed (elapsed 0.016 seconds)
[  111.276004] PM: pm_system_irq_wakeup: 359 triggered wlan0
[  111.291521] PM: suspend exit
```

The whole path works: the freezer completes, the display hands over, the PMIC
keeps time, and an incoming Wi-Fi packet wakes the system 3.07 seconds later.
`suspend_stats` went from `0/0` to `1/0` and stayed there. ADB drops while the
system is suspended because the USB gadget is suspended with it, which is what a
successful suspend looks like from outside.

## Round 33: it is stable, and it is the default

`mem_sleep_default=s2idle` now travels on the kernel command line, so the
`[deep]` default no longer selects the one path this port cannot yet finish. The
guard then released the diagnostic lock for a 90-second window, re-arming the
RTC alarm after every resume:

- `/sys/power/mem_sleep` reads `[s2idle] deep` at the probe, from the command
  line alone, with no sysfs write.
- 43 suspend/resume cycles completed: `suspend_stats/success` reached 32 by the
  time the window closed (the guard counts each resume it observes).
- Resumes were caused by both the RTC (`Resume caused by IRQ 283, mt6397-rtc`)
  and ordinary traffic, and every cycle re-froze and re-thawed userspace
  normally. The phone was fully usable afterwards.

Two details worth remembering:

- `suspend_stats/fail` reached 11 with `last_failed_dev=alarmtimer.0.auto`.
  Those are aborted entries - the kernel refuses to suspend when a wakeup is
  already pending - not hangs. Android retries and the system stays up.
- Suspended time cannot be measured as `CLOCK_BOOTTIME - CLOCK_MONOTONIC` in
  `s2idle` on this platform: both clocks advance together across it, which the
  per-cycle `boot_delta`/`mono_delta` values show. The kernel's own
  `PM: suspend entry`/`PM: suspend exit` timestamps are the honest measure, and
  the difference is expected to appear once a deep suspend is available.

## What is still missing

Deep suspend. `/sys/power/mem_sleep` offers `deep`, but the MediaTek platform
suspend stack that programmes the SoC wake mask and DDR retention is not part of
this port: the only callers of `suspend_set_ops` in the vendor tree are
`spm/common_v0/mtk_sleep.c` and `spm/common_v1/mtk_sleep.c`, gated by
`CONFIG_MTK_SPM_V0` and `CONFIG_MTK_SPM`, and neither is set. The vendor's own
MT6893 overlay instead enables `CONFIG_MTK_LPM_LEGACY=m`,
`CONFIG_MTK_LPM_MT6893=m` and `CONFIG_MTK_TINYSYS_SSPM_SUPPORT=m` with
`CONFIG_MTK_TINYSYS_SSPM_V2=y` while explicitly disabling the newer
`CONFIG_MTK_LOW_POWER_MODULE`; stock 4.19 built its LPM in with the MT6885
platform selected. Until that module set is built, loaded and proven, a `deep`
write is the shape of the one hang this port ever had (V4, before the diagnostic
wake lock existed), so `s2idle` is the deliberate default.

## Reproducing a suspend probe

The guard reads the probe settings before `switch_root` discards the ramdisk, so
a round only has to drop in a `nord2-power-probe` file next to `init`:

```
probe_at=100      # seconds of guard uptime before the probe
alarm_after=20    # arm the RTC this many seconds out (omit to skip)
mem_sleep=s2idle  # optional explicit memory sleep state
drop=1            # release the diagnostic wake lock (omit for a read-only probe)
hold_open=90      # keep re-suspending for this many seconds (omit to close after one)
```

A read-only probe (no `drop`) is always safe: it only reads and logs.

## Related work

The startup path is slow: the framework is fully usable (Wi-Fi associated,
Bluetooth on) well before `sys.boot_completed` appears, and the boot animation
runs long. That is the same "why is Android not idle" question from the other
end and belongs with this work.
