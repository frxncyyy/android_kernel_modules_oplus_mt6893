# Trustonic 500 on the Nord 2

The installed DN2103 firmware reports Trustonic 500a, MCI 1.8, NWD ABI 8.3,
trustlet API 1.20 and driver API 1.4. Its vendor image uses Trustonic keymaster
4.1 and gatekeeper services. The inherited Trustonic 600 selection requires
MCI 2.0 and exposes NWD ABI 10.0, including different GP shared-memory layouts.
The Nord 2 configuration now selects the matching 500 driver and excludes
the unrelated Microtrust implementation. The firmware version checks remain
intact.

The legacy TUI implementation is a template that does not hand ownership of
this board's display and input devices to the secure world. It is disabled
in the Nord 2 profile until that integration is implemented. Core Trustonic
communication does not establish that trusted UI works.

## Memory integration

The 500 driver had already received several kernel API updates, but its
DMA-BUF path still assumed an ION implementation. Changes for 6.6:

- Guard ION-only headers and exporter-private fields with `CONFIG_ION`;
  retain DMA-BUF support on DMA heaps.
- Use the registered platform device with a DMA mask matching its physical
  page-table format, instead of the placeholder device used for logging.
- Use the DMA-BUF map/unmap wrappers that acquire the reservation lock.
- Transfer buffer/attachment ownership only after successful mapping, so
  failed acquisition cannot leave freed pointers for the MMU destructor.
- Preserve acquisition errors, reject out-of-range descriptors and lengths,
  and require enough original scatterlist pages for each page-table chunk.
- Release partial `pin_user_pages` results with the matching unpin API.
  Mark output pages dirty when unpinning so file-backed writes can reach
  writeback.

`test_tee500_memory.py` compiles actual acquisition and cleanup code with
fault injection for descriptor lookup, attachment and mapping failures. It
also checks lengths, partial pin counts and output-page dirty accounting.
Negative controls restoring incorrect pin release, unlocked access to the
locked DMA-BUF API, or omitted size checks fail.

## Hardware checks

With 6.6 and the matching driver on DN2103, `MC_IO_VERSION` returns the same
firmware interfaces as working 4.19 recovery. A memory diagnostic completes
12 anonymous read/write registrations, one read-only input registration and
three cached system-heap DMA-BUF registrations, releasing each afterward.
Sizes include 4 KiB, 12,305 bytes and 2,101,248 bytes, crossing the 512-entry
page-table boundary. The Trustonic interrupt count increased from 1 to 33;
all tracked clients, buffers, sessions, mappings and MMUs returned to zero.
A subsequent boot of the complete rebuilt configuration also passed these
memory checks alongside DEVAPC, 96 Mali buffer cycles and 60/90/60 Hz display
tests. No new kernel exception or translation-fault markers were observed.

These checks register shared memory; they do not ask a trusted application
to process or modify its contents. No keymaster operation, encrypted-data
unlock, secure-storage format, firmware update or trusted-UI session was
performed. Uncached DMA-BUF coherency, file-backed secure writes, suspend and
the complete Android HAL path still require validation.

## Build and run the probes

After the normal build, from the module repository (adjust paths as needed):

```sh
for probe in tee_version tee_memory; do
  clang --target=aarch64-linux-gnu -Os -nostdlib -static \
    -fno-stack-protector -fno-builtin -fuse-ld=lld -Wno-unknown-attributes \
    -I../kernel-6.6/tools/include/nolibc -I../out-nord2/usr/include \
    -Ikernel/kernel_device_modules-6.6/drivers/tee/gud/500/MobiCoreDriver/public \
    tools/nord2/diagnostic/$probe.c -o /tmp/$probe
done
```

On the diagnostic device, load `mcDrvModule.ko` from the same build and run
`tee_version`. Run `tee_memory /dev/dma_heap/system` for anonymous and cached
DMA-BUF checks, or omit the argument for anonymous memory only. The memory
probe verifies MCI 1.8 / NWD 8.3 before using that ABI. Inspect kernel logs
and `/sys/kernel/debug/trustonic_tee/structs_counters` after the process exits.
Module insertion alone is insufficient: `/dev/mobicore-user` must exist and
the version query must succeed.
