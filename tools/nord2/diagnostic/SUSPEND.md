# Nord 2 system suspend

System suspend does not work yet. Round 30 established where the failure is, and
it is not in the kernel: Android never asks the kernel to suspend.

## Round 30 evidence

The probe turned the screen off, stayed silent for two long stretches so that ADB
traffic could not hold wake locks itself, and compared wall time with
`/proc/uptime`:

- `sys.boot_completed=1` was reached between 49 and 91 seconds, so boot
  completion is not the blocker; the `PowerManagerService.Booting` suspend
  blocker seen at 43 seconds is the transient one and is released.
- `/sys/power/suspend_stats/success` and `/sys/power/suspend_stats/fail` both
  stayed `0`, and no `PM: suspend entry` line appears anywhere in the round's
  1.3 MB boot log. The kernel was never asked to sleep.
- `dumpsys display` reported `Display State=ON`, `mState=ON`,
  `mCommittedState=ON` while `dumpsys power` reported `mWakefulness=Awake`, so an
  Android wake lock kept the system awake.
- The kernel-side paths that a suspend would exercise do work:
  `[DISP]CRTC0 release wakelock mtk_drm_crtc_suspend ... cnt(1)`,
  `[TP0]touchpanel: tp_suspend ts->bus_ready =1`,
  `[TP]focaltech,fts: MODE_GESTURE, Melo, ts->is_suspended = 1` and
  `priv_driver_set_suspend_mode` on `wlan0` all ran.
- `DisplayPowerController[0]: updatePowerStateInternal policy:DIM->OFF,
  state:ON->OFF` and `OplusPowerManagerHelper: [onGoToSleep]` show Android's
  screen-off path completing normally, so the failure is after `goToSleep`.
- The suspend HAL is declared and bound
  (`servicemanager: ... Found android.system.suspend.ISystemSuspend/default in
  framework VINTF manifest`), but no suspend request followed.

## Where to look next

1. Capture the full wake-lock list over time (`dumpsys power` without a filter,
   `dumpsys deviceidle`, and `/sys/power/wake_lock` once readable) at several
   points of a quiet window, to name the holder that survives `goToSleep`.
2. Capture the SystemSuspend HAL side (`logcat` for `SystemSuspend`,
   `SuspendControlService` and `wakeup_count`) to see whether the HAL is armed
   and whether its `wakeup_count` write is rejected, which is what a lingering
   wakeup source would do.
3. Only if both are clean, look at the kernel side again; the current evidence
   says the kernel is not the problem.

## Related work

The startup path is slow: the framework is fully usable (Wi-Fi associated,
Bluetooth on) well before `sys.boot_completed` appears, and the boot animation
runs long. That is the same "why is Android not idle" question from the other
end and belongs with this work.
