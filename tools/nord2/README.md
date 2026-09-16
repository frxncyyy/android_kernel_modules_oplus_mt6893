# OnePlus Nord 2 (DN2103) bring-up

This is an experimental Linux 6.6.30 board port. A minimal diagnostic ramdisk
has booted on a Nord 2 DN2103 with all eight CPUs online, root USB ADB, and
UFS partition reads matching known hashes. The owner has confirmed a 60 Hz
colour-bar display test; 90 Hz modesets and page flips also complete. FT3518
reports physical swipes before and after display off/on cycles.
Normal Android, charging, cameras and remaining hardware need integration and
validation. A successful build is not a ready-to-flash phone image.
See [diagnostic notes](diagnostic/README.md) for the tested environment.
Read-only mounts of all 15 installed logical filesystems and 120 extent
samples also pass; see [filesystem checks](diagnostic/FILESYSTEMS.md).
The installed encrypted data mounts and Android reaches boot completion; the
splash hands over to the boot animation with no black gap and the display
survives the idle manager, both confirmed by the owner on V23. Sustained
operation and HBM still need validation; the vendor always-on display is
half-implemented and must stay disabled - with `Setting_AodEnable=1` the panel
enters doze and never comes back, so rounds 34-36 keep it off (the guard can
save/clear/restore it from root). See
[Android results](diagnostic/ANDROID.md) and [CPU/battery integration](diagnostic/CPU-POWER.md).

## Source baseline

- Kernel: `hzyry2046/android_kernel_oplus_op6893`,
  `bb18e2305d13cac2205279913f8c3d48a55b68e1`.
- Device modules: `hzyry2046/android_kernel_modules_oplus_mt6893`,
  `bringup/display-panel`, `a3e7bfe4c75be0cd37118b4b2267aae613d09e8b`.
- Panel reference: `mt6893-development/android_kernel_oplus_mt6893`,
  `lineage-23.2`, `306ad1404053789ffa154db022ee0d76a795923d`,
  `drivers/gpu/drm/panel/oplus20615_samsung_ams643ye05_1080p_dsi_cmd.c`.
  The three `oplus20615_mtk_data_hw_roundedpattern*.h` files come from that
  revision's `drivers/gpu/drm/mediatek/mtk_corner_pattern/` directory.

