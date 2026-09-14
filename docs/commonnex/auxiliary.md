# CommonNEX auxiliary schemas

Status: v0 design draft; not an implemented format or API.

[Overview](overview.md)

## Scope and record families

Receiver telemetry uses optional auxiliary schemas and can be persisted by
ParquetNEX independently of observations. Its absence does not affect Core
compliance. It covers receiver-reported estimates and diagnostics as well as
sensor readings. It is not limited to fields present in both UBX and SBF or
representable in RINEX. Preserve each available quantity at its native reporting
cadence; do not require an observation at the same instant.

Use separate typed families rather than a sparse universal table:

| Family | Contents |
| --- | --- |
| ReceiverClock | Reported bias, frequency offset and accuracy estimates |
| PulseTiming | Pulse identity, offset, quantization error and synchronization |
| ReceiverEnvironment | Temperature and sensor identity |
| ReceiverStatus | Uptime, boot/status and diagnostic fields |
| ReceiverSolution | Receiver-computed position/velocity/fix, not an independent survey |

Families share the following context; their full field catalogs remain open.
Do not duplicate low-rate temperature into every clock or observation sample.
Unlike RawBits epoch-only association, pulse and clock quantities retain the
time/reference information necessary to interpret their measurements.
Observation correction declarations/offsets belong in Events; source lock-loss
evidence stays on Observation. Auxiliary clock series replace neither.

Proposed common context for each telemetry record:

| Field | Type | Meaning |
| --- | --- | --- |
| `setup_id` | `string` | Logical station Setup; no mandatory telemetry row ID |
| `gpst` | `GpstTimestamp?` | Normalized sample or association time in GPST seconds |
| `observation_gpst`, `nav_epoch_gpst` | `GpstTimestamp?`, `GpstTimestamp?` | Optional justified context coordinates, not epoch-table references |
| `time_role`, `time_basis` | enum, enum | Sample/event/context time and source-field/association basis |
| `subject_id` | `string?` | Clock domain, sensor, or pulse output to which the record applies |
| `reference` | typed record? | Reference clock/time scale, nominal epoch, pulse edge, or sensor location needed by the quantity |
| `source_identity` | typed record | Source message/field identity and relevant revision; not a raw envelope |
| `quality` | typed record | Per-quantity validity, synchronization/lock, saturation, and applicable source status |

Telemetry payloads are typed quantity records with declared semantics; the
table below proposes their numerical fields. A transport may batch them or
group compatible quantities from one report. It need not use a sparse universal
table, string-valued measurements, or one callback per quantity.

| Quantity field | Type | Unit; scale | Semantic constraint |
| --- | --- | --- | --- |
| `clock_frequency_offset` | `int64?` | ppb; 1/1000000 | Positive when the receiver clock runs faster than the declared reference |
| `clock_bias_s` | `TimeDelta?` | s | Receiver time minus declared reference time |
| `navigation_time_offset_s` | `TimeDelta?` | s | Resolved navigation epoch minus its explicitly identified nominal epoch |
| `pps_offset_s` | `TimeDelta?` | s | Actual pulse edge minus its specified ideal edge, positive when late |
| `pulse_quantization_error_s` | `TimeDelta?` | s | Pulse-generator quantization error with a defined sign mapping, distinct from total PPS offset |
| `receiver_temperature_mdeg_c` | `int32?` | degC; 1/1000 | Physical temperature at least -273.15 degC; sensor location/meaning retained |
| `receiver_uptime_s` | `Duration?` | s | Reported uptime, not extrapolated or unwrapped; preserve source resolution and counter wrap semantics |
| `time_accuracy_s` | `Duration?` | s | Nonnegative; retain the source's accuracy definition, not implicitly a standard deviation |
| `frequency_accuracy` | `uint64?` | ppb; 1/1000000 | Nonnegative; retain the source's accuracy definition |

