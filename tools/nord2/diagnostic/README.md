# Minimal Nord 2 diagnostic ramdisk

These are the init assets used for the first confirmed DN2103 Linux 6.6.30
boots on 2026-09-13. They are not an Android ROM, charging environment, or
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

Display and charging drivers are not loaded by these assets. The first image
needed the ueventd correction above; it was not an unattended passing image.
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

Remaining display findings include DMA translation faults during the first
cold modeset, consistent with stale bootloader scanout addresses, and a TE
check timeout during a long static frame. Later modesets used the new mapped
buffers without fresh translation faults. These findings still need fixes;
visible bars alone do not establish full display stability or suspend/resume.

## Assembly constraints

Use a fresh copy of the working recovery ramdisk, keep `/system/bin/recovery`
as Android init's recovery-mode marker, and replace its init service scripts
with `init.rc`. Preserve `system/etc/ueventd.rc`: it is not an init service
script. These assets use no commands to mount persistent block filesystems.
The inherited recovery executable is not started.

Put the shell scripts in `/system/bin`, the module lists in `/nord2`, and the
recursive module dependencies plus depmod's text indexes in
`/lib/modules/<kernel.release>`. Keep `modules.load` empty: the two diagnostic
services load modules after the return timer starts. The selected USB/storage
closure contains 20 modules; check vermagic, modpost and GKI symbol access.
Use the installed recovery's root-ADB policy (`ro.adb.secure=0`,
`ro.debuggable=1`) only in this isolated USB diagnostic ramdisk.

BOOT has a 32 MiB partition and Android header v2, 2048-byte pages. The working
6.6 package used the recovery base DTB in the original BOOT DT-table wrapper,
with only the USB controller's `dr_mode` changed to `peripheral`; the existing
DTBO partition supplies the board overlay. It used a gzip-compressed Image.

Derive the kernel load address from the ARM64 Image header. The 4.19 kernel's
`text_offset` is `0x80000`; this 6.6 Image's is zero. The tested 6.6 boot header
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
