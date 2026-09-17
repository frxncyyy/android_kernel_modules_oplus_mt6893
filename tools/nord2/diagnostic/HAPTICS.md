# Haptics (AW8697 vibrator) - OnePlus Nord 2 / MT6893, Linux 6.6 port

## Round 404: modules staged - change made, NOT YET VERIFIED on device

### What was already in place (all verified by inspection)

- The 6.6 tree already carries the driver: `vendor/oplus/kernel/vibrator/aw8697_haptic/`
  (`aw8697.c`, `aw8692x.c`, `aw869xx.c`, `haptic_hv.c`, `haptic_feedback.c`).
- It **builds**: `aw8697.ko` (809 KB), `haptic.ko` (1.37 MB) and `haptic_feedback.ko` (245 KB) all exist,
  with vermagic `6.6.30-4k-g2a08123e2d84+` - exactly the target release.
- Build config is on: `CONFIG_AW8697_HAPTIC=m` and `CONFIG_HAPTIC_FEEDBACK=m`.
- Module dependencies are satisfiable: `haptic` depends on `haptic_feedback`, which depends on `kernel_fb`
  (present in `modules.load`); `alt_cb_patch_nops` is provided by modules already staged (`aee_aed.ko` etc.).
- **Both DT nodes exist and bind correctly** in the live device tree:
    `/i2c9@11CB1000/aw8697_haptic@5A`  compatible `awinic,aw8697_haptic`
    `/odm/vibrator_fb`                compatible `oplus,haptic-feedback`

### The actual defect

Nothing ever **copied** those three modules into the staging directory, so they were in no load manifest and
the vibrator was dead. `grep -aiE "haptic|aw86|vibrat"` over `modules.load` and the staging dir returned
nothing at all.

### The change

`work/android-boot/package_v118.py`:

1. copy the three built modules into `moddir`, raising if any is missing;
2. add them to the load order: `order += ['haptic_feedback','haptic','aw8697']` (placed after the
   fmeter/SCP group and before `rtc_mt6397`, so `kernel_fb` is already up when `haptic_feedback` binds).

The package script is local tooling and is not tracked in either git repo, so there is no commit for it.

### Status: UNVERIFIED - the round did not complete

The round aborted before the boot test, so there is **no evidence yet that the vibrator works**. Three
freshness guards and one state guard fired in sequence, each correctly:

- `start_round.py` asserts the device is in **recovery** and that `expdb-before.img` is absent;
- `run_v118.py` asserts the round output dir does not exist (`mkdir(exist_ok=False)`);
- `flash_boot_diagnostic.py` asserts live `boot` matches the **stock baseline** before installing.

I also passed the round *number* (404) where the script takes the **version** (118); the version is the one
matching `preflight_v118.py` / `run_v118.py` / `package_v118.py`.

**A prior partial run had left the diagnostic boot image on the device.** That was caught by the guard, and I
restored stock directly from `private/partition-backup/boot.img` and verified against the manifest:

    boot      MATCH  (sha256 079d3d66c383c1ae...)
    recovery  MATCH
    phone     bootmode=normal  kernel=4.19.191+  boot_completed=1

So the device is healthy stock ColorOS.

### Resume procedure for the next round

1. `cd work && . ./build-env.sh` - **in the same shell** (provides `llvm-nm` on PATH).
2. `adb reboot recovery`, wait for `ro.bootmode=recovery`.
3. Move the previous round dir aside: `mv private/test-android-v118 private/test-android-v118.stale.$(date +%s)`.
4. `rm -f private/android-v118/expdb-before.img private/android-v118/expdb-backup.json`.
5. Because a partial run left the diagnostic image on `boot`, either run
   `flash_boot_diagnostic.py restore --bundle private/android-v118` first (it checks the state file), or
   re-flash stock boot from `private/partition-backup/boot.img` as was done here.
6. `python3 -u android-boot/start_round.py 118`.

Then verify the vibrator by reading the boot log for the AW8697 probe (`aw8697` / `haptic_hv` / i2c9 addr
0x5A) and by triggering a vibration through the sysfs/led-class interface.

## Round 405: the vibrator driver now probes successfully - one missing firmware file left

