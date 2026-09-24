# CommonNEX Core

Status: selected v0 design with implemented UBX/SBF and RTCM3 observation subsets; see [importer](importer.md).

[Overview](overview.md)

## Context and identity

This document defines shared context and the Observation family. The
[RawBits family](raw-bits.md) is also part of the core model, specified separately
for readability. Either family may be present alone. In a RawBits-only dataset,
Setup still names one logical station; it does not assert
that code, carrier-phase, Doppler or signal-strength observations are available.

All records follow the format-wide [RINEX interoperability and string rules](overview.md#rinex-interoperability-and-strings).
Logical fields are not constrained by RINEX output widths or character limits.

Observation Setup (Setup) describes a fixed receiver, antenna installation,
firmware and scientifically relevant measurement configuration. Changing that
combination starts a new Setup. Era A/B/C remain project dataset-period names,
not Core identities. V0 does not model a general equipment-configuration history.

| Record | Required fields | Optional context |
| --- | --- | --- |
| Setup | `setup_id: string`, receiver/antenna identities, firmware/configuration identity | Marker, position, antenna offsets and installation metadata |

IDs are scoped to the declared dataset/session and must remain resolvable in
replay. No UUID service or artifact hash chain is required. Coordinates and
offsets declare units/frame and whether approximate or independently supplied.
Do not claim surveyed coordinates from a receiver position estimate.

`setup_id` is a nonempty free-form string, not a path component or an implicit
RINEX marker name. Preserve separate `marker.name`, `marker.number` and
`marker.type` in Setup metadata. User-selected storage paths are independent
of the identity string; never construct paths from its untrusted contents.

A Setup represents one logical station with one receiver and one antenna,
independently of output interfaces, message selections, ordering or recording
paths. Different receiver/antenna inputs use separate logical stations and
Setup directories. There is no separate Stream ID, registry or antenna-name
reference. A station does not promise continuous or gapless sampling.

Two u-blox UART outputs, or Septentrio Disk Logger and UART/IP outputs, may be
Recording Sources for the same station. Message sets, output rates, order,
latency and missing records can differ. Equal field values are not a condition
of station identity; corresponding-record conflicts are reconciled explicitly.
Source-to-station association is declared, not inferred solely from matching
coordinates, receiver models, timestamps or payloads.

Reconnects, logger handovers, reboots with unchanged settings and midnight do
not by themselves create a Setup. Receiver measurement-rate changes
are measurement configuration changes; changing only an interface's message
output rate is not. Restart/continuity evidence is represented separately.
Recording-source attribution and overlap decisions belong to
[import policy](import-policy.md), not Setup version management.

## Setup metadata

Setup identities are defined above. The `setup.json`
serialization, field groups, single antenna, examples and validation
rules are specified separately in [Setup JSON](setup-json.md). ParquetNEX imports
this metadata at directory initialization, not with each daily recording.

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
Measurement-clock evidence belongs to [Auxiliary](auxiliary.md). Consumers load required event
context, possibly from preceding days, without per-row event references.
Observation quality remains signal-local. Events are not epoch lookup tables.

Skip untimeable Observation and report counts; preserve raw archives.
RawBits with unknown time is retained with null GPST under the
[receiver time association policy](telemetry-time.md).
RawBits-only sources need no observations.
Legitimate untimed RINEX special events retain separate mapping semantics.
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
| `code_quality`, `phase_quality`, `doppler_quality`, `cn0_quality` | ObservableQuality? | Independent per-observable quality |
| `phase_tracking` | PhaseTracking? | Phase-specific indicators and reported lock duration |
| `receiver_corrections` | ReceiverCorrections? | Reported per-observable preprocessing corrections, not applied or undone by import |
| `doppler_variance_factor` | float32? | Finite nonnegative factor in Hz2/cycles2 used to derive Doppler variance from phase variance |

### ObservableQuality

| Field | Type | Meaning |
| --- | --- | --- |
| `status` | enum | `valid`, `invalid`, or `unknown`: source-reported validity, not downstream scientific acceptance |
| `stddev` | float32? | Finite nonnegative standard deviation in the corresponding observable's unit |
| `stddev_is_lower_bound` | bool? | Whether the uncertainty represents a lower bound rather than an uncensored estimate |
| `rinex_ssi` | uint8? | Original RINEX strength indicator, 1-9; zero/blank becomes null |

Store only standard deviation, not a second variance column. Decode a source
variance's units and no-data rules before taking its square root; do not infer
uncertainty from C/N0. This normalization does not promise bitwise reversibility
of a floating-point square root. Missing quality is not evidence of validity.
`stddev_is_lower_bound` belongs to each observable's quality independently,
not the entire row. True identifies a known lower-bound uncertainty, including
one derived from a clipped variance. False requires an established uncensored
source mapping; null means unknown or unavailable. A null stddev requires a
null bound flag. This describes uncertainty, not clipping of the observable,
RF signal or ADC. Keep stddev as float32: converted bounds have the same
rounding limits as other uncertainties, not exact interval-arithmetic semantics.
There is no generic `saturated` flag: lock saturation has its own representation,
and RF/ADC clipping belongs to receiver diagnostics, not inferred from C/N0.

### PhaseTracking and LockDuration

| Field | Type | Meaning |
| --- | --- | --- |
| `loss_of_lock` | bool? | Explicit source indication of loss of lock; unknown is not false |
| `half_cycle_ambiguity` | bool? | Source indicates unresolved half-cycle ambiguity |
| `half_cycle_subtracted` | bool? | Source explicitly reports a half-cycle already subtracted from the exported phase |
| `rinex_lli` | uint8? | Original RINEX phase LLI bitmask, 0-7; blank becomes null |
| `lock` | LockDuration? | Source-reported duration or bounds, not an importer-generated counter |
| `continuity_counter` | uint32? | Source-reported signal continuity-change counter, not an inferred loss-of-lock boolean |
| `continuity_counter_modulus` | uint32? | Modulus of that counter, greater than one; null when counter is absent |

| LockDuration field | Type | Meaning |
| --- | --- | --- |
| `lower_s` | Duration | Reported duration or lower bound in seconds |
| `upper_s` | Duration? | Exclusive upper bound, used only for `interval` |
| `representation` | enum | `reported_value`, `interval`, or `lower_bound` |

An interval is `[lower_s, upper_s)` with upper greater than lower. The other
representations require null upper bounds. A reported value retains source
quantization; it does not claim perfect duration accuracy. Saturated counters
use lower bounds. No lock information means a null lock record.

Continuity counter and modulus are supplied together, with counter less than
modulus. SBF MeasExtra reports modulo 256; acquisition and cycle slips can both
increment it. Preserve the reported value without unwrapping or deriving a
receiver restart from a decrease. An unchanged value is not proof of continuity
across a gap, and a change does not uniquely identify a PLL loss of lock.

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
[adapter mapping](receiver-profiles.md#observation-quality-mapping).
Preserve RINEX LLI while mapping its defined semantics to common phase flags;
zero/blank does not establish scientifically verified continuity. Do not infer
slips from phase jumps or invent half-cycle subtraction from lock resets.

Absent fields and source no-data sentinels become null. Zero is a numerical
value, not a universal sentinel. Finite in-domain but unreliable measurements
remain numerical values with quality flags. Do not create a general missing-
reason taxonomy; retain explicit diagnostics only where interpretation needs it.
Missing C, L, D or S does not invalidate the remaining columns. Consumer
requirements such as dual-frequency TEC do not constrain Core compliance.

Only dB-Hz signal-strength observations are supported. Explicit non-DBHZ RINEX
S-observable units produce an unsupported-unit error, not a guessed conversion
or silent omission. Missing unit headers follow the applicable supported RINEX
version's rules; absence alone is not a declaration of another unit. SSI alone
cannot populate `cn0_db_hz`, and importers do not synthesize SSI from C/N0.
Derive full C/L/D/S codes from system and signal only when justified; do not
force unlike signal attributes into one row.

## Time representation

### Shared semantic types

| Type | Representation | Meaning and constraints |
| --- | --- | --- |
| `GpstTimestamp` | DECIMAL(38,12) | Nonnegative seconds since the GPST origin |
| `TimeDelta` | DECIMAL(38,12) | Signed seconds: differences, offsets, biases and timing errors |
| `Duration` | DECIMAL(38,12) | Nonnegative seconds: elapsed time, bounds, ages and periods |

All normalized time quantities use these types. Periods additionally require
strictly positive values. Field nullability is declared separately. A duration
used for accuracy retains the source's accuracy definition; it is not implicitly
a standard deviation. Exact arithmetic applies to represented values, not
measurement accuracy or arbitrary source floating-point bits. Apply the same
picosecond quantization and range checks to all three types.

These are logical semantic types, not a required C++ ABI. Native counters,
standard week/TOW encodings and model coefficients retain their own definitions;
normalizing a counter into duration does not unwrap or extrapolate it. Frequency
offset/drift is not a duration. Observables and non-time physical parameters
retain their individually declared numerical types.

All canonical timestamps use `DECIMAL(38,12)` seconds since
1980-01-06 00:00:00 GPST: a value of one always means one second. This exact
fixed-point type has 38 total decimal digits and 12 fractional digits, giving
1 ps resolution and a maximum magnitude of `10^26 - 10^-12` seconds.
It is not a Unix timestamp. Absolute GPST values must be nonnegative and within
the decimal range. Use names such as `gpst`, `start_gpst` and `end_gpst`, not
`gpst_ns` or `gpst_ps`; there is no separate fractional-remainder column.
Required epoch timestamps cannot be unknown. Other families may permit null
time under their own semantics; zero denotes the origin, not unknown time.
The type specifies representation resolution, not receiver measurement accuracy.

Convert source time scales at the input boundary. Preserve necessary native
navigation time fields with their standards-defined meanings. GPST calendar
partitions and labels do not use UTC suffixes or timezone conversion.

Normalize source epoch values to the nearest picosecond, using round half to
even and carrying into the next second when necessary. No timestamp is snapped to a nominal sampling interval or
whole second. Parse decimal source timestamps without first constructing a
large floating-point absolute-seconds value. For week/TOW inputs, retain the
integer week contribution separately during conversion and handle week carry.
Summarize actual subpicosecond rounding without per-record warning spam.

This is a deliberate precision boundary: a normalized timestamp does not
promise exact preservation of every source timestamp bit. Raw archives retain
the original representation. This rule does not quantize pseudorange, carrier
phase, or other non-time scientific values to picosecond ticks. Clock offsets
use the `TimeDelta` picosecond resolution. Decimal
source epochs with at most 12 fractional digits are representable exactly
after an exact time-scale conversion. Finer inputs require the stated rounding;
fixed scale 12 does not promise arbitrary future precision.

GPST representation does not imply removal of receiver clock error. Inputs with
applied observation clock-offset correction are unsupported. Observation time, receiver message time, and navigation epoch
context are distinct and must not be substituted for one another.

Exact timestamp differences use `TimeDelta` with checked arithmetic.
Normalized durations and clock offsets use `Duration` and `TimeDelta`.
Native standard week/TOW and
navigation-model time parameters are not replaced by this canonical timestamp.
Arrow maps the type to `decimal128(38,12)`; neither Arrow nor a particular
integer implementation is a CommonNEX conformance requirement.

## Logical types and naming

The proposed schema uses `uint8`, `uint16`, `uint32`, `uint64`, `int32`, `int64`,
`float32`, `float64`, `DECIMAL(38,12)`, `bool`, `string`, `bytes`, enums, and records/lists of
these types. Every field declaration includes `name`, `data_type`,
`nullable: bool`, and `semantics`, together with applicable units and constraints.
`semantics` identifies a CommonNEX-defined scientific quantity or role; its
definition reuses the relevant GNSS standard rather than introducing competing
physical meanings. Constraints restrict representable values beyond the type.
Nullability is a schema property, not another value repeated in each record.
In this specification, `T?` abbreviates `data_type: T, nullable: true`; a type
without `?` has `nullable: false`. Enum wire encodings remain unspecified.

For example, this is an illustrative schema declaration, not a wire format:

```yaml
name: receiver_temperature_c
data_type: float32
nullable: true
semantics: receiver_temperature
unit: degC
scale:
  numerator: 1
  denominator: 1
constraints:
  minimum: -273.15
```

The minimum in this example is expressed in degrees Celsius.

### Integer-first numerical representation

Prefer integers for counters and scaled integers for physical quantities.
The schema fixes the unit and an exact rational `scale` for each field:
`physical_value = stored_integer * scale` in the declared unit. Scale is part
of the canonical field definition, not chosen independently per record,
receiver, file, or transport. An omitted scale means one. Do not encode a
decimal scale through an approximate binary floating-point metadata value.
No additive offset is proposed; signed physical quantities use signed integers.

Choose a scale and width from the source resolution, reconstruction arithmetic,
required dynamic range, and an explicit maximum quantization error. Integers
alone do not guarantee exact preservation of a source floating-point value.
If rounding is required, document and validate its effect; a source's nominal
accuracy does not by itself authorize discarding its reported resolution.
Unresolved fidelity requirements remain review items rather than claims of
lossless normalization. Do not preserve IEEE floating-point bits in an integer
field and call that a normalized physical quantity.

Logical range constraints apply to decoded physical values; the integer type's
range also bounds the encoded value. Nullable integer fields use logical null,
never a reserved integer sentinel. Calculations may use floating point without
requiring the persisted or transported logical quantity to be floating point.
Likewise, a transport must not convert large integers through an inexact float.

Raw observables are an agreed exception to integer-first storage: retain the
observation `float64` values and uncertainty `float32` fields. Direct
source binary64 observations and phase/Doppler reconstruction involving
frequency ratios justify avoiding additional fixed-point quantization here.
No conversion of these fields to scaled integers is planned for v0. A RINEX-specific
auxiliary epoch-local clock-offset estimate uses `TimeDelta`. Continuous non-time physical parameters, including
decoded navigation parameters, may use `float64` with defined units; do not
force them onto a broadcast fixed-point grid solely for integer-first storage.
Telemetry integer types and scales
in [Auxiliary](auxiliary.md) are agreed design choices; implementation must still validate source
conversion, rounding, and overflow rather than claim measured fidelity already.

Logical null means no usable numerical value is available, including absent
source fields and source-defined no-data sentinels. It is not zero, an empty
string, or a numerical infinity. A non-null value can still have invalid or
uncertain scientific quality: preserve the number and its separate quality
state when the source provides both. Preserve finite saturation limits and
their saturation flags rather than replacing them with infinity.

In-memory or transport implementations may represent logical null using a
validity mask, an optional value, or NaN for nullable floating-point fields.
Such mappings must be unambiguous and must reconstruct logical null on read.
Do not encode different missing reasons in NaN payloads or use positive/negative
infinity as status codes. Scientifically necessary reasons, tracking states,
and quality flags remain explicit fields. A required non-null field cannot be
satisfied by a no-data sentinel. Nullability of a list and of its elements is
declared separately; an empty list is not null.

### Semantic constraints

Schema definitions may specify inclusive `minimum`/`maximum`, exclusive bounds,
`finite`, allowed enum values, and fixed lengths where appropriate. These are
logical requirements, independent of transport. Null is checked against
`nullable` first; numerical bounds apply only to non-null values. A semantic
definition supplies its unit and required constraints, so a field cannot claim
that meaning while weakening its required range. The exact machine-readable
schema syntax remains a proposal; no generic expression engine is required.

Initial semantic rules:

| Quantity or role | Required non-null domain |
| --- | --- |
| Absolute pseudorange (`C` observable) | Finite meters, greater than or equal to zero |
| Carrier phase (`L` observable) | Finite cycles; signed values permitted |
| Doppler (`D` observable) | Finite hertz; signed values permitted |
| Standard deviation | Finite, greater than or equal to zero, in the observable's unit |
| Variance | Finite, greater than or equal to zero, in the squared observable unit |
| Lock duration | Finite seconds, greater than or equal to zero; saturation remains separate |
| Clock offset, residual, or additive correction | Finite signed value in its declared unit |
| C/N0 in dB-Hz | Finite; no generic nonnegative constraint on a logarithmic quantity |
| GPST timestamp | Nonnegative `DECIMAL(38,12)` seconds with the defined GPST origin |

The pseudorange bound is the accepted domain of the CommonNEX absolute
observation, not a rule for pseudorange differences, residuals, corrections,
or intermediate receiver encodings. Those are distinct signed quantities.
No universal maximum range or receiver-specific quality threshold is imposed
on pseudorange. Zero satisfies the common lower bound, but an input protocol's
zero/no-data sentinel must still become null at its decoding boundary.

Each observation column has its own quantity, unit, and domain. A nullable
column does not weaken those constraints. Unmapped quantities require a typed
semantic definition; never guess their meaning from magnitude or sign.

Cross-field interpretation also matters: code and unit must agree, uncertainty
must refer to the correct observable, and canonical payload lengths must match
their family definition. Record these rules directly in the relevant schemas.
Scientific plausibility filters, such as elevation masks or a receiver's
preferred C/N0 threshold, belong to processing policy, not mandatory value ranges.

Permissive import does not permit out-of-domain values in compliant fields.
For a recoverable nullable value violation, emit null with an explicit invalid
quality/diagnostic reason and retain the offending source value in importer
diagnostics or the preserved source record. Keep unaffected observations and
continue importing. Never clamp to a bound, take an absolute value, or silently
mark the result valid. A missing required identity or unresolvable structural
violation excludes the affected record with a diagnostic, not necessarily the
whole file. Finite in-domain values marked unreliable by the receiver remain
stored with their quality flags; numeric validity is not scientific acceptance.

### GNSS identifiers

Use the in-scope RINEX system identifiers `G`, `E`, `C`, `J`, and `S`.
Represent a satellite as `satellite_system: string` and
`satellite_number: uint16`, following the RINEX numbering rules for that system.
The pair can be rendered as a RINEX satellite identifier, such as `G01`.
Do not universally label the numeric component `prn`, or impose a two-digit
storage limit because of a textual example.

An observable is identified by its full RINEX observation code, such as
`C1C`, `L1C`, `D1C`, or `S1C`, together with the satellite system. A band and
attribute such as `1C` may group related observables within a system; it is not
a globally unique signal identifier. Do not invent a competing canonical
signal-name vocabulary.

Importers map native satellite/signal identifiers to the common identity.
Do not repeat UBX/SBF/RTCM identity structs on normal Observation rows.
Unresolved or unsupported signal mappings exclude the affected observation
with a diagnostic; never guess the nearest-looking code. Raw archives preserve
native identifiers. RINEX 2 and its ambiguous legacy codes are out of scope.


## Normalization and processing boundary

Apply source unit/time conventions and decode RINEX scale factors into physical
values; downstream consumers never reapply ASCII storage scaling. Preserve
phase convention and already-applied correction metadata with explicit scope.
Do not apply or undo receiver clock corrections during import. Accept only
inputs without applied observation clock-offset correction and retain the
source's consistent epoch/code/phase values. GPST time-scale
normalization is not receiver clock-error removal. NAV-CLOCK and SBF PVT clock
bias/drift remain telemetry; never use them to fill or correct raw observations.

There is no general clock-correction application-state mechanism. RINEX input
with `RCV CLOCK OFFS APPL=1` is an unsupported-input error; zero is accepted,
and an absent header follows that supported version's specified default.
Optional RINEX epoch offset estimates may be retained as
`rinex_receiver_clock_offset_s: TimeDelta?` in a source-specific auxiliary
record, never applied, interpolated or held forward. That adapter is not yet
implemented. Internal receiver clock steering/integer-millisecond adjustments
are not this RINEX correction workflow: retain their reported measurement-clock
evidence independently, without inferring reboot, loss of lock or adjustment size.
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

See [current GPST policy](../time-policy.md) for implemented products; this
draft's decimal representation does not retroactively reinterpret them.

## Reference

- [RINEX 4.02 specification](https://files.igs.org/pub/data/format/rinex_4.02.pdf):
  identifier and semantic reference, not a claim of complete adapter support.
