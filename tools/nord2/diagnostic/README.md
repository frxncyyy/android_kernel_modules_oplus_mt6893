# Minimal Nord 2 diagnostic ramdisk

These init assets extend the confirmed DN2103 Linux 6.6.30 USB/UFS boots
on 2026-09-13 with automatic display and FT3518 initialization. They are not an Android ROM, charging environment, or
complete image packager. They run with the binaries, linker configuration,
SELinux policy and recovery marker from the owner's working Lineage 22
recovery (2024-12-16). Recovery binaries, device trees, firmware, raw images,
private boot arguments and partition backups are not included here.

## Observed results

Kernel revision `bb18e2305d13cac2205279913f8c3d48a55b68e1`, device modules
`f1917cc398b44eb25e4568854df679f545cc7b2a`, Clang/LLVM 18.1.8, kernel release
`6.6.30-4k-gbb18e2305d13`:

- All eight CPUs online; diagnostic userspace and root USB ADB reached.
- MTU3 UDC `11201000.usb0` configured in peripheral mode.
- UFS enumerated all three normal LUNs and the 58 GPT partitions on the main
  disk. BOOT, RECOVERY, DTBO and vbmeta SHA-256 reads matched their expected
  images. This is not a filesystem, encryption or stress test.
- The first diagnostic returned automatically to the original 4.19 recovery
  through Android init and its boot-control message (BCB).
- A second boot with the preserved `ueventd.rc` produced partition nodes
  without any host repair and returned automatically to the original recovery.
  The first image had mistakenly removed that file;
  restoring it, restarting ueventd and replaying block uevents recovered the
  nodes during that test. Preserve its 16 MiB uevent socket buffer setting.
- No block filesystems were mounted during these diagnostic boots.

The current assets also load display and touch, as described below. Charging
drivers are not loaded. The first image needed the ueventd correction above;
it was not an unattended passing image.
Persistent console captures returned by recovery were stale 4.19 logs, despite
ramoops registering under 6.6. Until that path is validated, capture live dmesg
over ADB and check the kernel version in every saved log.

## I2C high-speed limitation

On this DN2103, the retained DT requests 3.4 MHz for I2C bus 5. With that
setting the 6.6 driver returns `0x08` for MT6360 PMU ID register 0x00 instead
of `0x53`, and the LDO provider refuses to probe. SMBus byte, combined I2C,
separate address/read transfers and SMBus I2C-block reads all reproduced it.
Using the existing driver parameter `force_speed=i2c5:400000` made all four
methods return `0x53`; the LDO driver then registered revision 3 and reported
VMC at 3.0 V. The chip-ID check was not bypassed.

Copy `modules.options` alongside `modules.dep` when extending the diagnostic
with I2C-dependent drivers. The early USB/UFS lists do not load I2C. This is a
verified bring-up limit for this board, not a fix or validation of the 3.4 MHz
path. Do not generalize it to other buses or claim high-speed I2C works.

## Panel binding and GPIO ownership

Loading the WL2868C alternate PMIC driver exposed a double release: its
managed enable-GPIO request was freed manually and again when chip-ID probing
failed. Module tracepoints showed one GPIO request and two releases, leaving
`pinctrl_mt6885` at reference count -1. All later GPIO consumers then deferred,
including the panel's bias GPIO.

With the manual free removed, the same absent-chip probe has one request and
one release. The pin-controller count returns to zero, and the panel then
holds two references for bias and reset. With the I2C setting above, the
YE05/non-PVT YE01 panel binds, `/dev/dri/card0` appears, and DSI reports connected
with two 1080x2400 modes.

A userspace DRM test subsequently allocated two XRGB8888 dumb buffers, performed
legacy modesets and page flips, and disabled the CRTC after each test. The
owner confirmed red/green/blue bars at 60 Hz with backlight level 512. Separate
60 Hz, 90 Hz and 60 Hz runs completed their modesets and flip events; DCS
register 0x0A returned 0x9F. Visible output at 90 Hz and seamless refresh
switching were not independently verified.

