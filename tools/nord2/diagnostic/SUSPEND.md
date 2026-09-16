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

## Always-on display is broken on the port (rounds 34-36)

The vendor always-on display is half-implemented on this kernel and must stay
disabled. With `Setting_AodEnable=1` the screen blanks into `DOZE` correctly -
`dumpsys display` reports `mState=DOZE`, `dozeScreenState=DOZE`, the OFP layer
logs `aod state is true` and the panel runs its doze entry - but it never comes
back. Pressing power again moves the framework to `mState=ON`,
`mBrightnessState=5862`, and `leds-mtk` even programs the backlight
(`Set lcd-backlight directly ... map:485`), yet the panel stays black. The
backlight value a shell can read is therefore *not* evidence that the display
recovered; only eyes on the panel are.

With the preference clear the port is fine: blank and unblank both work, which
is what the phone has been used with throughout bring-up.

What the logs show at the failed wake (`guard-expdb.txt`, the raw `/dev/kmsg`
copy):

```
[OFP] oplus_ofp_aod_off_status_handle:1618 - aod off status handle
[OFP] oplus_ofp_set_aod_state:462 - oplus_ofp_aod_state: 0
[DISP] mtk_dsi_encoder_enable doze status=1+
[DISP] doze early set powerdown,data =0
```

`oplus_ofp_set_aod_state(0)` runs *before* the `!new_doze_state &&
dsi->doze_enabled` block in `mtk_output_dsi_enable()`, so the OPLUS gate around
`doze_disable()`/`oplus_doze_disable()` in `mediatek_v2/mtk_dsi.c` is already
false and the LCM is never told to leave AOD. Round 36 replaced that gate with
an unconditional call. The panel module does implement every hook
(`doze_enable_start`, `doze_enable`, `oplus_doze_enable`, `doze_area`,
`doze_disable`, `oplus_doze_disable` are all present in
`oplus20615_samsung_ams643ye05_1080p_dsi_cmd.ko`), the call was made, and the
screen still stayed black - so the missing step is further down the resume path
(CRTC/DSI video restart, MM clocks, or a full panel unprepare/prepare cycle),
not the call itself. That change was reverted; it is not in the tree.

The stock 4.19 kernel runs the same panel, the same HAL and the same DTB (which
also lacks an `AOD-SCP-ON` node, so `aod_scp_flag` is 0 there too) and its AOD
does work, so the difference is in the 6.6 display build itself.

Round 37 contains it: `mtk_dsi_doze_state()` now reports doze as inactive, so
the panel is never put into the LCM doze mode at all.  The CRTC property and
the framework's AOD state machine are untouched - Android still enters `DOZE`,
`DOZE_SUSPEND` and `OFF` exactly as before - but a doze request now falls
through the DSI as an ordinary blank and the next wake is the ordinary enable
path that has always worked.  Verified with the preference *enabled*, the case
that used to go black: over two blank/wake cycles the log contains no
`panel_doze_enable`, no `doze status=1` and no doze enable, only the normal
`lcmoff` and `doze status=0+` of a regular screen-off, and the backlight returns
to its normal 488 afterwards.  What this costs is the AOD clock itself, which
never rendered anyway; with AOD enabled the screen now sleeps and wakes like a
normal screen-off.  Restoring the clock needs the real doze exit path, which is
still open; the former workaround of keeping the preference off is no longer
necessary. The guard can do it from a
root context, which ColorOS denies to both adb and the vendor shell, and it
preserves whatever the phone owner had chosen:

```
fix_aod=1     # save the current preference, clear it, restore it at the end
aod_test=1    # additionally turn it on for a second window (the failing case)
```

### What stock does that the port does not (round 37)

A stock 4.19 dmesg during a real AOD cycle is the reference; the phone ran it
while the stock vendor was up, so the same HAL, panel module and DTB were in
play.  Its AOD enter/exit sequence is:

```
enter AOD, disable PMIC LPMODE
debug for lcm panel_doze_enable
debug for panel_doze_enable normal aod light mode
oplus_panel_set_aod_light_mode, 0 to be 0
debug for display panel backlight value,func:=lcm_setbacklight_cmdq,level :=1, mapped_level := 4
enter aod mode, ignore set backlight to 1
...
debug for lcm panel_doze_disable, oplus_fp_notify_down_delay=0
debug for lcm panel_doze_post_disp_on
exit AOD, enable PMIC LPMODE
```

Against the port's unpatched AOD cycle (round 36 log) the differences are
concrete and three:

1. `pmic_ldo_vio18_lp` is live on stock for MT6893.  `mtk_drm_drv.c` calls it on
   both AOD edges - `pmic_ldo_vio18_lp(SRCLKEN0, 0, 1, HW_LP)` and the SRCLKEN2
   twin when entering, `(SRCLKEN0, 1, 0, HW_LP)` when leaving - so the display
   I/O rail is handed to SRCLKEN control for AOD and handed back on exit.  The
   port's `mediatek_v2/mtk_drm_drv.c` has the same four calls **commented out**
   behind `priv->data->doze_ctrl_pmic`, and the function does not exist in the
   6.6 tree at all (`pmic_lp_api.c` is absent; the 6.6 sibling comments name
   `clk_buf_voter_ctrl_by_id(12, SW_BBLPM/SW_OFF)`, which is also absent).  The
   6.6 tree does carry `drivers/misc/mediatek/srclken_rc/`, which is the
   SRCLKEN side of the same thing.
2. Stock's exit runs `panel_doze_post_disp_on` - the callback that turns the
   panel back on after doze - and `oplus_fp_notify_down_delay`.  The port logs
   neither (0 occurrences in the round-36 log).  `mediatek_v2/mtk_dsi.c` does
   call `panel_funcs->doze_post_disp_on` unconditionally in its doze config
   path, so the likely reason it never runs is that the port's panel module
   leaves that pointer NULL in its `mtk_panel_ext`, or that the port reaches a
   different doze path.
3. Stock's AOD is accompanied by the sensor-hub path (`lux_aodhub`,
   `chre_kthread`, `sensors@2.0-service`), which the port does not have at all
   - the same missing MTK sensor stack as the sensors backlog item.

The next round should test (2) first: it is the step that would literally turn
the panel back on, it is a one-pointer question, and unlike (1) it needs no
PMIC API that the 6.6 tree lacks.  (1) is the follow-up and is mostly a power
question, since a rail left in its normal mode is more power, not less function.
