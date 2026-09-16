# Nord 2 normal Android diagnostics

These are controlled startup results on DN2103 with the installed ColorOS 15
port and Linux 6.6.30. They do not establish a usable Android port.

## Results

| Attempt | Change | Result |
| --- | --- | --- |
| V1 | Installed first-stage init with 94 tested 6.6 modules | Normal Android ADB, encrypted data mount, vold, Trustonic, both zygotes and EGL initialization; SurfaceFlinger aborted at a rejected Mali job stride. |
| V2 | Preserve the MT6893 r49 job ABI | SurfaceFlinger stayed running; both boot-completion properties became 1; a screenshot captured the lockscreen. The owner reported a black physical screen. |
| V3 | Also load `leds-mtk-disp.ko` | The composer detected `lcd-backlight`, its range and brightness support. Android again reported boot completion, but display command-queue timeouts and framework restarts remained. Physical output was not confirmed. |
| V4 | Add CPU policies and standard ZY0603/MP2650 reporting | Boot completion, one observed framework PID and valid battery readings. Owner photo shows full-screen pixel corruption; loss of ADB at system suspend required a physical restart. |
| V5 | Integrated CPU/power build, diagnostic wake lock and debugfs/log accounting | Boot completion by 60 seconds, one framework PID through 256 seconds, valid battery readings, automatic recovery at 296 seconds. Display remains faulty; suspend was deliberately blocked. |
| V9 | Initialise TE pinctrl before synchronous binding; use the cold CRTC enable path consistently | Owner confirmed a readable, working Android screen; live frames and 60/90 Hz switching. Six startup faults remained in the inherited splash framebuffer. |
| V10 | Use the overlay DMA device for PRIME imports and segment limits | Android remained functional; the same six splash-region faults remained. |
| V11 | Clear inherited overlay layers in the cold default-path configuration | Owner confirmed normal output after three screen off/on cycles. Zero IOMMU faults and CMDQ software timeouts in the post-cycle capture; seven refresh switches completed. |
| V12 | Adopt LK's framebuffer and DSI state at runtime (`22740f2f`) | Owner confirmed the black interval between splash and boot animation is gone. The adopted boot DTB first dropped the gauge and charger; V13 corrected that. |
| V13 | Rebuild the board boot DTB instead of copying a recovery tree | Gauge and charger return (`present=true`, level 88-92). Display regression: 2 CMDQ timeouts and an ESD TE timeout on the first idle cycle. |
| V14 | Program `EXT_TE_EN` on the adopted output | Timeouts fell from 4 to 2; ESD TE timeout and recovery remain, so `EXT_TE_EN` is necessary but not sufficient. |
| V15 | Add caller tracing to the trigger loop | Isolated the idle manager: monitor thread `mtk_dsi_enter_idle`/`mtk_crtc_stop`, kick thread `mtk_dsi_leave_idle`/`mtk_crtc_start_trig_loop` at 32.11 s, then the loop parks on EVENT_TE. |
| V16 | Trace the DSI enable/idle register state | `EXT_TE_EN` is correctly set (0x0001023c) after the idle cycle and TE still never arrives; the ESD workaround recovers at 36.4-36.7 s by unpreparing/repreparing the panel. The inherited panel/DSI state, not the register bit, is at fault. |
| V17 | Keep the CRTC handoff, stop inheriting LK's DSI/panel state | 16 idle/power cycles between 32.3 s and 74.5 s with zero CMDQ timeouts, zero ESD TE timeouts, no ESD recovery, no DDPAEE or panic; boot completion unchanged; gauge healthy. |
| V18 | Clean build of the same change (diagnostics removed) | Same healthy capture: zero CMDQ and zero ESD TE timeouts, boot completion unchanged, gauge healthy. |
| V19 | Byte-identical rebuild of V18, watched by the owner | Owner reported a black interval of "almost 10 seconds" between splash and boot animation. The capture shows the kernel's panel re-init at 20.49-20.63 s and the first Android backlight write at 26.12 s, so the panel is dark for about 5.6 s. |
| V20 | Re-apply the remembered brightness (DCS `0x51`, 1023) right after the kernel's panel re-init | The write reaches the panel with zero CMDQ errors and the owner still sees the full interval, so the gap is missing content, not missing backlight. Both the V12 and V20 captures contain zero frame-completion events between 20.5 s and 26.0 s: the CRTC commits nothing across the hand-over, so only the frame LK left in the DDIC RAM can keep the screen lit. |
| V21/V22 | Inherit LK's panel but let the kernel program the DSI | Panel never reset ("Skipping prepare of already prepared panel") yet TE dies immediately: 38-40 CMDQ timeouts from 20.27-20.38 s, because the trigger loop waits for `STREAM_EOF` forever. Inheriting both sides or neither is the only consistent choice. |
| V23 | Inherit LK's DSI/panel state again and keep the idle manager off that DSI (`0c4fb0f7`) | Owner confirmed the hand-over seamless. Zero CMDQ timeouts, zero ESD TE timeouts, zero ESD recoveries, no `DDPAEE`, `boot_completed` at 32.9 s, gauge `present=true`, and zero idle entries for the whole round where V17+ entered idle about 16 times in 42 s. |
| V24 | Byte-identical rebuild of V23 plus two scripted screen off/on cycles | Validates the transition that hands the panel to the kernel and lets idle power saving resume. |

