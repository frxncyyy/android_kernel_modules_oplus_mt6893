# Nord 2 Android display

The DN2103 owner confirmed a readable, working Android home screen with the
TE pinctrl ordering and cold-start fixes, and later confirmed that the black
interval between the bootloader splash and the boot animation was gone after
the seamless hand-off landed. This replaces the earlier result of full-screen
pixel noise and a stalled boot-animation frame.

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
- Hand the bootloader framebuffer to the CRTC on the first enable when
  `/chosen/atag,videolfb` says LK lit this display (`22740f2f`). The CRTC
  keeps LK's layer, rewrites its address to an IOVA and starts its trigger
  loop, so scanout never stops between the splash and the first Android
  frame. Without this the panel went dark for the gap the owner reported.
- Do **not** inherit LK's DSI or panel state in probe (`03c4ad38`). Adopting
  `output_en`, `clk_refcnt` and `panel->prepared/enabled` left the AMS643YE05
  DDIC unable to report TE after the display idle manager's first DSI
  power/ULPS cycle: the trigger loop parked on `EVENT_TE` (event 147), the
  next config packet hit the 1 s CMDQ timeout and only the ESD workaround
  recovered, by unpreparing and reinitialising the whole panel. Probe now
  inherits one thing only, the connector's "panel present" bit, and the
  kernel runs its own DSI + panel bring-up on the first encoder enable while
  the inherited splash is still on screen.

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
scanout, so V12 added the CRTC-side hand-off and the owner confirmed the
interval was gone; V13 corrected the boot DTB that the first hand-off build
had regressed. The hand-off initially also inherited LK's DSI/panel state,
which produced a deterministic post-boot failure (V13-V16): the trigger loop
parked on TE immediately after the idle manager's first ULPS cycle at ~32 s,
`cmdq` reported timeouts at 34.2/35.3/36.3 s, ESD reported `TE timeout` at
34.8 s and the panel was reinitialised at 36.4-36.7 s, which killed
surfaceflinger mid-boot. V14/V15/V16 diagnostics showed that `EXT_TE_EN` was
correctly programmed in `DSI_TXRX_CTRL` (0x0001023c) across the cycle, so the
register bit was not the cause; the inherited panel state was.

V17 dropped the DSI/panel inheritance and kept the CRTC hand-off. In the
capture, the kernel ran `mtk_output_dsi_enable` with `output_en 0` at 20.49 s
(full bring-up, panel init, 60-to-90 Hz switch at 20.64 s) and then completed
16 idle/power cycles between 32.3 s and 74.5 s with zero CMDQ timeouts, zero
ESD TE timeouts, no ESD recovery, no `DDPAEE` and no panic. Boot completion
was unchanged at 50-62 s and the fuel gauge stayed healthy
(`present=true`, `level=97`, `voltage=8638`). V18 is the same change with all
diagnostic instrumentation removed and is the revision described here.

The complete V11 guard capture contains 4,589 consecutive kernel records,
with zero sequence gaps, recorded overruns or truncation. It contains zero
IOMMU translation faults and CMDQ software timeouts; all eight mode switches
(including the final shutdown transition) completed. Original recovery was
reached automatically at 296 host seconds. BOOT, RECOVERY, the three vendor
blocks, expdb and the complete `super` partition were verified against their
original hashes after restoration. Every V12-V18 round ended the same way.

Regression tests compile actual driver code for synchronous binding and
probe failures, both LK handoff configurations, and PRIME device selection
with and without shared SMMU remapping. `test_lk_handoff.py` additionally
asserts at source level that the DSI probe inherits no output, clock or panel
state. Existing vblank reference, boot layer and YE05 panel callback/power
tests also pass.

These Android display tests use an independent recovery guard and a temporary wake
lock. Display blank/unblank tests under that guard do not validate system
suspend. Test images, device data and raw logs remain private. The installed
first-stage init and encrypted data are retained without formatting.

