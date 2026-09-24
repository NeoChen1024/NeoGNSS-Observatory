# CommonNEX shared types and identity

[Overview](overview.md)

## Context and identity

These definitions apply to [Observation](observations.md), [RawBits](raw-bits.md),
[Events](events.md) and [receiver telemetry](receiver-telemetry.md).
Observation and RawBits are independent core record families. Either family may be present alone. In a RawBits-only dataset,
Setup still names one logical station; it does not assert
that code, carrier-phase, Doppler or signal-strength observations are available.

All records follow the format-wide [naming and string rules](overview.md#naming-and-strings).
Logical fields are not constrained by RINEX output widths or character limits.

Observation Setup (Setup) describes a fixed receiver, antenna installation,
firmware and scientifically relevant measurement configuration. Changing that
combination starts a new Setup. Era A/B/C remain project dataset-period names,
not Core identities. V0 does not model a general equipment-configuration history.

| Record | Required fields | Optional context |
| --- | --- | --- |
| Setup | `setup_id: string`, required configuration structure defined in Setup JSON | Known receiver/antenna identities, marker, position and installation metadata |

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

Convert source time scales at the input boundary. GPST calendar
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
Arrow maps the type to `decimal128(38,12)`; neither Arrow nor a particular
integer implementation is a CommonNEX conformance requirement.

## Logical types and naming

The logical schema uses `uint8`, `uint16`, `uint32`, `uint64`, `int32`, `int64`,
`float32`, `float64`, `DECIMAL(38,12)`, `bool`, `string`, `bytes`, enums, and records/lists of
these types. Every field declaration includes `name`, `data_type`,
`nullable: bool`, and `semantics`, together with applicable units and constraints.
`semantics` identifies a CommonNEX-defined scientific quantity or role; its
definition reuses the relevant GNSS standard rather than introducing competing
physical meanings. Constraints restrict representable values beyond the type.
Nullability is a schema property, not another value repeated in each record.
In this specification, `T?` abbreviates `data_type: T, nullable: true`; a type
without `?` has `nullable: false`. The current Arrow/Parquet mapping stores enums as strings; see [ParquetNEX](parquetnex.md).

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
No conversion of these fields to scaled integers is planned for v0. Continuous non-time
physical parameters may use `float64` with defined units; do not
force them onto a broadcast fixed-point grid solely for integer-first storage.
Telemetry integer types and scales
in [receiver telemetry](receiver-telemetry.md) are field-specific choices, not a
requirement to quantize all physical quantities into integers. Source conversion,
rounding and overflow must respect each declared type.

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
Do not repeat source-protocol identity structs on normal Observation rows.
Unresolved or unsupported signal mappings exclude the affected observation
with a diagnostic; never guess the nearest-looking code. Raw archives preserve
native identifiers.