See [GPU notes](GPU.md) for the r49 fix and actual-header regression. The V2
kernel capture contained no job-stride rejection, Mali fault or IOMMU
translation fault. This is limited startup evidence, not sustained GPU
validation. A compositor screenshot does not prove physical panel scanout.

The existing `/data` mounted read/write through the encryption mapping, with
device-encrypted Android directories available. Credential-encrypted user
unlock was not tested. No data format was performed.

## Boot packaging and recovery

The diagnostic retained the installed first-stage init byte-for-byte. A small
PID 1 launcher started an independent logging/recovery child before executing
that init. The normal ramdisk loaded the tested USB, storage, display, touch,
Trustonic and GPU dependencies and the matching private Mali firmware.
`leds-mtk` alone supplies the shared LED code; the physical `disp-leds` DT node
also needs `leds-mtk-disp` to register Android's brightness files.

The legacy boot header, load addresses, working 6.6 Image and DTB were retained.
Image/ramdisk round trips, complete dependency closure, unresolved imports,
GKI access and duplicate exports were checked before each test. V3 contains
95 modules and occupies 22,702,080 bytes before partition padding.

Formatting fallbacks were temporarily disabled in the ramdisk and vendor
fstabs. The Trustonic daemon's explicit secure-storage reformat option was
also temporarily removed. The vendor edits occupied three individually
backed-up and verified 4096-byte blocks. They are diagnostic safeguards, not
production vendor changes.

The child requested recovery after 240 seconds and retained a 300-second hard
return. The source is now included as `android_boot_guard.c`. Its
`--selftest DIR` uses new regular files only; `--inspect` verifies the two
fixed diagnostic partition identities/sizes without writing them;
`--wake-test` acquires, verifies and releases only its named wake lock.
Build it statically for arm64 using the matching kernel nolibc and generated
UAPI headers. The metadata log path must be unique per run (O_EXCL), and
expdb must be freshly backed up before a guarded boot. From V5 the guard
holds a diagnostic suspend blocker, uses CLOCK_BOOTTIME and records kernel
ring overruns and local log truncation in header offsets 40 and 48.
These measures improve bounded observation; they do not validate suspend. It armed the existing recovery command without changing other BCB
payload bytes. Kernel logs initially used a fully backed-up expdb partition;
Android's crash collector also writes there, so subsequent attempts moved the
log to a new private metadata file once that filesystem was mounted.

V1–V3, V5 and V9–V18 returned automatically to the unchanged 4.19 recovery. V4 required
a physical forced restart after losing ADB at autosuspend. Original BOOT, the three
vendor blocks and expdb were restored and verified. After V3, the entire
`super` partition also matched its original backup hash. Every V12–V18 round was
restored the same way. Raw logs, firmware,
boot images and partition backups remain private.

## Remaining startup blockers

- **Display validation:** V11 fixes the observed corruption, TE stalls and
  inherited-framebuffer faults. V23 keeps the hand-off seamless and the display
  healthy by inheriting LK's DSI/panel state and holding the idle manager off
  that DSI until the kernel owns the panel; the owner confirmed it. See
  [display findings and checks](DISPLAY.md).
  Brightness calibration and AOD/HBM remain
  unvalidated. Screen off/on under the diagnostic wake lock is not system
  suspend validation.
