# Nord 2 GPU diagnostics

Mali r49p1 now builds with the Nord 2 recipe. On DN2103, the driver identifies
GPU architecture 9.0.8 r0p1. A minimal 6.6 ramdisk completed these tests:

- Open `/dev/mali0`, negotiate JM ABI 11.0, 11.23 and 11.46, and read the
  749-byte GPU property table.
- Create and close a context for each ABI.
- Run 32 allocate/map/query/CPU-write-and-readback/unmap cycles per context,
  using buffers from 4 KiB to 1 MiB. All 96 cycles passed.
- Modeset and page-flip the display at 60/90/60 Hz with Mali and GED loaded.

No kernel warning, GPU fault or IOMMU translation fault was observed during
these runs. These checks cover initialization and mapped-buffer lifecycle;
the readback is performed by the CPU. They do not verify graphics rendering,
compute jobs, the Android graphics HAL, sustained load, system suspend or
thermal and battery limits.

## Firmware

The GPU requires `valhall-1691526.wa`. The file from this phone's installed
vendor partition is 440 bytes, uses workaround format version 2, and has
SHA-256 `1a52a7f3c7c8b15e13e226d0956091621a83baafd80a3f93debb8271110e5951`.
It was read from a verified local backup and supplied in RAM at
`/lib/firmware/valhall-1691526.wa` before opening Mali. No firmware partition
was flashed. The file is not included in this repository.

Without this file, module insertion still reports success and creates
`/dev/mali0`, but the first open fails with `ENODEV` when the driver tries to
load its required workaround. Device-node existence alone is not a passing
test. Use the matching firmware from the device's vendor image; do not disable
the hardware erratum check to make open succeed.

## Build and run the probe

From the module repository, after the normal build, compile the probe using
the same kernel source and output directories (adjust these two paths if needed):

```sh
clang --target=aarch64-linux-gnu -Os -nostdlib -static \
  -fno-stack-protector -fno-builtin -fuse-ld=lld -Wno-unknown-attributes \
  -I../kernel-6.6/tools/include/nolibc -I../out-nord2/usr/include \
  -Ivendor/mediatek/kernel_modules/gpu/gpu_mali/mali_avalon/mali-r49p1/drivers/gpu/arm/midgard/include/uapi \
  tools/nord2/diagnostic/gpu_probe.c -o /tmp/nord2-gpu-probe
```

The probe requires a 4 KiB page kernel, a working Mali device, and permission
to open it. In the diagnostic ramdisk it can be pushed to `/tmp` and run over
root ADB. It creates only temporary GPU allocations and does not submit
rendering jobs or touch block devices.

## Loading dependencies and remaining work

The display diagnostic supplies most shared dependencies. GPU testing adds
`spmi_mtk_pmif`, `mtk_spmi_pmic`, `mt6315_regulator`, `nvmem_mtk_devinfo`,
`clk_mt6893_mfgcfg`, `mtk_gpu_hal`, `mtk_gpufreq_wrapper_legacy`,
`mtk_gpufreq_mt6893`, `ged` and `mali_kbase_mt6893_r49`, in dependency order.
Use modules and dependency files from the same build. The ramdisk's automatic
module list does not yet include GPU support or its proprietary firmware.

The Nord 2 configuration now resolves both CMDQ and gpufreq to
`device-apc-common`. The inherited multi-platform configuration resolved CMDQ
to `device-apc-mt6765`; loading that alongside the GPU's required provider
failed with a duplicate `register_devapc_vio_callback` export. The MT6893
hardware driver now binds the legacy `mediatek,mt6885-devapc` node and passes
the GPU and display checks above. See [DEVAPC notes](DEVAPC.md) for the
startup fix, preserved runtime policy and remaining validation.

Power integration also remains incomplete. The port expects an
`efuse_pod19` cell absent from this DT. The 4.19 source contains a helper for
`efuse_ptpod22_cell` with different bit fields, but marks that helper
"do not use" and warns that PTPOD may change the voltage at low temperature.
Renaming the lookup alone would be incorrect. The default
41-entry frequency, voltage, SRAM voltage, divider and aging table matches
the 4.19 reference; calibrated DVFS remains unvalidated.
GED reports missing optional newer-platform nodes and
core-mask callbacks. Mali has no DT OPP table and continues without devfreq;
MediaTek's separate gpufreq driver does initialize. Existing bring-up code
also skips battery-throttling callbacks on MT6893. These limitations must be
resolved before claiming validated GPU DVFS or running sustained load tests.

## MT6893 job ABI

The first normal Android diagnostic reached the installed encrypted data and
created an EGL context with the installed r32p1 userspace library. SurfaceFlinger
then aborted during `eglCreateSyncKHR`, while the kernel rejected a 72-byte
`KBASE_IOCTL_JOB_SUBMIT` stride. Disassembly of the installed library confirms
that stride.

Disabling `CONFIG_MALI_MTK_GPU_BM_JM` had also removed `frame_nr` from the r49
userspace job structures, shrinking v2/v3 from 64/72 bytes to 56/64 bytes. The
r32 tree already preserved this extension for MT6893; r49 needed the same
board-specific ABI treatment. MT6893 now retains the field whether its platform
option is built in or modular. Bandwidth-monitoring code stays disabled, and
other platforms retain their previous layouts. `__u32` keeps the field valid
in userspace headers as well as kernel code.

`test_mali_job_abi.py` compiles the actual header with both platform forms and
with monitoring enabled/disabled. It pins the legacy record strides, frame
positions and job-chain/core-requirement offsets, and checks the generic
configurations. The previous header fails the MT6893 layout assertions.

After rebuilding r49 with this fix, a second timed Android boot kept
SurfaceFlinger running and reported `sys.boot_completed=1`. A screenshot
captured the ColorOS lockscreen using the existing encrypted data. The kernel
no longer reported unsupported job strides or Mali faults during the run.
This is initial evidence that the installed EGL library can submit jobs; it
does not establish sustained GPU stability.

The owner reported a black physical screen. The display command queue timed
out at the first Android 60-to-90 Hz switch, and the hardware composer could
not find the expected `lcd-backlight` brightness interface. A compositor
screenshot is therefore not evidence of successful panel scanout. Android
also repeatedly aborted app starts at the missing Oplus `memory.app_uid`
interface and reported an absent battery. The watchdog returned to recovery;
original BOOT, the three temporary vendor blocks and expdb were restored and
verified. No data format was performed.