Staging the three modules worked.  The round completed `EXIT=0` and the boot log now shows the whole haptics
stack coming up (module counts in the log: haptic 54 lines, aw8697 29, haptic_feedback 2):

    [haptic_hv]aw_haptic driver version v0.0.0.13
    [haptic_hv]parse_dt: reset gpio provide ok 622
    [haptic_hv]parse_dt: irq gpio provide ok irq = 647.
    [haptic_hv]parse_dt: device_id=815
    aw8697_haptic 9-005a: aw8697_parse_dt: reset gpio provided ok
    aw8697_haptic_softreset enter
    aw8697_haptic_init enter
    aw8697_haptic_os_calibration enter
    aw8697_haptic_init get f0_pre=1700
    aw8697_i2c_probe probe completed successfully!

So the module dependency order I chose is correct and the **AW8697 really does respond on i2c9 at 0x5A** -
reset, init, calibration and the f0 read all succeed (`f0_pre=1700`).

**Two distinct drivers bind this node**, which matters:

- `haptic_hv` (from `haptic_hv.c`, via `haptic.ko`) probes first and **fails**: `ctrl_init: unexpected chipid`
  x5 then `ctrl_init failed ret=-22`, `probe of 9-005a failed with error -22`.  Reason: in `ctrl_init`
  (haptic_hv.c:4036) the cases for the plain chips are **commented out** -
  `//case AW8695_CHIPID:` / `//case AW8697_CHIPID:` - so only the AW8690x/AW8691x (aw869xx_func_list) and
  AW8692x (aw8692x_func_list) families are accepted, and this board's AW8697 (`AW8697_CHIPID 0x97`) is
  rejected with `-EINVAL`.
- `aw8697_haptic` (from `aw8697.c`, via `aw8697.ko`) probes second and **succeeds** - it is the driver that
  actually owns this part.

**The one remaining blocker is a missing firmware file**, not a driver defect:

    aw8697_ram_update: haptic bin name  aw8697_haptic_170.bin
    Direct firmware load for aw8697_haptic_170.bin failed with error -2      (ENOENT)
    aw8697_ram_loaded: failed to read aw8697_haptic_170.bin

`device_id == 815` takes the default `aw8697_ram_name[]` table (aw8697.c:81-88), which is
`aw8697_haptic_170.bin` for all five index slots.  The RAM waveform is what the chip plays for a vibration, so
without it the device probes and calibrates but cannot actually buzz.  The file is not in either kernel tree;
it normally ships in the vendor firmware partition, and the port's ramdisk/`lib/firmware` does not carry it.

### Next step

