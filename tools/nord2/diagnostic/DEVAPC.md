# MT6893 DEVAPC on the Nord 2

The MT6893 driver now binds the DN2103's legacy
`mediatek,mt6885-devapc` node. The MT6885 and MT6893 INFRA/PERI/PERI2 device
tables and used register offsets match the 4.19 reference at
`306ad1404053789ffa154db022ee0d76a795923d`. Both compatible strings have
module aliases.

The common driver's device descriptor changed since 4.19: its first field
now identifies a power subsystem, rather than a slave bank. All three
MT6893 banks belong to INFRA and IRQ index zero. Designated initializers
preserve this distinction, including the original INFRA permission-query
type; PERI/PERI2 must not be interpreted as VLP/ADSP.

## Startup failure

Enabling the legacy binding initially caused a reboot during probe. Live
instrumentation localized it to the boot violation dump's fatal handler.
The device had already latched an APMCU ROM read at address `0x4`, slave
index 419. The origin of that earlier access is not established.

The 4.19 driver cleared inherited status before requesting its interrupt.
The newer common driver instead dumped boot status through its runtime
callbacks and fatal policy. MT6893 now opts into reporting and acknowledging
inherited status without those callbacks. This applies only to the dump
performed during probe. Runtime dumps and the ISR retain their normal
callback and exception policy. Other SoCs retain their existing boot-dump
policy unless they explicitly opt in.

IRQs are requested with `IRQF_NO_AUTOEN` and enabled only after the clock,
HRE state, boot dump and startup status initialization. Request failures
propagate instead of silently leaving a bound driver without its interrupt.

## Validation and limits

The vendor build and modpost pass. `test_devapc_startup.py` compiles the actual
dump and ISR with fixtures for all three banks, ordinary and type-2 status,
unknown masters, and both startup policies. It checks acknowledgement and
unmasking, retained runtime callbacks, failed IRQ requests, and delayed IRQ
enablement. Negative controls restoring fatal startup handling, automatic
IRQ enablement, or ignored request failures are rejected.

On a diagnostic 6.6 boot of DN2103, the platform driver binds and registers
Linux IRQ 335 / GIC 219. The inherited violation is reported and cleared;
`start_devapc` completes. With the driver active, Mali ABI 11.0/11.23/11.46,
all 96 buffer lifecycle checks, and 60/90/60 Hz display modesets/page flips
pass. DEVAPC reports zero runtime violations afterward, with `enable_KE`,
`enable_AEE` and `enable_WARN` still set to 1. No kernel warning, GPU fault or
IOMMU translation fault was observed in this run. Original BOOT and recovery
are preserved through the diagnostic's recovery return procedure.
Earlier boot logs still contain CMDQ debug stack dumps for unavailable
mailbox channels; those require separate integration work.

This does not establish runtime interrupt delivery under a real violation,
system suspend, or complete access-policy correctness. The firmware returns
`-1` for the newer subsystem-status SMC; the inherited common driver treats
that nonzero result as enabled. Explicit handling of that legacy firmware
interface remains to be audited. No access permissions or fatal runtime
settings were relaxed by this change.
