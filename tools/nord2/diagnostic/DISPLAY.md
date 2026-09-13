# Nord 2 Android display

The DN2103 owner confirmed a readable, working Android home screen with the
TE pinctrl ordering and cold-start fixes. This replaces the earlier result
of full-screen pixel noise and a stalled boot-animation frame.

## Display fixes

- Initialise `private->pctrl` before `component_master_add_with_match()`.
  Binding is synchronous if all components are present. ESD IRQ registration
  puts GPIO 41 into interrupt mode and must restore its TE mux immediately.
  Previously that restoration ran with a null pinctrl handle. The minimal
  diagnostic loaded components in a different order and avoided the race.
  Setup errors now stop/defer probe before binding.
- Honour `CONFIG_MTK_DISP_NO_LK` in the DRM handoff decision as well as DSI
  probe. The first atomic enable must configure the complete path when the
  DSI driver has declined LK handoff. This flag is an empty header define;
  it requires `#ifdef`, rather than the integer-valued Kconfig test.
- Clear every inherited overlay layer in `mtk_crtc_config_default_path()`
  before engine start and IOMMU enable. The earlier no-LK layer clearing
  existed only in `first_enable_ddp_config()`, which cold enable bypasses.
  Active DRM planes are restored afterwards; the idle helper retains its
  existing first-layer policy.
- Attach PRIME-imported buffers to the OVL DMA device (or its shared SMMU
  device), matching dumb-buffer allocation. Legacy `dispsys` has no IOMMU
  attachment; using it for imports produces physical scatterlist addresses
  where the overlay requires an IOVA. Configure the maximum DMA segment
  size and clean up allocated DMA parameters on the same device.

## Evidence and checks

Before the pinctrl fix, the frame-trigger queue waited for TE event 147 and
the configuration queue timed out waiting for STREAM_EOF event 641. Only one
DSI interrupt was observed, despite ESD GPIO interrupts arriving. After the
fix, 1,328 DSI interrupts were counted in the early capture, the 60-to-90 Hz
switch completed, Android screenshots changed, and the owner confirmed
physical output. V10 with the PRIME correction still reported six faults;
those addresses all lay within the reserved bootloader framebuffer. Clearing
inherited layers in the cold-enable path removed those faults in V11.

V11 reached Android boot completion and the owner again confirmed a readable,
working physical screen after three display off/on cycles. The post-cycle
capture has zero IOMMU translation faults, zero CMDQ software timeouts, and
seven mode-switch starts matched by seven completions, exercising both
60-to-90 and 90-to-60 Hz. PRIME device selection is covered by source-level
regression; the Android test does not isolate use of that import path.

The owner reported a brief black interval between the splash and animation
on V9. Cold initialization does not preserve seamless bootloader framebuffer
scanout. This interval is distinct from the earlier persistent corruption;
seamless splash handover is not implemented. Brightness calibration, AOD/HBM
and prolonged panel testing are not established by these short startup tests.

Regression tests compile actual driver code for synchronous binding and
probe failures, both LK handoff configurations, and PRIME device selection
with and without shared SMMU remapping. Existing vblank reference, boot
layer and YE05 panel callback/power tests also pass.

These Android display tests use an independent recovery guard and a temporary wake
lock. Display blank/unblank tests under that guard do not validate system
suspend. Test images, device data and raw logs remain private. The installed
first-stage init and encrypted data are retained without formatting.
