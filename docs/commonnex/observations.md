# CommonNEX Observation records

[Overview](overview.md) | [Shared types](types.md)

## Independent epoch times, without epoch tables

Measurement epochs and navigation epochs are distinct time contexts, not
required record tables. Observation stores its own `gpst`; RawBits stores
`nav_epoch_gpst`. Observation time is non-null; RawBits time is nullable.
Both use `GpstTimestamp` backed by
`DECIMAL(38,12)` seconds. RAWX/Measurements measurement time must not be
overwritten with navigation context. No one-to-one or equal-time relationship
is required between the two.

There are no mandatory epoch/occurrence IDs or row-to-row foreign keys.
Timestamps are coordinates, not unique keys: retain distinct occurrences with
equal times and payloads. Optional implementation row counters are file-local,
can be reassigned on reconstruction, and carry no cross-file/revision meaning.
Setup metadata references remain resolvable shared context.

The [Events family](events.md) carries completion and discontinuities.
Measurement-clock evidence belongs to [receiver telemetry](receiver-telemetry.md). Consumers load required event
context, possibly from preceding days, without per-row event references.
Observation quality remains signal-local. Events are not epoch lookup tables.

Skip untimeable Observation and report counts; preserve raw archives.
RawBits with unknown time is retained with null GPST under the
[receiver time association policy](receiver-time.md).
RawBits-only sources need no observations.
Completion closes the relevant producer epoch, not a promise of every signal
being received. Navigation completion does not close measurements or future
pulses; source-local completion does not imply merged-stream completion.

A complete UBX RXM-RAWX frame contains its measurement epoch and `numMeas`
records; validated structure completes that measurement record without NAV-EOE.
NAV-EOE closes navigation messages, not RAWX. SBF Measurements group completion
uses a matching EndOfMeas. Complete messages can span a group/file boundary;
missing optional observables do not make a structurally complete RAWX incomplete.

## Observation: one row per epoch/satellite/signal occurrence

The logical record and primary Parquet layout are wide, not scalar observable
rows. A normal row groups C/L/D/S of one system-specific signal. If genuinely
distinct or conflicting occurrences share those values, retain separate rows
and expose conflicts without treating timestamps as unique row identities.

| Field | Type | Meaning |
| --- | --- | --- |
| `setup_id` | string | Parent logical station Setup |
| `gpst` | GpstTimestamp | Source measurement time stored directly, not navigation-context time |
| `satellite_system`, `satellite_number` | string, uint16 | RINEX satellite identity |
| `signal` | string | System-specific RINEX band/attribute, such as 1C |
| `pseudorange_m` | float64? | C observable |
| `carrier_phase_cycles` | float64? | L observable |
| `doppler_hz` | float64? | D observable |
| `cn0_db_hz` | float64? | S observable only when its unit is known to be dB-Hz |
| `quality` | ObservationQuality? | Independent source validity and uncertainty fields |
| `tracking` | Tracking? | Half-cycle, lock duration and continuity evidence |
| `receiver_corrections` | ReceiverCorrections? | Source preprocessing corrections, neither applied nor undone by import |

### ObservationQuality

All members are nullable. A null struct means no quality report; it is not a
claim that the observations are valid. Units are explicit in field names.

| Field | Type | Meaning |
| --- | --- | --- |
| `code_valid`, `phase_valid` | bool? | Explicit source validity: true valid, false invalid, null unreported |
| `code_stddev_m` | float32? | Pseudorange standard deviation in meters |
| `phase_stddev_cycles` | float32? | Carrier-phase standard deviation in cycles |
| `doppler_stddev_hz` | float32? | Doppler standard deviation in Hz |
| `code_stddev_is_lower_bound` | bool? | Whether code uncertainty is a clipped lower bound |
| `phase_stddev_is_lower_bound` | bool? | Whether phase uncertainty is a clipped lower bound |
| `doppler_stddev_is_lower_bound` | bool? | Whether Doppler uncertainty is a lower bound |

Each stddev is finite and nonnegative. Its bound flag is null when stddev is
absent or bound semantics are unknown; false requires an established uncensored
mapping. Preserve per-observable bounds, not one row-wide saturation flag.
Decode source variance units and sentinels before taking the square root.
For SBF Doppler, derive the stddev from CarrierVar and DopplerVarFactor before
float32 storage; do not store variance or the factor as additional quantities.
No uncertainty is inferred from C/N0. There is no empty C/N0 quality struct or
Doppler validity declaration without a source field.

Source validity is independent of value presence. Finite in-domain measurements
marked invalid remain stored with false; source no-data values become null.
A numerical value does not make unknown source validity true. Scientific
acceptance thresholds belong to downstream processing.