Time types follow the [Core semantic types](core.md#shared-semantic-types), all
backed by `DECIMAL(38,12)` seconds. Synchronization/association ages and pulse
periods also use `Duration` when defined, with periods strictly positive. Native
runtime counters may retain their original integer type separately; a normalized
duration does not remove rollover evidence. Non-time integer types and scales
remain agreed design choices, not measured fidelity guarantees.
For names with an explicit subunit suffix, that suffix identifies the stored
integer step (for example milli-degrees Celsius). Frequency fields omit a unit
suffix because their ppb scale must be read from the canonical schema.
Do not confuse an integer count of micro-ppb with a numerical ppb value.

These are initial semantic definitions for review. "Navigation time offset"
must not be a generic bucket for clock bias, a fractional TOW field, inter-system
time differences, and pulse errors. Preserve its nominal epoch/reference and
source interpretation. Add separate named quantities when those meanings differ.
Unknown mappings are reported, not silently relabeled under the closest name.
Reference time scales describe the measured quantity; all sample timelines and
partitions still use GPST. Do not relabel a bias relative to another system as
a GPST-relative bias without a justified conversion.

All offsets are signed `TimeDelta` measurements, not absolute `gpst` values.
Expected receiver-specific magnitudes do not impose generic schema bounds.
Pulse quantization error remains distinct from total PPS offset.

Convert source units directly into decimal seconds: ns multiplies by `10^-9`,
ps by `10^-12`, and ms by `10^-3`. Use checked wide arithmetic, never a large
absolute float intermediate. Values finer than 1 ps use half-to-even rounding
with reported precision reduction. Validate the decimal range and semantic
constraints; overflow produces null plus an explicit diagnostic retaining the
source value. Never wrap, saturate, or reinterpret errors modulo the pulse
period. This does not imply picosecond measurement accuracy or exact retention
of every binary floating-point source value.

At the proposed frequency scale, one stored step is 0.000001 ppb, or 1e-15
relative frequency. Nearest rounding adds at most 0.0000005 ppb error. This
may still round very small floating-point source values; it is not exact for
every possible SBF float. Decimal time quantization adds at most 0.5 ps rounding
error. Temperature adds at most 0.0005 degC
rounding error if its source is finer than the selected step. Validate these
budgets against supported sources before finalizing the mappings.

One ns/s is numerically one ppb; ppm converts to ppb by multiplying by 1,000.
For the proposed stored frequency integer, multiply a ppb value by 1,000,000,
or a ppm value by 1,000,000,000, using checked arithmetic and the rounding rule.
Do not equate a receiver clock estimate with an independently measured
free-running oscillator frequency.

Initial mapping evidence and limits:

- The existing UBX clock pipeline retains `NAV-CLOCK.clkB`/`clkD`, `tAcc`/
  `fAcc`, `NAV-TIMEGPS.fTOW`, and `MON-SYS` temperature/runtime. Reuse their
  interpretation, keeping fractional navigation time separate from clock bias.
  Monitor-message epoch association is not a temperature measurement timestamp.
  See [current receiver-clock semantics](../receiver-clock.md).
- `UBX-TIM-TP.qErr` is a picosecond quantization-error field concerning the next
  pulse, with validity and reference flags. It is not a direct measurement of
  total PPS offset. Convert its integer-ps value exactly to `TimeDelta` seconds and preserve pulse association;
  validate sign mapping before assigning the canonical error field.
  [u-blox time-pulse definition](https://content.u-blox.com/sites/default/files/documents/u-blox-F9-HPG-1.51_InterfaceDescription_UBXDOC-963802114-13124.pdf#page=211).
- SBF PVT `RxClkDrift` is in ppm and `RxClkBias` references `TimeSystem`;
  verify units for each block rather than all similarly named fields.
  `xPPSOffset.Offset` is in ns, negative for an early pulse; convert to canonical
  decimal seconds with the rounding/range policy above and retain `TimeScale`
  and `SyncAge` and distinguish its pulse from message arrival.
  [Septentrio clock and pulse definitions](https://docs.sparkfun.com/SparkFun_GNSS_mosaic-X5/assets/component_documentation/firmware/mosaic-X5_Firmware_v4.15.0_Reference_Guide.pdf#page=379).

Pulse port/edge, synchronization state, reference scale, configured delays,
counter rollover/saturation, and sensor location are retained when needed to
interpret the quantity. Temperature does not imply ambient or crystal
temperature. Missing telemetry remains absent/null; do not fill it from a nearby
sample during import. A downstream join may use a declared age limit while
retaining the original sample time and association age.

Clock-reset, steering, synchronization-loss, and runtime-continuity events must
survive import with reported versus inferred evidence distinguished. Unwrapped
clock bias, thermal fits, interpolation, and reconstructed oscillator histories
are derived analyses. Their parameters and outputs are separate from imported
receiver estimates, so they can be recomputed from ParquetNEX alone.
