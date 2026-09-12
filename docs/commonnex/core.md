# CommonNEX Core

Status: v0 design draft; not an implemented format or API.

[Overview](overview.md)

## Context and identity

Observation Setup (Setup) describes a fixed receiver, antenna installation,
firmware and scientifically relevant measurement configuration. Changing that
combination starts a new Setup. Era A/B/C remain project dataset-period names,
not Core identities. V0 does not model a general equipment-configuration history.

| Record | Required fields | Optional context |
| --- | --- | --- |
| Setup | `setup_id: string`, receiver/antenna identities, firmware/configuration identity | Marker, position, antenna offsets and installation metadata |
| Observation Stream (Stream) | `stream_id: string`, `setup_id: string`, `antenna_id: string` | Logical receiver observation input identity |

IDs are scoped to the declared dataset/session and must remain resolvable in
replay. No UUID service or artifact hash chain is required. Coordinates and
offsets declare units/frame and whether approximate or independently supplied.
Do not claim surveyed coordinates from a receiver position estimate.

A Stream represents observations and associated records derived from one
logical receiver observation source, independently of output interfaces,
message selections, ordering, or recording paths. Independent antenna inputs
use distinct streams. A Stream belongs to one Setup; a new Setup creates new
streams. It does not promise continuous or gapless sampling.

Two u-blox UART outputs, or Septentrio Disk Logger and UART/IP outputs, may be
Recording Sources for the same Stream. Message sets, output rates, order,
latency and missing records can differ. Equal field values are not a condition
of Stream identity; corresponding-record conflicts are reconciled explicitly.
Source-to-Stream association is declared, not inferred solely from matching
coordinates, receiver models, timestamps or payloads.

Reconnects, logger handovers, reboots with unchanged settings and midnight do
not by themselves create a Setup or Stream. Receiver measurement-rate changes
are measurement configuration changes; changing only an interface's message
output rate is not. Restart/continuity evidence is represented separately.
Recording-source attribution and overlap decisions belong to
[import policy](import-policy.md), not Setup version management.

## Setup metadata