### Tracking

All members are nullable; absent reports do not mean false or zero.

| Field | Type | Meaning |
| --- | --- | --- |
| `half_cycle_ambiguity` | bool? | Source reports unresolved half-cycle ambiguity |
| `half_cycle_subtracted` | bool? | Source reports half-cycle subtraction already applied to exported phase |
| `lock_duration_s` | Duration? | Source lock duration in seconds, or its saturated lower bound |
| `lock_duration_is_lower_bound` | bool? | False for an ordinary reported duration; true for a saturated lower bound |
| `continuity_counter` | uint32? | Source-reported continuity-change counter |
| `continuity_counter_modulus` | uint32? | Counter modulus, greater than one |

Lock duration and its bound flag are supplied together or both null. An ordinary
reported duration retains source quantization; it is not an exact continuous
measurement. There is no interval upper bound or separate representation enum.

Continuity counter and modulus are supplied together, with counter less than
modulus. SBF MeasExtra uses modulo 256; acquisition and cycle slips can both
increment it. Preserve the value without unwrapping, inferring receiver restart
or manufacturing a loss-of-lock boolean. An unchanged counter does not establish
continuity across a gap. Arc/slip inference belongs to processing facilities.

### ReceiverCorrections

| Field | Type | Unit |
| --- | --- | --- |
| `code_multipath_m` | float64? | m |
| `code_smoothing_m` | float64? | m |
| `phase_multipath_cycles` | float64? | cycles |
| `code_smoothing_applied` | bool? | Source explicitly reports whether pseudorange is smoothed; null means unavailable |

The three numerical fields are signed finite amounts to add to the receiver's exported observable
to undo the corresponding preprocessing correction. Import retains the exported
code and phase; it does not add these values back or confuse them with clock
corrections. Null is unavailable; zero is a reported zero, not proof that the
feature is disabled. Numerical fields use physical-unit float64 values, not source-specific
scaled integer encodings. These non-time corrections are an explicit exception
to the integer-first preference, like the observables they accompany.
The boolean is independent: false means explicitly unsmoothed; null means no
declaration. Preserve it even when every correction amount is unavailable.

### Tracking interpretation

Half-cycle ambiguity and subtraction are independent. Subtraction reports an
operation already performed, not an instruction to subtract again. See the
[adapter mapping](receiver-mappings.md#observation-quality-mapping).
Do not infer slips from phase jumps or invent half-cycle subtraction from lock resets.

Absent fields and source no-data sentinels become null. Zero is a numerical
value, not a universal sentinel. Finite in-domain but unreliable measurements
remain numerical values with quality flags. Do not create a general missing-
reason taxonomy; retain explicit diagnostics only where interpretation needs it.
Missing C, L, D or S does not invalidate the remaining columns. Consumer
requirements such as dual-frequency TEC do not constrain Core compliance.

Only dB-Hz signal-strength observations are stored in `cn0_db_hz`; do not
guess units or synthesize signal-strength ranks.
Derive full C/L/D/S codes from system and signal only when justified; do not
force unlike signal attributes into one row.

## Normalization and processing boundary

Apply receiver-native unit/time conventions to obtain physical values. Preserve
phase convention and already-applied correction metadata with explicit scope.
Do not apply or undo receiver clock corrections during import. Accept only
inputs without applied observation clock-offset correction and retain the
source's consistent epoch/code/phase values. GPST time-scale
normalization is not receiver clock-error removal. NAV-CLOCK and SBF PVT clock
bias/drift remain telemetry; never use them to fill or correct raw observations.

There is no general clock-correction application-state mechanism. Preserve
internal receiver clock-steering/integer-millisecond adjustment evidence in
telemetry, without inferring reboot, loss of lock or adjustment size.
SBF smoothing state remains known even without MeasExtra correction amounts;
UBX has no equivalent declaration here and uses null, not false.
Import does not smooth, interpolate, repair slips, unwrap clocks or estimate
missing observations. Inferred arcs and scientific corrections are outputs of
processing facilities, not mutations of imported records.

Shared continuity events identify stream, explicit time scope, event kind
and reported/inferred evidence. They must distinguish actual restart/loss from
transport completion; batch boundaries have no scientific meaning.
Selected Events semantics are defined in [Events](events.md); exact payload
encodings and source mappings remain review items.

See the [GPST policy](../time-policy.md) for project-wide time interpretation.

## Reference

- [RINEX 4.02 specification](https://files.igs.org/pub/data/format/rinex_4.02.pdf):
  identifier and semantic reference, not a claim of complete adapter support.