The atomic-begin error reporting called `drm_crtc_vblank_get()` again while
formatting an unconditional trackpoint message. This both leaked a reference
on successful flips and reported "invalid vblank:0". The corrected path calls
it once and reports only failures. The extracted-code regression test checks
success, failure and absent events, with trackpoint support on and off, and
rejects the original code. Rebuilt hardware runs no longer emitted that false
message and returned to the same disabled-CRTC reference count after repeated
modesets and flips.

The no-LK configuration skipped framebuffer import but still retained the
bootloader's first overlay layer on each pipe. First scanout then faulted at
the unmapped physical framebuffer address. First-enable now clears every
bootloader layer in this configuration before IOMMU translation and scanout;
the existing LK-adoption and idle-entry policies remain unchanged. A cold
60 Hz modeset, 120-second static frame, and subsequent 90/60 Hz modesets and
page flips completed without those faults. `test_boot_layers.py` exercises
both pipes, LK/no-LK builds and the preserved idle policy.

An earlier TE timeout followed an unintended diagnostic read of DCS register
0x10. Two controlled 120-second runs using only status register 0x0A did not
reproduce it. This is not sufficient evidence of a separate TE defect, nor
does it establish general display stability or system suspend/resume.

Repeated blank/unblank cycles exposed a touch IRQ bookkeeping defect, described below.

## FT3518 probe

The separate touch build produces `oplus_bsp_tp_comon`, `oplus_bsp_tp_custom`,
`oplus_bsp_tp_focal_common` and `oplus_bsp_tp_ft3518`. Include their recursive
dependencies from the device-module build and use the existing shared
`oplus_bsp_tp_notify`. Their modpost and GKI import checks passed.

The diagnostic `modules.options` supplies the FT3518 compatible string to the
custom selector. On this DN2103, the live I2C node is `focaltech,fts`; its
supported-project list contains 20827 and selects the Samsung panel variant.
The driver probes I2C address 0x38 on bus 0, reads chip ID 0x5452 and existing
firmware version 0x48, and registers `touchpanel` at event0. Display blanking
notifications reach its suspend/resume callbacks. No firmware blob was supplied
or flashed.

The legacy DT supplies an interrupt GPIO but leaves `i2c_client.irq` unset.
The common driver requested GPIO-derived IRQ 36, then probe completion copied
the client's zero back into `ts->irq`. First resume tried to free IRQ 0 and
re-requested the still-owned IRQ 36, producing a `devm_free_irq` warning and
`-EBUSY`. Registration now synchronizes the resolved IRQ into the bus client
and rejects invalid mappings. This also supplies the correct IRQ to the
FT3518 ESD callback, which uses the client directly.

With that fix, physical swipes produced 1,495 input frames before blanking
and over 1,000 fresh frames after repeated display off/on cycles. Observed
coordinates were within the advertised X=0..4319 and Y=0..9599 ranges;
multiple touch slots were reported. The first resume and later cycles no
longer warned or failed IRQ registration. `test_touch_irq.py` checks the
actual registration and probe-completion code for I2C/SPI, supplied and
GPIO-derived IRQs, and mapping/request errors; the original source fails it.
This verifies basic physical input and display-driven resume, not Android
input integration, precision, gesture wake or full system suspend.

## Battery and charger work in progress

A separate telemetry-only diagnostic read standard gauge registers at I2C
bus 7, address 0x55, three times under 6.6. It reported 54% SOC, temperature
3075 in tenths of kelvin (about 34.4 degrees C), and pack voltage 7941--7956 mV.
The values agree with the preceding 4.19 recovery readings of 54%, 34.3 degrees
C and 3974 mV per cell. No gauge control, unseal, calibration or flash commands
were used. This establishes basic gauge access; no 6.6 battery power-supply
integration or charging policy is validated.

The v2 tree does contain `oplus_hal_mp2650.c`, selected by the misleadingly
named `CONFIG_OPLUS_MP2762_CHARGER`. Compiling it exposed a missing pinctrl
consumer header and pre-6.6 I2C probe/remove callbacks. With those corrected,
the driver object and full vendor modpost/link pass when the option is selected
through Make. The standard board profile still leaves it disabled. Its runtime
initialization, required v2 DT properties, removal cleanup and charging limits
need work before it can be enabled on this board.