The ParquetNEX directory initializer imports `setup.json` and its referenced
vendor configuration file once. Daily UBX/SBF/RINEX inputs do not carry or
recopy these files. See [storage initialization](parquetnex.md#storage-initialization).
JSON describes the station setup, not every piece of dataset metadata.

The selected metadata groups are below; concrete nested keys and the complete
RINEX header mapping remain implementation review items.

| Group | Contents |
| --- | --- |
| Marker | RINEX-like marker name, number and type; coordinates with frame, units and position basis |
| Receiver | Manufacturer/model, serial number and firmware |
| Antennas | Antenna type/model, radome, serial number and receiver input association |
| Installation | Marker-to-ARP offset with explicit direction, coordinate representation and units |
| Tracking | Declared constellations and signals per constellation; receiver measurement rate where known |
| Feed lines | Optional cable type, length with units, and antenna/receiver input association |
| `vendor_config` | Optional filename of a configuration file beside `setup.json` |

Do not infer cable delay from type or length or add an independent cable-delay
field. Receiver compensation settings remain in the vendor configuration.
The specification does not prescribe that file's format or require the importer
to decode it. It should allow the original receiver settings to be recovered,
or be directly applicable to a compatible receiver. Keep its contents out of
`setup.json`. No automatic configuration tracking, dump comparison or update
service is required; differences in transport/logging settings do not define
new Setups merely because the dump differs.

Unknown metadata remains absent/null, not an invented value. Declared signal
configuration is distinct from actual signal coverage. Consumers inspect it
before reading observations: for example, an L1/L2-only algorithm can reject a
known GPS L1/L5-only source immediately. Unknown configuration is not proof of
incompatibility, and enabled signals do not guarantee measurements at every
epoch. Exact constellation/signal identifiers and configuration completeness
must be defined so omission is not silently interpreted as disabled.

RINEX source headers, comments, conversion details and observation events do
not all belong in Setup. Map them to source metadata or the appropriate record
family. RINEX-like observation coverage plus extra receiver setup information
is the intended superset; lossless RINEX import still requires explicit field,
correction and event mappings rather than merely retaining a JSON container.

## ObservationEpoch and NavigationEpoch

These are independent logical records; there is no required one-to-one mapping.

| ObservationEpoch field | Type | Meaning |
| --- | --- | --- |
| `stream_id` | string | Parent stream |
| `epoch_id` | uint64 | Identity within the stream; not the timestamp |
| `gpst_ns` | uint64 | Measurement epoch on the GPST axis |
| `receiver_clock_offset_s` | float64? | Source-reported clock offset; canonical sign mapping must be finalized |
| `clock_correction_state` | record | Separate applied/not_applied/unknown states for epoch time, code and phase |
| `completion` | enum | complete, incomplete or unknown |
| `completion_basis` | enum | protocol_boundary, record_structure, incomplete_tail or unknown |
| `rinex_epoch_flag` | uint8? | Original flag when supplied |

| NavigationEpoch field | Type | Meaning |
| --- | --- | --- |
| `stream_id` | string | Parent stream |
| `nav_epoch_id` | uint64 | Navigation-context identity within the stream |
| `gpst_ns` | uint64 | Associated navigation epoch, not precise transmission time |
| `completion` | enum | complete, incomplete or unknown |
| `completion_basis` | enum | protocol_boundary, record_structure, incomplete_tail or unknown |

RawNav may reference NavigationEpoch without any ObservationEpoch.
An adapter must not overwrite a measurement timestamp with navigation context.
Unknown time does not become a fabricated timed epoch: retain unassociated
extension records where allowed, otherwise report the unusable observation.
Repeated timestamps can have distinct epoch IDs. Nanosecond rounding does not
prove occurrence equality.

Completion means the producer closed the relevant epoch under its adapter
contract, not that every expected signal was received. Navigation completion
does not close unrelated measurements or future pulse events.
A source-local boundary is not automatically merged-stream completion.

## Observation: one row per epoch/satellite/signal occurrence

The logical record and primary Parquet layout are wide, not scalar observable
rows. A normal row groups C/L/D/S of one system-specific signal. If genuinely
distinct or conflicting occurrences share those keys, their observation IDs
remain distinct and reconciliation must expose the relationship.

| Field | Type | Meaning |
| --- | --- | --- |
| `stream_id`, `epoch_id` | string, uint64 | Parent ObservationEpoch |
| `observation_id` | uint64 | Row identity within the epoch |
| `satellite_system`, `satellite_number` | string, uint16 | RINEX satellite identity |
| `signal` | string? | System-specific RINEX band/attribute, such as 1C |
| `pseudorange_m` | float64? | C observable |
| `carrier_phase_cycles` | float64? | L observable |
| `doppler_hz` | float64? | D observable |
| `cn0_db_hz` | float64? | S observable only when its unit is known to be dB-Hz |
| `code_quality`, `phase_quality`, `doppler_quality`, `cn0_quality` | record? | Independent per-observable quality |
| `lock_time_ms` | uint64? | Reported duration; saturation and source meaning retained |
| `source_identity` | typed record? | Necessary namespaced signal/message identity |
| `source_quality` | typed record? | Additional scientifically relevant source flags |

Each quality record may carry status (valid/invalid/unknown), standard deviation
and variance (independently nullable float32, in that observable's unit and
squared unit), and applicable saturation/lock/half-cycle information.
Preserve LLI/SSI with their corresponding observable rather than assigning one
unqualified flag to all four columns. Final nested field names remain open.

Absent fields and source no-data sentinels become null. Zero is a numerical
value, not a universal sentinel. Finite in-domain but unreliable measurements
remain numerical values with quality flags. Do not create a general missing-
reason taxonomy; retain explicit diagnostics only where interpretation needs it.
Missing C, L, D or S does not invalidate the remaining columns. Consumer
requirements such as dual-frequency TEC do not constrain Core compliance.

An SSI digit or unknown-unit signal strength cannot populate cn0_db_hz.
Preserve it in a typed source field with its known unit/scale, or explicitly
report an unsupported mapping. Derive full C/L/D/S codes from system and signal
only when justified; do not force unlike signal attributes into one row.

## Time representation

`gpst_ns` is an unsigned 64-bit integer counting nanoseconds since
1980-01-06 00:00:00 GPST. It is not a Unix timestamp. The representable positive
duration is approximately 584 years. Negative and out-of-range absolute times
are rejected before unsigned conversion. Unknown time uses null or explicit
validity, never a sentinel such as zero.

Convert source time scales at the input boundary. Preserve necessary native
navigation time fields with their standards-defined meanings. GPST calendar
partitions and labels do not use UTC suffixes or timezone conversion.

Normalize source epoch values to the nearest nanosecond. Proposed tie rule:
round half to even. No timestamp is snapped to a nominal sampling interval or
whole second. Parse decimal source timestamps without first constructing a
large floating-point absolute-seconds value. For week/TOW inputs, retain the
integer week contribution separately during conversion and handle week carry.
Summarize actual subnanosecond rounding without per-record warning spam.

This is a deliberate precision boundary: a normalized timestamp does not
promise exact preservation of every source timestamp bit. Raw archives retain
the original representation. This rule does not quantize pseudorange, carrier
phase, clock bias, or other scientific values to integer nanoseconds.

GPST representation does not imply removal of receiver clock error. Preserve
whether a clock correction has already been applied to epoch tags and
observables. Observation time, receiver message time, and navigation epoch
context are distinct and must not be substituted for one another.

Time differences and clock corrections may be negative. Use an appropriate
signed or floating-point representation and checked arithmetic; unsigned
subtraction is not a general time-difference operation.

## Logical types and naming

The proposed schema uses `uint8`, `uint16`, `uint32`, `uint64`, `int32`, `int64`,
`float32`, `float64`, `bool`, `string`, `bytes`, enums, and records/lists of
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
name: receiver_temperature_mdeg_c
data_type: int32
nullable: true
semantics: receiver_temperature
unit: degC
scale:
  numerator: 1
  denominator: 1000
constraints:
  minimum: -273.15
```

The minimum in this example is expressed in physical degrees Celsius, not
integer milli-degrees.

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
No conversion of these fields to scaled integers is planned for v0. The epoch
clock-offset field retains `float64`. Other new floating-point
fields require an individual justification. Telemetry integer types and scales
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
| GPST timestamp | The defined `uint64` range and GPST epoch/unit |

The pseudorange bound is the accepted domain of the CommonNEX absolute
observation, not a rule for pseudorange differences, residuals, corrections,
or intermediate receiver encodings. Those are distinct signed quantities.
No universal maximum range or receiver-specific quality threshold is imposed
on pseudorange. Zero satisfies the common lower bound, but an input protocol's
zero/no-data sentinel must still become null at its decoding boundary.

Each observation column has its own quantity, unit, and domain. A nullable
column does not weaken those constraints. Unmapped quantities require a typed
semantic definition; never guess their meaning from magnitude or sign.

Cross-field interpretation also matters: code and unit must agree, variance
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

Use RINEX system identifiers such as `G`, `E`, `C`, `J`, `I`, and `S`.
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

Retain necessary source observation codes and UBX/SBF signal identifiers when
mapping them to common identities. If there is no justified RINEX mapping,
leave the common code null and retain a typed, namespaced source identity.
Do not assign the nearest-looking RINEX code. Whether a processor accepts such
records is an explicit capability decision.


## Normalization and processing boundary

Apply source unit/time conventions and account for RINEX scale factors, phase
shifts and already-applied clock corrections without applying them twice.
Exact correction-sign and adapter mappings remain review items.
Import does not smooth, interpolate, repair slips, unwrap clocks or estimate
missing observations. Inferred arcs and scientific corrections are outputs of
processing facilities, not mutations of imported records.

Shared continuity events identify stream, optional affected epoch, event kind
and reported/inferred evidence. They must distinguish actual restart/loss from
transport completion; batch boundaries have no scientific meaning.
Detailed event encodings remain a review item.

See [current GPST policy](../time-policy.md) for implemented products; this
draft's unsigned representation does not retroactively reinterpret them.

## Reference

- [RINEX 4.02 specification](https://files.igs.org/pub/data/format/rinex_4.02.pdf):
  identifier and semantic reference, not a claim of complete adapter support.