The [upstream thread](https://xdaforums.com/t/linux-kernel-6-6-for-realme-gt-neo-op6893.4800693/)
reports results on RMX3031 / RUI 4 EX.01, using the preserved 4.19 device tree
and modules in the boot ramdisk. Those runtime results do not establish Nord 2
compatibility. The exact upstream runtime configuration and ramdisk are not
provided by that thread.

## Build

Requirements: arm64-capable Clang/LLVM 18, make, a host C compiler, Python 3,
flex, bison, m4, OpenSSL and ELF development libraries, zlib/zstd, and pahole.
The initial build used Debian Clang/LLVM 18.1.8 and pahole 1.30. The upstream
Android compiler revision is `r510928`; Debian LLVM is not bit-identical to it.

Clone the kernel next to this repository as `kernel-6.6`, then run:

```sh
python3 tools/nord2/build.py --kernel ../kernel-6.6
python3 tools/nord2/tests/test_ye05.py
python3 tools/nord2/tests/test_vblank_reference.py
python3 tools/nord2/tests/test_boot_layers.py
python3 tools/nord2/tests/test_touch_irq.py
python3 tools/nord2/tests/test_charging_binding.py
python3 tools/nord2/tests/test_devapc_startup.py
python3 tools/nord2/tests/test_tee500_memory.py
python3 tools/nord2/tests/test_prepare_dtb.py
python3 tools/nord2/tests/test_build_boot_dtb.py
python3 tools/nord2/tests/test_i2c_fifo.py
python3 tools/nord2/tests/test_nord2_power.py
python3 tools/nord2/tests/test_key_reporting.py
```

The build script merges, in order, GKI, `mgk_64_k66_defconfig`,
`mt6893_overlay.config`, and `nord2_bringup.config`. It refreshes generated
headers before external-module compilation and verifies that every requested
Nord 2 setting survived Kconfig resolution. `--configure-only` checks that
stage without building the kernel and modules. Host include/library paths can
be supplied through `HOSTCFLAGS` and `HOSTLDFLAGS`.

After the device modules, it builds the Oplus FT3518 stack from its separate
`oplus_touchscreen_v2` Kbuild root. The Makefile-only configuration selects the
MediaTek platform, display notifier, touch common/custom code, Focal common
code and FT3518. It reuses the device-module notifier and symbol table rather
than building a second notifier with the same name and exports. The manifest
includes hashes of all four resulting touch modules. Firmware is not packaged
or updated by this build.

The final phase builds the MT6893 Mali r49p1 Job Manager driver and its memory
helper modules from the separate `gpu/mt6893` Kbuild root, using the same kernel
configuration and the device-module symbol table. Their hashes are included in
the manifest. Building the DMA-BUF test exporter does not load it on the phone.
GPU bandwidth monitoring remains disabled on MT6893 in both Mali and GED until
the SSPM QoS path is available; enabling only Mali's hooks left
`qos_get_frame_nr` unresolved at modpost.

The alert slider is built from its own `device_info/tri_state_key` Kbuild root
the same way. `oplus_tri_key.c` is only a symbol provider for the two MXM1120
hall IC drivers, so all three modules are built together and their hashes are
recorded; `CONFIG_OPLUS_MTK_DRM_GKI_NOTIFY` is defined in `KCFLAGS` because the
driver's fbdev notifier fallback is otherwise compiled but never referenced and
this tree builds with `-Werror`. The power key and volume up are reported by
`mtk-pmic-keys` from the PMIC interrupts, volume down by `mtk-kpd` from the
`VOLUME_DOWN-eint` GPIO, and the keypad matrix is deliberately left switched off;
see the [Android notes](diagnostic/ANDROID.md) for the module load order and the
evidence that all four controls work.

The Nord 2 profile selects the MT6893 DEVAPC architecture and excludes the
incompatible legacy providers. The multi-platform defaults exported
`register_devapc_vio_callback` from several modules, directing CMDQ to MT6765
while gpufreq required the multi-AO implementation. Loading both failed with a
duplicate-export error. The MT6893 driver also accepts the legacy DT binding
and initializes without treating inherited boot status as a fatal runtime
violation. GPU and display diagnostics pass with the driver bound; see
[DEVAPC validation and limits](diagnostic/DEVAPC.md).

The Nord 2 profile selects Trustonic 500 for the installed secure firmware's
MCI 1.8 / NWD ABI 8.3. Version queries and anonymous/cached DMA-BUF memory
registration work under 6.6. The 500 trusted-UI template and unrelated
Microtrust stack are excluded; keymaster, encrypted-data unlock and trusted
UI still require integration. See [Trustonic diagnostics](diagnostic/TEE.md).

By default output is `out-nord2` beside the kernel checkout. A custom `--out`
must be at the same depth below the common ancestor as the kernel checkout:
several vendor Makefiles interpret the module path relative to both source
and output. The script checks this requirement. Build logs and a manifest of
source revisions, dirty state, compiler, configuration and Image hashes are
written under the output directory. Vendor module objects are currently
written into the source tree by the upstream external-module build.

The script neither packages nor flashes a phone image. The profile excludes
unrelated display panels, factory charging tests, newer SoC cache/VM scheduling,
and the MT6991 audio card. The camera sensor list is intentionally empty until
the actual Nord 2 sensors are ported. The inherited charging v2 framework does
not implement this board's complete legacy MP2650/ZY0603 power path.

## Connectivity

Wi-Fi and Bluetooth run on the port. Round 29 completed the framework's first
scan, associated to the owner's network, took a DHCP lease and passed 604 Tx
packets, enabled Bluetooth, and recorded no UBSAN report and no panic. The
change behind it is
[`2a08123e2`](https://github.com/frxncyyy/android_kernel_oplus_op6893/commit/2a08123e2),
which assigns `cfg80211_scan_request::n_channels` before the `__counted_by`
annotation's bounds checks can read it; without it the first scan aborted the
kernel and the phone boot looped. `tools/nord2/build-connsys.sh` builds the
connsys stack, `tools/nord2/refresh-modules.py` re-stages the cached module sets
after any kernel change (their vermagic carries the kernel commit hash) and
`tools/nord2/check-modversions.py` checks every shipped module against the
kernel's symbol CRCs. See
[Connectivity diagnostics](diagnostic/CONNECTIVITY.md) for module roles, load
order, the firmware path and the remaining GPS/FM and MDDP work.

System suspend runs on the port. Rounds 32 and 33 released the diagnostic boot
guard's own wake lock - the only suspend blocker - and the system entered and
left `s2idle` 43 times without a failure, woken by both the PMIC RTC and ordinary
network traffic. `mem_sleep_default=s2idle` now rides on the kernel command line,
so the image does not fall into the `deep` path that needs MediaTek's SPM/LPM
stack. [Suspend notes](diagnostic/SUSPEND.md) record the evidence, the probe
interface, and why `deep` is still deliberately out of reach.

The RTC joined the shipped module set in round 31: the boot DTB describes the
MT6359 PMIC RTC as `mediatek,mt6359-rtc`, which `rtc-mt6397.ko` matches, and one
of its wakeup sources is what makes an unattended suspend probe recoverable.

## Boot DTB

The table behind the kernel is a 64-byte MediaTek header carrying exactly one
device tree. The stock table holds a plain `mediatek,MT6893` tree, and flashing
that tree unchanged loses two board facts the port depends on: the root
`compatible` that opts the machine into the OnePlus power monitor, and the four
CPU regulator supplies `mt_cpufreq` resolves. Without them `nord2-power`'s
`oplus,bq27541-battery` gauge and `oplus,mp2650-charger` port both return
`-ENODEV`, so Android reports no battery and the gauge daemon exits on every
retry.

`tools/nord2/build_boot_dtb.py` builds the table from the device's own DTB:

```sh
python3 tools/nord2/build_boot_dtb.py recovery.dtb boot-table-source.bin \
    base.dtb boot-table.dtb --power --usb-role peripheral
```

It runs `prepare_dtb.py --power`, forces the diagnostic USB role, and only then
rewraps the result, asserting the board compatible, the four supplies and the
USB role survived. Packaging that uses the plain recovery tree instead is what
dropped the fuel gauge; run the tool rather than copying a device tree into the
bundle by hand.

## Panel integration

The tested 4.19 device identifies AMS643YE05, but its display graph points to a
node named `oplus20615_samsung_ams643ye01_1080p_dsi_cmd`. The reference YE01
**non-PVT** and YE05 drivers contain the same 60/90 Hz command data. The new
module accepts both compatible names and binds only the connected node. The
YE01 PVT 60/120 Hz module is a distinct panel and must not be substituted.

The driver preserves the 98 command entries in ten active tables, including
the 129-byte DSC PPS command; its buffer includes the command byte. The 60 Hz
clock is corrected to 167670 kHz for the existing 1150 x 2430 totals; the 90 Hz
clock remains 251505 kHz. Refresh callbacks use connector mode contents instead
of assuming fixed mode indices. The seed callback uses the 6.6 packed-command
ABI. GPIO and regulator acquisition failures propagate, failed preparation
stays unprepared, and repeated power callbacks do not leak VMC enable counts.

`ldo3-supply` resolves to the legacy MT6360 LDO bank's VMC regulator. The profile
enables that provider and the board-information module used to choose the
external OLED rail. The global 8191 normal-brightness convention is preserved
from 4.19; mapping through the 6.6 Oplus display stack still needs runtime tests.
The removed 4.19 fingerprint down-delay global has no direct 6.6 counterpart;
HBM synchronization needs on-device validation through the OFP timing API, and
AOD is broken on the port: the panel's doze exit is called but the display never
resumes (see [SUSPEND.md](diagnostic/SUSPEND.md)).

Unlike the upstream PVT bring-up shim, this panel retains its imports from
`mediatek-drm` for brightness, seed state and LCD notifications. Its final
module dependencies are `oplus_bsp_boot_projectinfo`, `mtk_panel_ext`,
`mediatek-drm`, `device_info`, and `pmic_ldo_set_common`. The DSI host currently
unregisters its child devices on deferred probe; module loading must account
for a panel registered after the DRM module. Resolve this ordering before
claiming the panel can bind at boot.

Validation completed: panel object compilation with corner support disabled
and enabled; complete vendor modpost/link; unchanged active command-table data
against the pinned 4.19 reference; software tests for reversed connector-mode
order, invalid modes, packed seed commands, rail failures, retries and balanced
regulator references. These checks do not validate display electrical behavior,
brightness, fingerprint HBM, AOD, suspend/resume or image quality.