The MP2650 and BQ27541 v2 probes previously initialized hardware before reading
mandatory `oplus,ic_type` and `oplus,ic_index`. The gauge path could even reach
parameter/AFI handling before discovering that those properties were absent.
Both probes now reject incomplete bindings before allocation or hardware
configuration. Extracted-prefix tests cover every combination of missing
properties and reject the old ordering; full vendor modpost/link passes with
MP2650 selected. These rejection paths have not been exercised by loading the
whole charging stack on the phone.

The existing board DT lacks these v2 properties. The configured v2 module also
contains the MTK6991 charging shim matching generic `mediatek,charger`, plus
OP10 matching the legacy SY6610 node. Its dependency closure includes MT6379
and MT6373 drivers. Audit these bindings and translate the board's charging
configuration before runtime integration; do not treat a linked module as a
validated Nord 2 charging stack.

## Automatic display and touch loading

`nord2-display.sh` waits for USB ADB and the misc block node before loading
`modules.display` with its dependency closure and `modules.options`. It then
retries the unbound MT6893 DSI device, accounting for the upstream host's
child-device removal on deferred probe. This is an explicit loading-order
workaround; the DSI probe lifecycle still needs a driver-level fix.

The combined image initialized DRM and FT3518 without any host module pushes,
manual insmod or host-requested reprobes. `sys.nord2.display=ready` indicates
that `/dev/dri/card0` exists, and `sys.nord2.touch=bound` indicates the I2C
client has a driver. These properties do not replace scanout/input tests.
Host-run modeset tests and physical swipes provided the separate evidence
above. The init service itself does not draw a test pattern.

## Assembly constraints

Use a fresh copy of the working recovery ramdisk, keep `/system/bin/recovery`
as Android init's recovery-mode marker, and replace its init service scripts
with `init.rc`. Preserve `system/etc/ueventd.rc`: it is not an init service
script. These assets use no commands to mount persistent block filesystems.
The inherited recovery executable is not started.

Put the shell scripts in `/system/bin`, the module lists in `/nord2`, and the
recursive module dependencies plus depmod's text indexes in
`/lib/modules/<kernel.release>`. Keep `modules.load` empty: the two diagnostic
services load modules after the return timer starts. The early USB/storage closure contains 20 modules; the tested combined
USB/storage/display/touch closure contains 82. Copy `modules.options` into the
module directory and include `modules.display` in `/nord2`. Check vermagic,
modpost and GKI symbol access for the complete closure.
Use the installed recovery's root-ADB policy (`ro.adb.secure=0`,
`ro.debuggable=1`) only in this isolated USB diagnostic ramdisk.

BOOT has a 32 MiB partition and Android header v2, 2048-byte pages. The working
6.6 package used the recovery base DTB in the original BOOT DT-table wrapper,
with only the USB controller's `dr_mode` changed to `peripheral`; the existing
DTBO partition supplies the board overlay. It used a gzip-compressed Image.

Derive the kernel load address from the ARM64 Image header. The 4.19 kernel's
`text_offset` is `0x80000`; this 6.6 Image's is zero. The combined diagnostic occupied 33,259,520 bytes of the 33,554,432-byte BOOT
partition, so always enforce the partition-size check after packaging.
The tested 6.6 boot header
uses base `0x40000000` and kernel offset zero, ramdisk address `0x51100000`,
and tags/DTB address `0x47c80000`. The load address minus Image `text_offset`
must be 2 MiB aligned. Verify kernel and ramdisk bytes after unpacking the
finished image; do not merely copy the old kernel address.

The diagnostic command line adds `regulator_ignore_unused clk_ignore_unused`
to preserve resources whose consumers have not yet been loaded, together with
`androidboot.selinux=permissive panic=10 loglevel=7`. These are bring-up settings,
not production fixes for incomplete power or SELinux integration.

The installed device has unlocked verification and a separate recovery
partition. Original BOOT and RECOVERY were backed up and hash-verified before
writing BOOT. RECOVERY remained original during the successful 6.6 tests.
A direct `reboot("recovery")` request from the earlier 4.19 rescue did not
reliably select recovery on this firmware. The timeout script therefore uses
Android init, which writes the BCB through `/dev/block/by-name/misc`. It keeps
ADB available if that block device is absent. Early failure before userspace
can still require the physical recovery keys.