- **CPU policies:** V4 supplies and loads the real CPU_DVFS implementation;
  policies 0, 4 and 7 exist and the earlier `OplusCpuInfoStore.parseCpuFreqType`
  crash no longer occurs in the captured startup interval. Sustained CPU/idle
  validation remains pending. See [CPU and power notes](CPU-POWER.md).
- **App groups:** V2 app starts aborted when libprocessgroup tried to write
  the absent `memory.app_uid` file. V3 reported
  `ro.config.per_app_memcg=false` and did not exercise that failing path.
  This does not constitute a fix for the missing Oplus interface.
- **Battery and thermal:** V4 reports a present battery with valid gauge
  readings and MP2650 input/status/fault information. Charging control,
  proprietary Oplus field compatibility and thermal policy remain incomplete.
- **System suspend:** V4 loses ADB at Android autosuspend and does not return
  automatically. Subsequent timed tests hold a diagnostic wake lock; this
  deliberately leaves suspend/resume validation separate.
- **Display wait loop and logging:** V3 hit repeated `-ERESTARTSYS` results in
  `mtk_crtc_gce_flush` while waiting for mode-switch completion. The loop
  retries an interruptible wait with a pending signal and floods the kernel
  log. Its capture has 45 sequence gaps totaling 486,287 missing records;
  absence of other errors in that capture is not a clean test result. The
  display debugfs snapshots were unavailable. Repair the wait handling before relying on a run which hits that loop.
  V5 provides debugfs state; its 5,362 kernel records have no sequence
  gaps, overruns or truncation. The V9–V11 display fixes avoid the observed
  mode-switch stall; signal-error handling in that loop is unchanged.

Wireless, modem/calls, audio, cameras, sensors, fingerprint, GNSS, NFC,
haptics and sustained suspend/load/charging behavior remain separate work.

## Image identity

These hashes identify local diagnostic artifacts; they are not downloads or
daily-use releases. These attempts used kernel release
`6.6.30-4k-gbb18e2305d13`.

| Padded BOOT image | SHA-256 |
| --- | --- |
| V1 | `f7e94a0b5bc97933c357ee5b20721ba5182ce52b4a8388605d7e5580e1af6867` |
| V2 | `99854b8d00a9a0231605a64bd01d56f9a371fba674f72d4366d712f34b6df978` |
| V3 | `ffee94549760565cd5e0973e558335fed8cf648fec3ba9d6b7d601e2ec776b6e` |
| V4 | `395fe1fc024df024d0a7868dbf803f4b1355712505cfab2711aacd610f314c74` |
| V5 | `e4bee36ff98b1f49c45cb62a5eda205447a1553335acce3ca45a7a36f48c7579` |
| V9 | `6d2df838ca83db8794bfc9352f731f36c5af4a54c412d83d0d15834c5e7f0959` |
| V10 | `147aa86f4f65628c6b53d8410727c98b97df22d6973c16dbc3174f256f598bc0` |
| V11 | `0a5d6074f3a261efcbe715247377333dfbe873714528508605e17ccd35de2dc0` |
| V12 | `b055707f8f07ccf578c61d2d6bc98de50bf82abfa07777317a3f2959732c864a` |
| V13 | `5b43aa2026e240cbdbd488486890ce695166aeaf04773ca661377dd2035647e6` |
| V14 | `61a60f7c9d36e6be8facdc5b2aa5b69d7d0d84c371ea278b8826d2e0f4b957bd` |
| V15 | `0e18b583cd9559c1043aa10177e23b4aa450ca3a90ec89f247427482bbeb2048` |
| V16 | `2eb0ab06e51b402e0f72db3c6443b36dfd39a42ddf851ba3cba573a76eeab5e1` |
| V17 | `698f41c4e014aa1fb6b576ff1dcb0e91fcfc8cf942011684a01e4a4b51d4cc2f` |
| V18 | `651e92f9c81f74c871bbdcb22f931cbe19c6ac5d122106fd7f366463ca400310` |
| V19 | `651e92f9c81f74c871bbdcb22f931cbe19c6ac5d122106fd7f366463ca400310` |
