# Nord 2 Android connectivity (Wi-Fi and Bluetooth)

Wi-Fi and Bluetooth both run on the 6.6 port. The first Android round that
exercised them end to end (V29) completed a scan, associated and passed traffic,
and enabled Bluetooth, with no kernel complaint anywhere in the boot log.

## Round 29 evidence

- `[wlan][1924]SCANLOG: ... Call cfg80211_scan_done (aborted=0)` for the
  framework's first scan, and `cmd wifi list-scan-results` returned the
  surrounding access points (`FRITZ!Box 7530 TU_EXT`, `TP-Link_63A2`, ...).
- `cmd wifi status` reported `Wifi is connected to "FRITZ!Box 7530 TU"`,
  `Supplicant state: COMPLETED`, `Wi-Fi standard: 11n`, `RSSI: -64`,
  `Link speed: 130Mbps`, `IP: /192.168.178.78`, and 604 successful Tx packets,
  so the data path works, not just the association.
- `dumpsys bluetooth_manager` reported `enabled: true`, `state: ON`, the
  controller address in `XX:XX:XX:XX:F8:CC` form and the name
  `OnePlus Nord 2 5G`. The driver logged
  `main_driver_init: MTK BT Driver Version : 7.0.2020121701`,
  `main_init supported intf count <4>` and a clean power cycle
  (`BT turn on OK!` / `BT turn off OK!`).
- No `UBSAN` report and no panic appeared in the round's 1.6 MB `expdb` boot log
  or in any of the 26 `dmesg` snapshots.

## The scan bug that had to be fixed first

The first scan used to abort the kernel. `struct cfg80211_scan_request`'s
`channels[]` array carries a `__counted_by(n_channels)` annotation, so the
bounds checks generated for it compare every access against `request->n_channels`
— and both scan-request builders assigned that field only after filling the
array, leaving it at the zero that `kzalloc()` had put there. With
`CONFIG_UBSAN_BOUNDS` and `CONFIG_UBSAN_TRAP` enabled (this port enables
UBSAN), the first `NL80211_CMD_TRIGGER_SCAN` from `wificond` aborted:

```
Internal error: UBSAN: array index out of bounds: ... [#1] PREEMPT SMP
pc : nl80211_trigger_scan+0xba0/0xba8 [cfg80211]
Comm: wificond
```

and because the boot image carries `panic=10`, the abort became a reset and the
phone boot looped every ~95 seconds. Wi-Fi could never be used.

[`2a08123e2`](https://github.com/frxncyyy/android_kernel_oplus_op6893/commit/2a08123e2)
publishes the allocated capacity before the first store in both builders while
keeping the final "channels actually kept" store where it was, the order
`cfg80211_scan()` in `net/wireless/scan.c` already used for the same struct.

## Kernel configuration split

`tools/nord2/build.py`'s `nord2_bringup.config` selects:

- `CONFIG_RFKILL=y` and `CONFIG_BT=y`: both are kernel built-ins. The
  module-form exports (`hci_*`, `l2cap_*`, `rfkill_*`) are on the GKI
  protected-export list, while the built-in symbols are in
  `gki_module_unprotected.h`, which is the list the vendor modules are checked
  against. Building them in is therefore what lets `bt_drv_6893` link.
- `CONFIG_CFG80211=m`: GKI treats `cfg80211` as a module, and its symbols are in
  neither generated list, so it has to be shipped and loaded by the port like
  any vendor module. Building it in makes the port fail its own preflight with
  `__cfg80211_alloc_event_skb` and friends missing from the dependency closure.

## The shipped modules and their load order

`tools/nord2/build-connsys.sh` builds five modules; `connadp` and `btif_drv`
come from the device modules:

| Module | Role |
| --- | --- |
| `connadp` | kernel-side WMT/connectivity adaptor; exports the platform bridge and power-throttling hooks `conninfra` links against |
| `conninfra` | SoC <-> connsys transport (`/dev/conninfra`) every other module depends on |
| `connfem` | front-end module (ePA/eLNA) and its SKU table |
| `wmt_chrdev_wifi` | the `/dev/wmtWifi` character device the Android Wi-Fi HAL powers the chip on through |
| `wlan_drv_gen4m_6893` | the gen4m (connac2x, SOC3_0) Wi-Fi driver, installed as `wlan_drv_gen4m.ko` |
| `bt_drv_6893` | the BTIF character device `/dev/stpbt` and the Bluetooth firmware loader |
| `btif_drv` | the BTIF transport the Bluetooth driver sits on |

Init loads them as
`connadp, conninfra, connfem, wmt_chrdev_wifi, cfg80211, btif_drv, wlan_drv_gen4m_6893, bt_drv_6893`
— `connadp` first because it provides `conninfra`'s imports, and `conninfra`
before everything that talks to the chip through it.

Firmware comes from the stock vendor partition through the boot command line's
`firmware_class.path=/vendor/firmware`; the port does not ship Wi-Fi or BT
firmware blobs. gen4m is built with `CONFIG_MTK_WIFI_CCCI_SUPPORT=n` and
`CONFIG_MTK_WIFI_MDDP_SUPPORT=n` because this port has no modem stack, and those
two features are the only reason it would need `mddp.ko` and the
`ccci_md_all`/`eccci` modem modules at load time.

## Module caches and kernel version changes

A module's vermagic embeds the kernel release string, and that string carries
the kernel commit hash. Any kernel change — even a commit that only touches
`cfg80211` — therefore leaves every cached module in
`private/display-v11/staging`, `private/gpu-v7` and `private/cpu-v1` unable to
load, and the packager stages from those caches. After committing a kernel
change:

1. rebuild the kernel and device modules with `tools/nord2/build.py`;
2. `tools/nord2/build-connsys.sh` for the connectivity stack;
3. `tools/nord2/refresh-modules.py --work <workspace>` to re-copy the cached
   sets from the paths the manifests recorded and re-run depmod plus the GKI
   import/export checks;
4. `tools/nord2/check-modversions.py --work <workspace>` to confirm every
   shipped module still resolves against the kernel's symbol CRCs.

Skipping step 1 with a dirty kernel tree is worse than skipping a commit: the
release string then ends in `-dirty`, which no cached module matches either.

## Remaining work

- GPS (`gps/`) and FM radio (`fmradio/Build/connac2x`) are not built yet; they
  append one more `build` line each to `build-connsys.sh`.
- MDDP/CCCI stay off until a modem stack exists; revisit the two `=n` overrides
  then.
- The framework is fully functional well before `sys.boot_completed` appears
  (round 29 saw Wi-Fi associated and Bluetooth on while the property was still
  unset), and the boot animation runs long. That is a startup/performance
  question, tracked with the suspend work rather than with connectivity.
