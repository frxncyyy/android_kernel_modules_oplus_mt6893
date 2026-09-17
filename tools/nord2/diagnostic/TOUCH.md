# Touchscreen (Focaltech FT3518)

## Summary

Touch is broken on the port because the FT3518 driver runs a firmware update that corrupts the
controller. The fix is **not** about making the firmware file reachable — that is what broke it.
The panel's own firmware is correct, and the port must stop the update from running.

## The mechanism

The driver only touches panel firmware if `request_firmware()` succeeds
(`touchpanel_common_driver.c:1527`). On failure it does `goto EXIT` and leaves the controller's
running firmware alone. On success it compares versions (`ft3518_driver.c:1710`):

```c
if (force || (buf[OFFSET_FW_DATA_FW_VER] != ts_data->fwver)) {   /* 0x010E */
        TPD_INFO("Need update, force(%d)/fwver:Host(0x%02x),TP(0x%02x)", ...);
        ret = fts_upgrade(ts_data, buf, len);                    /* erase + reflash */
```

When that mismatch fires, `fts_upgrade()` erases the controller and rewrites it, and the result
does not take: the chip ends up answering `0x00ef` instead of `0x5452`, with `boot id:0x545c,
fw abnormal`. The corruption is written into the panel and **survives reboots** — it is hardware
state, not a kernel state.

## What the two firmware sources actually provide

There are two different files with the same name, and they are not equivalent:

| source | how it is reached | version byte `0x010E` |
|---|---|---|
| `/odm/firmware/tp/*/FW_FT3518_SAMSUNG.img` | plain filesystem read | **`0x7e`** |
| Android userspace, via the `request_firmware` sysfs fallback | `bspFwUpdate` daemon | **`0x30`** |

`0x30` is the version this panel needs. All five stock panel directories (`20615`, `20817`,
`21061`, `21127`, `21881`) carry `0x7e` — the on-disk files are the wrong ones, which is why
stock itself never uses them.

## Stock's boot makes this visible

Stock hits the same filesystem failure the port does, and then recovers differently:

```
tp_fw_update_work: fw_name = tp/20817/FW_FT3518_SAMSUNG.img
Direct firmware load for tp/20817/FW_FT3518_SAMSUNG.img failed with error -2
Falling back to syfs fallback for: tp/20817/FW_FT3518_SAMSUNG.img   <-- userspace supplies it
Need update, force(1)/fwver:Host(0x30),TP(0x00)                     <-- correct version
ecc in tp:e6, host:e6                                               <-- checksum passes
upgrade success, reset to normal boot
read chip id:0x5452                                                 <-- controller repaired
fw update finished
```

The port has no `bspFwUpdate` daemon, so it never takes the fallback. Instead it read the on-disk
file, got `Host(0x7e)`, and reflashed into the bad state:

```
Need update, force(1)/fwver:Host(0x7e),TP(0x00)     <-- wrong file
erase now / write data to flash
ecc in tp:51, host:51                               <-- port's numbers, differs from stock's e6
upgrade success, reset to normal boot
read chip id:0x00ef                                 <-- still wrong
boot id:0x545c, fw abnormal
```

## Why the earlier "fix" broke touch

An earlier round concluded that touch failed because the firmware file was missing, and made it
reachable — first by appending `,/odm/firmware` to `firmware_class.path`, then by staging the file
into `/lib/firmware`, then by masking `/odm/firmware/tp`. Every one of those was aimed at the
wrong layer, and the first two actively caused the failure:

| round | firmware file | `fwver` seen | result |
|---|---|---|---|
| v100-v117 | absent (`-2`) | — (no update) | touch worked, controller untouched |
| appended `/odm/firmware` | found | `0x7e` | reflashed, corrupted, touch dead |
| staged in ramdisk | found | `0x7e` | reflashed, corrupted, touch dead |

Touch worked in v117 **because** the load failed. Making the file available is precisely what
donated the wrong version to the driver and destroyed the panel firmware.

This also corrects an earlier note in this file that blamed the `firmware_class.path` change for
"starving" the touch loader. The path change mattered, but the causation was the reverse of what
was written: the load *succeeding* was the problem, not the load failing.

## Recovery

Booting stock ColorOS repairs the controller, because stock's `bspFwUpdate` supplies the correct
`0x30` image through the sysfs fallback and the update then verifies (`ecc e6/e6`) and leaves the
chip at `0x5452`. A port round cannot recover this state on its own — it has to be recovered on
stock first.

## The correct fix for the port

Stop the update from running, rather than trying to feed it:

- Do **not** stage `tp/20817/FW_FT3518_SAMSUNG.img` into the ramdisk, and do not add paths that
  make the `/odm` copy reachable. Keep `firmware_class.path=/vendor/firmware` and leave the touch
  firmware unreachable so `request_firmware()` fails and the driver `goto EXIT`s, preserving the
  controller's working firmware.
- The robust version is to patch `ft3518_driver.c:1710` so an unknown/zero `fwver` never triggers
  `fts_upgrade()`, making correctness independent of file availability.
- Note this constrains the firmware-path work for other subsystems: haptics needs
  `/odm/firmware/*.bin` at the tree root. Touch needs `tp/` under it to stay unreachable. Those
  are compatible because they are different depths of the same tree, but any future change to the
  firmware search path must be checked against the touch request specifically.

## Probe log that distinguishes the two states

Healthy:   `read chip id:0x5452`, no `Need update`, no `erase now`.
Corrupted: `read chip id:0x00ef`, `boot id:0x545c, fw abnormal`, `Need update`, `erase now`.

The corrupted state shows up at probe (**1.95s**), before any firmware work — if `0x00ef` is there
at probe time, the panel was already damaged by an earlier boot and the round cannot fix it.