Obtain `aw8697_haptic_170.bin` (from the device's stock firmware, or the OPlus vendor firmware tree) and stage it
into the port's firmware path so `request_firmware` resolves it.  Then re-run the round and trigger a vibration
to confirm the motor actually fires.  Note `haptic_hv`'s chipid switch is a separate latent issue worth fixing
or leaving alone depending on which driver is intended to own the part - `aw8697_haptic` works, so the minimal
path is to keep it and just supply the firmware.

## Round 406: FIXED - the vibration firmware now loads and the motor is driven

The missing file was the whole defect.  `aw8697_haptic_170.bin` does exist on the phone, in the stock
**`/odm/firmware/`** tree (5828 bytes, among 893 AW8697 waveforms), but the port could never reach it:

- the diagnostic boot runs from the packaged **ramdisk** and never mounts `/odm`;
- `firmware_class.path` named only `/vendor/firmware`, and `/vendor/firmware` is a *different* directory on
  this device that does **not** contain the file;
- the ramdisk's own `/lib/firmware` was empty.

The loader searches `fw_path[]` = the ten `firmware_class.path` tokens followed by
`/lib/firmware/updates/<rel>`, `/lib/firmware/updates`, `/lib/firmware/<rel>`, `/lib/firmware`
(`firmware_loader/main.c:473-488`), so staging the file into `/lib/firmware` makes the round self-contained.

### The change

`work/android-boot/package_v118.py`:

1. stage `private/haptics-firmware/aw8697_haptic_170.bin` (the stock bytes pulled from the device,
   sha256 `45099d3faa0379a1aadf6f27add1ce05...`) into the ramdisk as `lib/firmware/aw8697_haptic_170.bin`,
   following the existing GPU-firmware line, and raise if it is missing;
2. extend the cmdline to `firmware_class.path=/vendor/firmware,/odm/firmware` - belt and braces, since the
   parser accepts comma-separated paths and `/odm/firmware` is the correct location if the port ever mounts
   `/odm`.

Both were verified in the built artifact before flashing: the name is present in the decompressed ramdisk, and
the new cmdline is present in the boot-image header at offset 180.

### Verification on the device

`ROUND EXIT=0`.  The `-2 (ENOENT)` is gone and the whole RAM path now completes:

    aw8697_ram_update: aw8697->haptic_real_f0 [170]
    aw8697_ram_update: haptic bin name  aw8697_haptic_170.bin
    aw8697_ram_loaded enter
    aw8697_ram_loaded: check sum pass : 0x0217
    aw8697_ram_loaded: fw update complete
    aw8697_haptic_trig_enable_config enter

Before / after:

    round 405:  aw8697_ram_loaded: failed to read aw8697_haptic_170.bin
    round 406:  aw8697_ram_loaded: fw update complete

### Notes

- `aw8697_rtp.bin` still reports `-2`, and that is **correct**: the file does not exist in the device's
  `/odm/firmware` either, so this matches stock.  RAM mode is the functional playback path and it is loaded.
- `haptic_hv` (from `haptic.ko`) still fails its probe with `ctrl_init: unexpected chipid` -> `-22`, because
  `ctrl_init` (haptic_hv.c:4036) has `//case AW8695_CHIPID:` / `//case AW8697_CHIPID:` commented out and so
  rejects the plain AW8697 (`0x97`).  That is harmless here - `aw8697_haptic` (from `aw8697.ko`) owns the part
  and probes successfully - but it is a latent duplicate-driver issue if `haptic.ko` is ever meant to serve it.
- `init_vibrator_proc: Couldn't create proc entry, 7672, ret -12` (`-ENOMEM`) is non-fatal; probe still reports
  `probe completed successfully!`.

## Round 407: functionally verified on the device - the motor is driven on command

The owner asked for a live test during a ported boot rather than accepting the firmware-load log alone.  Added a
haptics probe to `run_v118.py` that drives the motor while the ported kernel is running and captures the kernel's
response.  **It works.**

### Interfaces present on the running port

    /sys/class/leds/vibrator
    /sys/class/leds/vibrator_1
    /sys/bus/i2c/devices/9-005a/{driver,leds,modalias,name,of_node,power,ram,reg,subsystem,uevent}

`TIMED_OUTPUT` is **not** compiled into this build - the linked `aw8697.ko` exports no `timed_output` symbols -
so there is no `/sys/class/timed_output/vibrator/enable`, and `activate_store` (aw8697.c:8006) is the interface
that drives the **RAM** waveform that actually loads.  A first attempt wrote to the timed_output path and got
`rc=1` for exactly that reason.

### The verification

Writing the led-class `activate` attribute returned `rc=0` on both the start and the stop, and the kernel's own
log proves the motor was driven - counting occurrences in the boot log versus the post-trigger re-read:

    TS-aw869x-start   before trigger: 0     after trigger: 3
    config gpio       before trigger: 0     after trigger: 3

Zero during boot, and exactly three afterwards, matching the three writes the probe issues.  The driver's
vibration start routine (`OPLUS AW8697 haptics TS-aw869x-start......`, followed by
`TS-aw869x config gpio !` driving the motor pin) runs **only in response to the command**.

### A flaw in my own first test, worth recording

The first version of the probe ran *after* the round had already copied expdb, so the boot log could not
possibly contain the trigger's activity and the absence of trace lines was an artifact of my test order, not
evidence about the driver.  The probe now re-reads expdb into `guard-expdb-after-haptics.txt` after driving the
motor, and that file is what carries the three `TS-aw869x-start` lines.  This is the same
measure-the-wrong-artifact trap that has cost several rounds elsewhere in this port; the fix is to capture after
the action, not before it.

### Status

Haptics is **done**: the firmware loads and checksums (`fw update complete`, `check sum pass : 0x0217`), the
driver probes (`aw8697_i2c_probe probe completed successfully!`), the interfaces register, and a command now
provably drives the motor.  `aw8697_rtp.bin` remains absent, which matches stock (it is not in the device's
`/odm/firmware` either) and does not affect RAM-mode vibration.
