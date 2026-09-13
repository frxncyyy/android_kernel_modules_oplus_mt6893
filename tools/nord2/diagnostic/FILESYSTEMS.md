# Read-only logical filesystem checks

The DN2103's installed ColorOS 15 port was inspected under diagnostic Linux
6.6.30 using its existing `super` partition. All 15 logical partitions mounted
read-only: 13 EROFS partitions and two ext4 partitions (`my_preload` and
`my_company`). Root directories were readable. Across their 60 physical
extents, all 120 beginning/end samples matched the verified local backup.
The mounted vendor fstab's SHA-256 also matched the extracted backup file.

The LP geometry, metadata-header and table checksums were validated before
using the layout. On-device metadata matched the backup used for the plan.
Each temporary dm-linear mapping used read-only mode, then checked its byte
size and `BLKROGET` state. EROFS mounts used `ro,nosuid,nodev,noexec`; ext4
also used `noload`, preventing journal replay. Every mount and temporary
mapping was removed, with no block filesystems left mounted. The diagnostic
returned to original recovery and original BOOT was restored and verified.

This establishes logical mapping and filesystem-read support. It does not
verify AVB enforcement, FBE, metadata encryption, unlocking or mounting
`/data`, filesystem writes, Android init, or the framework. The Android
startup integration must still replace the installed ROM's 4.19 modules and
validate its vendor services.

## Mapping helper

`dm_ro.c` creates only temporary read-only linear mappings backed by
`/dev/block/by-name/super`. Names must begin with `nord2-ro-`. It validates
numeric extent bounds, refuses an existing mapping node, verifies the
resulting read-only state and size, and removes a partially created mapping
on failure. It does not parse LP metadata or mount a filesystem.

Compile from the module repository after the normal build:

```sh
clang --target=aarch64-linux-gnu -Os -nostdlib -static \
  -fno-stack-protector -fno-builtin -fuse-ld=lld -Wno-unknown-attributes \
  -I../kernel-6.6/tools/include/nolibc -I../out-nord2/usr/include \
  tools/nord2/diagnostic/dm_ro.c -o /tmp/dm_ro
```

Obtain lengths and physical starting sectors from validated LP metadata
for the image actually installed. Preserve each partition's extent order.
The interface on the diagnostic device is:

```text
dm_ro create nord2-ro-NAME LENGTH_SECTORS SOURCE_SECTOR [LENGTH_SECTORS SOURCE_SECTOR ...]
dm_ro remove nord2-ro-NAME
```

The node is `/dev/block/nord2-ro-NAME`. Unmount it before removal. The helper
was first checked with a vendor EROFS mount in working 4.19 recovery, then
used for the complete 6.6 run. Device images, LP layouts and raw captures
remain private; no firmware or filesystem contents are included here.
