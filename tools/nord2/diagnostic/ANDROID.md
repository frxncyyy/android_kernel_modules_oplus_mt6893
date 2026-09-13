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

V1–V3 and V5 returned automatically to the unchanged 4.19 recovery. V4 required
a physical forced restart after losing ADB at autosuspend. Original BOOT, the three
vendor blocks and expdb were restored and verified. After V3, the entire
`super` partition also matched its original backup hash. Raw logs, firmware,
boot images and partition backups remain private.

## Remaining startup blockers

- **Physical display:** the initial Android 60-to-90 Hz transition timed out
  waiting for a display command-queue event. Loading the brightness driver
  restores the expected interface but does not eliminate those timeouts.
  Its current default hardware range is 2047; the legacy silky-brightness DT
  uses `trans-bits=12` and requires a separate scaling audit against 4.19.
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
  V5 now provides debugfs state; its 5,362 kernel records have no sequence
  gaps, overruns or truncation.

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
