# CommonNEX and ParquetNEX v0 draft

Status: design proposal for review; not an implemented format or API.

This draft records the agreed architectural direction and proposes an initial
logical schema. Sections explicitly marked as proposals or open questions need
review before implementation. It does not change existing product schemas,
logger behavior, reconstruction policy, or supported processing paths.

## 1. Purpose and boundaries

CommonNEX is a logical GNSS data format for offline and live processing.
It defines fields, data types, units, validity, identities, and relationships.
ParquetNEX is its on-disk serialization using Apache Parquet.

The intended flow is:

```text
UBX / SBF / RINEX -> input adapters -> CommonNEX -> scientific processors
                                         |
                                         +-> ParquetNEX writer
                                         ^
                                         +-- ParquetNEX reader
```

CommonNEX does not define a wire format, memory layout, language ABI, transport,
or mandatory batch container. Arrow, Python objects or Pickle, Protobuf, and
native records are implementation choices. Implementations must preserve the
logical types and semantics across their chosen representation.

CommonNEX can be passed directly from an input adapter to a live processor.
Writing ParquetNEX is optional. A ParquetNEX reader exposes the same logical
records, allowing offline replay without requiring algorithms to parse the
original input format.

The format does not specify backpressure, acknowledgments, retransmission,
exactly-once delivery, network discovery, or processor checkpoint encoding.
These belong to implementations and processing contracts.

## 2. Agreed scope

- Accept UBX, SBF, and RINEX through format-specific adapters. RINEX need not
  be an intermediate when importing raw receiver observations.
- Include received raw navigation bits from every in-scope constellation as a
  core record family: subframes, pages, messages, and receiver-delivered
  fragments where a canonical fragment representation is defined. Adapters
  must normalize receiver packing to a canonical navigation-family layout;
  decoding every message type's scientific parameters is not required.
  SBAS is one specialization, not the boundary of this family. ParquetNEX
  preserves canonical records without a RINEX intermediate or receiver-specific
  payload fallback.
- Reuse RINEX satellite-system identifiers, satellite identification rules,
  observation codes, and scientific meanings wherever applicable.
- Do not inherit fixed-width text records, continuation lines, whitespace
  padding, or text-driven numerical precision limits.
- Exclude GLONASS from v0 observations, navigation, and scientific processing.
  Mixed inputs must still be parsed correctly across GLONASS records. Report
  skipped observation and navigation counts; an explicit request to process
  GLONASS fails. Do not implement GLONASS-specific biases or frequency-channel
  handling. The identifier `R` retains its RINEX meaning and is not reassigned.
- Use `uint64 gpst_ns` for absolute project observation timestamps. No
  subnanosecond remainder field is required.
- Support bounded batches, incremental imports, and live processing without
  treating physical file, batch, or GPST-day boundaries as scientific resets.
- Accept overlapping and repeated logging inputs by default. Reconcile copies
  of the same acquisition, retain complementary data, and expose conflicts;
  overlap alone is not an import error. The objective is maximum recoverable
  coverage without fabricated observations or silent duplicate counting.
- Include receiver clock, navigation-time, pulse-timing, temperature, and other
  supported analysis telemetry in the same import pass as observations and
  navigation bits. Their extraction must not depend on running a particular
  downstream analysis first.
- Preserve raw archives. CommonNEX is not a byte-for-byte replacement for
  UBX/SBF, and RINEX input cannot restore information lost before import.

Recognizing a system or observation code is not a claim of complete adapter,
navigation-message, firmware-revision, or algorithm support. Each implementation
must describe its supported subset and report exclusions rather than silently
discarding unsupported data.

## 3. Time representation

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

## 4. Logical type and naming conventions

The proposed schema uses `uint8`, `uint16`, `uint32`, `uint64`, `int32`, `int64`,
`float32`, `float64`, `bool`, `string`, `bytes`, enums, and records/lists of
these types. Every field declaration includes `name`, `data_type`,
`nullable: bool`, and `semantics`, together with applicable units and constraints.
`semantics` identifies a CommonNEX-defined scientific quantity or role; its
definition reuses the relevant GNSS standard rather than introducing competing
physical meanings. Constraints restrict representable values beyond the type.
Nullability is a schema property, not another value repeated in each record.
In the tables below, `T?` abbreviates `data_type: T, nullable: true`; a type
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
observation `float64` values and uncertainty `float32` fields below. Direct
source binary64 observations and phase/Doppler reconstruction involving
frequency ratios justify avoiding additional fixed-point quantization here.
No conversion of these fields to scaled integers is planned for v0. The epoch
clock-offset field also retains its current type. Other new floating-point
fields require an individual justification. Telemetry integer types and scales
below are agreed design choices; implementation must still validate source
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

The scalar Observation `value` field selects its semantic definition using
the observation code and unit. A shared numerical storage slot does not give
all observables the same numerical domain. Unmapped observables need an explicit
typed semantic definition before applying constraints; do not guess one from
the magnitude or sign of a number.

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

## 5. Proposed record families

The following is a reviewable starting schema, not a complete navigation or
receiver-telemetry field catalog. Logical relationships do not require SQL
tables or one network message per record.

### 5.1 Stream metadata

| Field | Type | Meaning |
| --- | --- | --- |
| `stream_id` | `string` | Identifies one logical receiver acquisition stream, independent of logger, file, batch, and daily import |
| `metadata_id` | `string` | Identifies a metadata revision within the stream |
| `effective_gpst_ns` | `uint64?` | Effective GPST time, when known |
| `receiver_id` | `string` | Receiver identity within the dataset or processing session |
| `antenna_id` | `string` | Antenna identity within the receiver |
| `logging_source_id` | `string` | Identifies a contributing recording source; several may map to one logical stream |
| `source_format` | enum | `ubx`, `sbf`, or `rinex` for this logging source, not current transport |
| `source_version` | `string?` | Relevant protocol or RINEX version for this logging source when known |

Receiver model, firmware, antenna model, marker information, positions, and
antenna offsets are optional typed metadata with units and coordinate-frame
definitions. Their detailed field catalog remains open. Preserve whether
coordinates are approximate or independently supplied; do not imply a survey.

Record references, not timestamp alone, determine which metadata applies when
effective time is unknown or time reverses. A stream ID must not change merely
because the writer starts a new daily partition. ID generation is implementation
defined; no random UUID, hash chain, or central registry is required.

Receiver/antenna metadata and logging-source metadata have distinct scopes.
The metadata family must support several logging-source entries for one stream;
it must not force the reconciled stream to have a single input format or file.
Map sources to the same stream using explicit configuration or reliable receiver,
antenna, and acquisition context. Equal timestamps, satellite IDs, station
coordinates, or file names alone do not establish shared acquisition identity.
Independent receivers remain separate even when observing the same satellites.

### 5.2 Epoch

| Field | Type | Meaning |
| --- | --- | --- |
| `stream_id` | `string` | Acquisition stream |
| `epoch_id` | `uint64` | Unique epoch identity within that stream; not a timestamp |
| `metadata_id` | `string` | Applicable stream metadata revision |
| `gpst_ns` | `uint64` | Observation epoch tag expressed on the GPST axis |
| `receiver_clock_offset_s` | `float64?` | Source-reported offset, with its source-defined sign convention mapped explicitly |
| `clock_correction_state` | record | Whether correction is applied to time, code, and phase; each may be unknown |
| `rinex_epoch_flag` | `uint8?` | Original RINEX epoch flag when supplied by RINEX |

Define the canonical clock-offset sign convention and correction-state fields
before implementation; their exact definitions are an open review item.
Do not guess their values from the presence of a clock-offset number alone.

Epoch IDs must distinguish repeated timestamps and survive file/day boundaries.
A producer can allocate increasing IDs, but IDs do not assert increasing GPST.
Unknown-time source records are not fabricated into timed epochs; adapters
report or retain them separately under an explicit unsupported/untimed policy.

### 5.3 Observation

| Field | Type | Meaning |
| --- | --- | --- |
| `stream_id`, `epoch_id` | `string`, `uint64` | Parent epoch |
| `observation_id` | `uint64` | Record identity within the epoch, including repeated observations |
| `satellite_system`, `satellite_number` | `string`, `uint16` | RINEX satellite identity |
| `observation_code` | `string?` | Full RINEX observable code; null only for explicitly unmapped source observations |
| `value` | `float64?` | Observation in the defined unit, subject to code-selected semantic constraints |
| `unit` | enum | `m`, `cycle`, `Hz`, or the declared signal-strength unit |
| `value_status` | enum | `valid`, `invalid`, or `unknown`; independent of whether a numerical value is available |
| `lli` | `uint8?` | RINEX Loss of Lock Indicator when supplied or justifiably mapped |
| `ssi` | `uint8?` | RINEX Signal Strength Indicator, distinct from an `S` observable |
| `lock_time_ms` | `uint64?` | Source-reported tracking duration in integer ms, with saturation/meaning retained |
| `standard_deviation` | `float32?` | Source uncertainty in the observable's unit, with its interpretation and saturation state retained |
| `variance` | `float32?` | Source uncertainty in the squared observable unit, with its statistical meaning and saturation state retained |
| `source_identity` | typed record? | Source message and signal/observable identifiers needed for interpretation |
| `source_quality` | typed record? | Additional source flags and quality fields |

`C`, `L`, and `D` observables use meters, cycles, and hertz respectively.
For `S` observables, preserve the declared unit; do not equate an SSI digit or
an unknown signal-strength scale with C/N0 in dB-Hz. A missing unit must remain
explicitly unknown rather than defaulting to dB-Hz.

No row for an observable means no record was supplied. It does not prove that
the receiver cannot measure it. Stream capability declarations, invalid
observations, and empty epochs express different information.

Native half-cycle flags, lock counters, and uncertainty encodings may not be
equivalent across receivers. Retain their typed source meanings when a common
mapping would lose information. Do not synthesize a generic quality score.
Tracking channel is an optional source attribute, not permanent signal identity.

Standard deviation and variance are nullable independently. Preserve whichever
statistic the source supplies; do not require both or silently replace one with
the other. Retain `float32` for both statistics; intermediate uncertainty
calculations may use `float64`. Do not require square-root conversion merely
to fit a shared numerical encoding. The scalar observation value, including
signal strength, retains the existing `float64` representation.
Nullability does not remove the need for source-quality and saturation metadata.

The logical scalar-observation view does not mandate scalar rows in Parquet
or individual objects/callbacks in memory. Implementations may batch and group
related observables while preserving all identities and quality fields.

### 5.4 Raw navigation bits: subframes, pages, messages, and fragments

CommonNEX stores receiver-delivered navigation bits for all in-scope systems
(GPS, Galileo, BeiDou, QZSS, NavIC, and SBAS), including navigation families that
the current scientific processors cannot decode. The earlier GLONASS exclusion
still applies; the record structure itself is not tied to a constellation.
A decoded ephemeris or correction record does not replace received raw bits.
Adapters normalize them directly, and ParquetNEX preserves them for later
decoding without reopening UBX/SBF input. Scientific message-decoder
availability does not gate storage; a validated receiver-packing mapping does.

Here, "raw bits" means the digital navigation content exported by a receiver,
not RF/IQ samples or recovery of bits already discarded by receiver firmware.
The record unit follows the canonical navigation family's word, fragment,
page, subframe, or message definition. Input adapters split or assemble
receiver reports as required by that definition. Do not require complete
ephemeris assembly before publishing an independently defined navigation unit,
force all signals into a GPS subframe length, or infer layout from length alone.

#### 5.4.1 Receiver-document basis

The following survey informs the proposed schema. It is not a claim of tested
firmware coverage or a complete bit-mapping specification.

UBX reference: F9 HPG 1.51 Interface description, protocol 27.50,
UBXDOC-963802114-13124 R01, section 3.17.9, page 201.
`UBX-RXM-SFRBX` (`0x02 0x13`) has an eight-byte prefix and
`numWords` little-endian `U4` values (`dwrd`). Prefix fields are `gnssId`,
`svId`, `sigId`, `freqId`, `numWords`, `chn`, `version`, and `reserved0`.
There is no payload timestamp or per-record navigation CRC flag. `freqId`
is GLONASS-specific; message version is distinct from protocol version.
[u-blox interface description](https://content.u-blox.com/sites/default/files/documents/u-blox-F9-HPG-1.51_InterfaceDescription_UBXDOC-963802114-13124.pdf#page=201).

The ZED-F9P Integration manual, UBX-18010802 R16, section 3.15.1,
pages 74-82, describes complete, parity-checked output and receiver-side
inversion handling. GPS LNAV and BeiDou use ten words with per-word padding;
Galileo uses signal-dependent page layouts. Its SBAS diagram describes eight
words, while its summary lists nine. These are not grounds to discard an extra
word or assert a universal packing rule.
[u-blox integration manual](https://content.u-blox.com/sites/default/files/ZED-F9P_IntegrationManual_UBX-18010802.pdf?hash=undefined#page=74).

SBF reference: mosaic-X5 firmware 4.15.0 Reference Guide, sections 4.1.3
and 4.2.2, pages 257 and 276-292. Navigation blocks use `NAVBits` words;
common fields include `TOW`, `WNc`, `SVID`, `Source`, `RxChannel`, and
block-dependent checks/diagnostics. SIS timestamps mark transmission-end of
the last contributing bit, not receiver arrival. Representative payload sizes:

| SBF blocks | Exported content length |
| --- | --- |
| `GPSRawCA`, `GPSRawL2C`, `GPSRawL5` | 300 bits |
| `QZSRawL1CA`, `QZSRawL2C`, `QZSRawL5` | 300 bits |
| `GALRawFNAV`, `GALRawINAV`, `GALRawCNAV` | 244, 234, 492 bits |
| `GEORawL1`, `GEORawL5` | 250 bits |
| `BDSRaw` | 300 bits |
| `BDSRawB1C`, `BDSRawB2a`, `BDSRawB2b` | 1800, 576, 984 binary symbols |
| `NAVICRaw` | 292 bits |

`GALRawINAV.Source` can indicate combined E1/E5b sub-pages; its layout removes
the even-page tail. `BDSRawB1C` has separate `CRCSF2`/`CRCSF3` checks.
`CRCPassed` and `ViterbiCnt` are not universal. GPSRawCA's parity treatment is
specified separately. Transport CRC is distinct from navigation validity.
[Septentrio reference guide](https://docs.sparkfun.com/SparkFun_GNSS_mosaic-X5/assets/component_documentation/firmware/mosaic-X5_Firmware_v4.15.0_Reference_Guide.pdf#page=257).

Design consequence: validate a canonical mapping for each signal and receiver
revision before emitting CommonNEX records. Equal message families and equal
lengths do not establish equal bit sequences. Receiver-export words can be
internal importer state or diagnostics while a mapping is investigated, but
are not a compliant CommonNEX payload. Navigation symbols after receiver error
correction also need a family-defined representation; do not label all payloads
as unmodified transmitted data bits.

#### 5.4.2 Proposed common fields

| Field | Type | Meaning |
| --- | --- | --- |
| `stream_id` | `string` | Acquisition stream |
| `raw_bits_id` | `uint64` | Record identity within the stream, retained across batches and parts |
| `gpst_ns` | `uint64?` | Applicable normalized timestamp, when resolvable |
| `time_role` | enum | `reception`, `transmission`, `navigation_epoch_context`, or `unknown` |
| `time_reference` | enum | `unit_start`, `unit_end`, `epoch`, or `unknown` |
| `time_basis` | enum | `source_field`, `stream_association`, `decoded_navigation`, or `unknown` |
| `epoch_id` | `uint64?` | Associated epoch within the stream, if available; not a substitute for time role |
| `satellite_system`, `satellite_number` | `string?`, `uint16?` | RINEX satellite identity where mapping is known; otherwise retain source identity |
| `signal_sources` | list of records | Known contributing signals, using system-specific RINEX band/attribute or native identity; may be empty if unknown |
| `signal_composition` | enum | `single`, `combined`, or `unknown`; does not imply an ordering of contributing signals |
| `message_family` | `string` | Standards-defined navigation family; not an observable code or receiver message ID |
| `body_format` | `string` | Canonical layout and revision defined for the navigation family, independent of receiver protocol |
| `content_kind` | enum | `navigation_bits` or `binary_symbols`, as fixed by the canonical family definition |
| `bit_length` | `uint32` | Number of meaningful bits under that layout |
| `body` | `bytes` | Canonical navigation content; parsing does not require a UBX/SBF packing decoder |
| `unit_kind` | enum | `word`, `fragment`, `page`, `subframe`, or `message`, as defined by the family |
| `completeness` | enum | `complete` or `partial`; partial records require a family-defined fragment representation |
| `source_identity` | typed record? | Native satellite/signal identifiers and source message ID/revision where scientifically relevant; not instructions for unpacking the body |
| `receiver_channel` | `uint16?` | Source tracking channel; never a permanent satellite/signal identity |
| `source_diagnostics` | typed record? | Applicable receiver-specific diagnostics with their native definitions |
| `checks` | list of records | Scoped receiver or independently evaluated checks; empty when none are known |

Each `signal_sources` entry contains nullable `signal: string`, optional typed
`source_identity`, and `component_scope: string?`. A combined report must not be
presented as exclusively received on the receiver's nominal signal code.
Do not infer which constituent page came from which signal without evidence.
An empty list means unknown contributors, not a signal-less broadcast.

Each `checks` entry has `origin` (`receiver` or `independent`), `kind`
(`crc`, `parity`, `bch`, or `unknown`), `scope: string`,
`result` (`pass`, `fail`, `unknown`, or `not_applicable`), and `evidence`
(`source_field`, `documented_output_policy`, or `computed`). The optional
`source_field: string?` identifies a native field when applicable. Scope names are defined
by `body_format`, for example a whole navigation unit or a numbered subframe.
The schema does not collapse multiple checks into a single successful boolean.
Receiver output policy is evidence distinct from an explicit per-record flag.

Processor acceptance is derived policy, not a property of the received bits;
it is omitted from this general record. The existing SBAS processing product
may retain its own acceptance field. Fragment/assembly context remains an
extension point, not a mandatory generic sequence model: do not invent sequence
numbers absent from the source. Transport truncation is not automatically a
legitimate navigation fragment.

#### 5.4.3 Parameter retention and adapter decisions

The following are proposed mappings based on the survey, not implemented APIs.

| Parameter group | Proposed retention |
| --- | --- |
| Identity | Normalize justified satellite/signal mappings; retain typed native identifiers for unresolved or representation-specific cases |
| Time | Preserve normalized time, its reference point, and how it was assigned; retain native time fields only where needed to interpret unresolved timing |
| Layout | Use a canonical family layout/revision; source message/block revisions select the importer mapping, not the downstream payload decoder |
| Channel | Map tracking-channel identity to `receiver_channel`; unknown remains null |
| Checks | Map each applicable check separately, with its scope and evidence; never equate transport validation with navigation validation |
| Diagnostics | Preserve defined, applicable source metrics; do not manufacture a shared quality score |
| Counts | Derive word counts from preserved word arrays when exact; retain separately only if the normalized representation no longer expresses the source count |
| Reserved fields | Remove documented padding; retain unexplained words in the raw archive or importer diagnostics, and report their exclusion from canonical content |

For UBX, an associated epoch supplies context time, not a measured arrival time.
Map `chn` to channel identity. Source identity and mapping selection use the exact `gnssId`,
`svId`, `sigId`, and `version` where required; protocol/firmware belongs in
stream metadata. A receiver-policy check may be recorded only for a documented
applicable version. Independently checking a normalized body requires the
right parity convention, not simply replaying a transport checksum.

For SBF, retain the complete meaningful interpretation of `Source`, including
combination flags, rather than masking it down to a nominal signal index.
Keep `CRCSF2` and `CRCSF3` as separate scoped results. Preserve applicable
`ViterbiCnt` in the `sbf` diagnostics namespace; not-applicable fields become
absent, not measurements of zero errors. Map known SIS time to `transmission`
and `unit_end`, with `source_field` as its basis. Preserve incomplete native
time information in an untimed record when needed; do not invent a week.

Names in the installed/generated schema can differ from the manual's display
names. The current SBF adapter accesses `NavBits` and decoded `SigIdx`, whereas
this document survey uses `NAVBits` and `Source`. Implementation must reconcile
these explicitly and retain defined flag bits, not assume a field-name match
or that an existing SBAS-only extraction covers every navigation block.
See [current SBF extraction](../libcppgnss/src/sbf.cpp) and
[current UBX subframe decoding](../libcppgnss/src/ubx_subframe.cpp).

#### 5.4.4 Payload preservation and scientific consumption

Canonical normalization is an agreed requirement. Field names, enum spellings,
and individual family layouts remain proposals. For canonical bit layouts,
bit order is MSB-first: body bit zero occupies bit 7 of byte zero. Unused low
bits in the final byte are zero. Each `body_format` specifies included bits,
parity/FEC/deinterleaving or other receiver transformations when known, and the
meaning of the check fields. Do not claim original over-the-air bits when the
receiver exports a transformed representation.

The logical `body` is a canonical bit/symbol sequence.
The byte convention describes that field's content, not a CommonNEX transport
envelope or mandatory in-memory container. A transport may carry typed words
instead of bytes if it preserves the declared logical value exactly.

Define canonical payloads by navigation family, for example GPS LNAV/CNAV,
Galileo I/NAV/F/NAV, the individual BeiDou navigation families, and SBAS L1.
Different families may have different sizes and structures; different receiver
protocols must not create alternative payload definitions for the same family
and canonical revision.

Every family definition must specify:

- The independently stored unit and any permitted fragment representation.
- Exact bit count/order and placement of headers, data, CRC/parity, and tails.
- Treatment of inversion, interleaving, FEC, and receiver transformations.
- How source-omitted or irrecoverable information is represented, including
  any required availability fields; never invent missing bits.
- Time reference points and the scopes of validity checks.

For equivalent navigation content, UBX and SBF adapters must produce equivalent
canonical payloads and explicitly represented availability, even when source
quality diagnostics or acquisition times differ. Source-only information needed
for interpretation belongs in typed auxiliary fields, not a second opaque
payload that scientific consumers must unpack. Resolving differences in parity,
tails, and receiver-added information is part of defining each family mapping;
neither discarding them without review nor assuming the source words match is
acceptable.

An unknown navigation message type inside a known canonical structure remains
storable without decoding its ephemeris/correction fields. An unknown receiver
packing or unresolved canonical mapping is unsupported input for this family:
report it explicitly and retain the original archive. Importer staging or
diagnostics may hold exported words, but neither CommonNEX nor ParquetNEX treats
them as a compliant fallback. Checksums alone do not establish a valid mapping.

Preserve failed navigation checks and family-defined partial units when their
canonical content is extractable. Do not manufacture fragments from truncated
transport frames. Report normalization exclusions separately from downstream
message-decoder limitations and successfully retained records.

For `sbas_l1_250`, persist exactly the 250-bit SBAS L1 message body, including
its preamble, message type, data, and CRC, in 32 bytes with six zero padding
bits. Remove UBX/SBF envelopes and receiver word-storage padding. This layout
does not accept SBAS L5 bodies. Native signal identifiers remain available in
raw archives/decoder APIs; a generic extension mechanism does not require
persisting them in the SBAS body product.

If an input has unexplained extra navigation words, retain them in the original
archive or importer diagnostics and report the canonical mapping's retention
limit. Emit the known 250-bit body only if its boundaries and interpretation
are established independently of those extra words. That record does not claim
complete preservation of every exported receiver word. If the extra information
affects interpretation, resolve the mapping before emitting a canonical record.

The receiver's CRC result and an independent body check remain distinct. An
unperformed check is absent or explicitly unknown, not success. A partial SBAS
payload is retained only under a defined canonical fragment layout, never
zero-filled into `sbas_l1_250` or stored as opaque receiver words.
Downstream processors select layouts, completeness, and validity they support;
failed or unknown checks do not prevent general raw-bit preservation.

Navigation epoch association must not be mislabeled as receiver message time.
Additional known time roles can be represented by separately named timestamps;
their detailed schema is open. Untimed raw bits may be retained with null time
and source/stream association; a timed scientific consumer must reject or
exclude them explicitly. This does not relax the existing SBAS grid boundary:
bodies supplied to that processor require time resolved during extraction.
The Parquet layout needs an explicit unassigned-time location; do not infer a
GPST partition from source filenames or arrival wall-clock time.

Retain generic continuity/end events alongside bodies. Neither daily partition
boundaries nor replay batches reset SBAS masks, message aging, or signal state.
RINEX input that does not contain these bodies cannot populate this family;
report that source limitation rather than manufacturing bits from decoded NAV.

### 5.5 Navigation and events

| Family | Required semantic boundary |
| --- | --- |
| Navigation | System, satellite, message type, relevant GPST times, native time fields, issue identifiers, health/validity, and typed message-specific parameters |
| Events | Stream/epoch association, event kind, applicable time, and source evidence or reason |

Do not flatten all constellation/message variants into a single set of
GPS-like ephemeris fields. Reception time, transmission time, and ephemeris
reference time are not interchangeable. A scientific consumer must not reopen
raw input merely to recover the timestamps required to interpret a record.

The existing SBAS intermediate boundary remains a 250-bit body with GPST,
signal identity, validity, and continuity/end information. CommonNEX does not
require adding UBX/SBF envelopes, field dictionaries, or source offsets to it.
Adapting existing SBAS field names to this proposed model is future work, not
an implicit change to current files.

Source extensions use namespaces such as `ubx` and `sbf` and preserve native
field names. Extensions have declared types and units; arbitrary JSON blobs
are not the common scientific interface. Raw envelopes and unknown payloads
remain available in raw archives and decoder APIs.

### 5.6 Receiver telemetry

Receiver telemetry is a first-class CommonNEX record family persisted by
ParquetNEX. It covers receiver-reported estimates and diagnostics as well as
sensor readings. It is not limited to fields present in both UBX and SBF or
representable in RINEX. Preserve each available quantity at its native reporting
cadence; do not require an observation at the same instant.

Proposed common context for each telemetry record:

| Field | Type | Meaning |
| --- | --- | --- |
| `stream_id`, `telemetry_id` | `string`, `uint64` | Logical stream and record identity within it |
| `metadata_id` | `string` | Applicable receiver/configuration metadata |
| `gpst_ns` | `uint64?` | Normalized sample or association time |
| `epoch_id` | `uint64?` | Associated navigation/observation epoch, when justified |
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
| `clock_bias_ps` | `int64?` | ps; 1 | Receiver time minus declared reference time |
| `navigation_time_offset_ns` | `int32?` | ns; 1 | Resolved navigation epoch minus its explicitly identified nominal epoch |
| `pps_offset_ps` | `int32?` | ps; 1 | Actual pulse edge minus its specified ideal edge, positive when late |
| `pulse_quantization_error_ps` | `int32?` | ps; 1 | Pulse-generator quantization error with a defined sign mapping, distinct from total PPS offset |
| `receiver_temperature_mdeg_c` | `int32?` | degC; 1/1000 | Physical temperature at least -273.15 degC; sensor location/meaning retained |
| `receiver_uptime_ms` | `uint64?` | ms; 1 | Reported uptime, not extrapolated; preserve source resolution and counter wrap semantics |
| `time_accuracy_ps` | `uint64?` | ps; 1 | Nonnegative; retain the source's accuracy definition |
| `frequency_accuracy` | `uint64?` | ppb; 1/1000000 | Nonnegative; retain the source's accuracy definition |

These integer types and scales are agreed for v0, not measured fidelity guarantees.
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

All offsets are signed numerical measurements, not absolute `gpst_ns` values.
Navigation time offset uses integer nanoseconds; its expected magnitude is
typically within 1 ms, but that expectation is not a hard schema bound. Its
`int32` domain is -2,147,483,648 through 2,147,483,647 ns (about +/-2.147 s).
PPS offset uses integer picoseconds with the same integer bounds (about
+/-2.147 ms), preserving subnanosecond pulse information independently of the
integer-nanosecond absolute epoch. Other offsets retain their declared types;
in particular, pulse quantization error remains a separate quantity.

Convert source units before storage: ns to ps multiplies by 1,000. Integer
source values must use sufficiently wide intermediate arithmetic. For a source
not exactly representable at the selected resolution, use nearest-integer,
half-to-even rounding and report precision reduction. Check the rounded result
against the target range before narrowing. Overflow produces null plus an
explicit diagnostic retaining the source value; never wrap, saturate, or
reinterpret a pulse-cycle-sized error modulo the pulse period. Canonical unit
conversion does not imply that the receiver achieves picosecond accuracy.

At the proposed frequency scale, one stored step is 0.000001 ppb, or 1e-15
relative frequency. Nearest rounding adds at most 0.0000005 ppb error. This
may still round very small floating-point source values; it is not exact for
every possible SBF float. Integer ps bias has a range of approximately +/-106.75
days and at most 0.5 ps rounding error. Temperature adds at most 0.0005 degC
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
  See [current receiver-clock semantics](receiver-clock.md).
- `UBX-TIM-TP.qErr` is a picosecond quantization-error field concerning the next
  pulse, with validity and reference flags. It is not a direct measurement of
  total PPS offset. Preserve its integer-ps value and pulse association;
  validate sign mapping before assigning the canonical error field.
  [u-blox time-pulse definition](https://content.u-blox.com/sites/default/files/documents/u-blox-F9-HPG-1.51_InterfaceDescription_UBXDOC-963802114-13124.pdf#page=211).
- SBF PVT `RxClkDrift` is in ppm and `RxClkBias` references `TimeSystem`;
  verify units for each block rather than all similarly named fields.
  `xPPSOffset.Offset` is in ns, negative for an early pulse; convert to canonical
  integer ps with the rounding/range policy above and retain `TimeScale`
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

### 5.7 One-pass extraction contract

Decode each source stream once and route all supported records to observation,
navigation, raw-bit, telemetry, metadata, and event batches. Share time/epoch
association and maintain state across file/day boundaries. Consumers may read
only the families they need; recording telemetry must not require enabling a
PPP, SBAS, or clock-analysis algorithm.

Bounded buffering may be needed to resolve a later time anchor, companion block,
or pulse association. Unresolved timing remains explicit at finalization.
Permissive reconciliation applies to telemetry too: logging copies can collapse,
but equal numerical readings at different occurrences or from different sensors
must not. A navigation EOE does not finalize an unrelated future pulse record.

"One pass" means a single sequential raw decode for all supported extraction
families, not a guarantee that unknown/proprietary fields or information never
logged can be recovered. Report extraction coverage and unsupported relevant
messages. Adding a new quantity requires a typed semantic mapping, not a generic
packet dump. Existing archives remain available for future unsupported analyses;
overlap reconciliation may still read affected existing ParquetNEX partitions.

## 6. Normalization and scientific state

Input adapters decode framing and source conventions, map identities, normalize
units and time scales, and expose validity. They must account for RINEX header
semantics such as scale factors, phase shifts, clock corrections, and changes
in metadata. Necessary application state must remain explicit so readers do
not apply a correction twice. Exact per-field mapping rules need review.

Import does not silently smooth measurements, fill gaps, repair cycle slips,
remove clock bias, choose a conflicting value by arrival order, or estimate
missing values. Exact duplicate elimination and compatible overlap merging are
explicit import operations defined in section 7, not scientific corrections.
Source-reported corrections already applied by a receiver are retained as
interpretation metadata. Inferred arcs, additional corrections, and filter
results are derived products, not mutations of imported observations.

Source lock-loss/reset events and algorithm-inferred discontinuities are
distinct. File EOF, a Parquet part boundary, and GPST midnight do not reset
tracking, clock, SBAS aging, or navigation assembly state.

## 7. Incremental and live processing

### 7.1 Permissive overlap reconciliation

Permissive import is an agreed requirement. The default accepts overlapping
time ranges from multiple logging sources of the same logical acquisition,
including repeated imports and late additions to an existing day. It must not
require a separate restitch/QA run or reject an entire input because its bounds
overlap earlier data. Canonical normalization, framing, scientific validity,
and the GLONASS exclusion still apply.

Each logging source is framed and associated independently before records are
reconciled into a logical stream. Do not concatenate overlapping byte streams
or splice unrelated packet fragments because their times appear adjacent.
Byte-level overlap repair, where needed, requires actual source alignment
evidence and remains an importer operation.

| Relationship between input records | Default import behavior |
| --- | --- |
| Proven duplicate occurrence, equal canonical content and interpretation | Keep one logical occurrence and associate its contributing sources |
| Same established epoch, different satellites/signals/observables or record families | Retain the union of records |
| Same occurrence with compatible additional metadata or quality information | Retain the additional information without duplicating the observation |
| Same apparent occurrence with different values, bits, corrections, or incompatible metadata | Preserve alternatives and explicitly mark the conflict; do not average or use last-write-wins |
| Uncertain occurrence identity or acquisition association | Preserve separately and mark unresolved association; do not force a merge |
| A source is incomplete but another supplies the missing records | Use the actual complementary records; retain unresolved completeness where evidence is insufficient |

Time equality is a candidate lookup, not a deduplication proof. Match identity
within the receiver/antenna and acquisition context, including restarts,
canonical family/observable, time role, and applicable corrections. Nanosecond
rounding can make distinct source timestamps equal. Do not snap nearby epochs
together or apply an implicit numeric tolerance to observation values.

For navigation bits, identical content may be broadcast repeatedly. Payload
equality alone does not identify the same received occurrence. In particular,
SBF transmission-end time and UBX epoch-context time are not directly equal
occurrence keys. A justified association rule is required before collapsing
cross-source copies. Without it, preserve both with unresolved association.

Different source formats may expose different precision or correction states.
A RINEX-derived value is not an exact duplicate of a raw-derived value solely
because they are numerically close. Any future equivalence/preference rule
must account for those transformations explicitly. Contradictory flags remain
source-attributed evidence rather than being combined into apparent certainty.

Proposed minimal logical relations for reconciliation:

- `RecordSource`: canonical record reference and `logging_source_id`.
- `RecordConflict`: `conflict_id`, candidate record references, and a typed
  reason such as differing value, body, metadata, or ambiguous occurrence.

These relations must survive persistence/replay where needed to avoid treating
alternatives as independent observations. Exact relation types and reference
keys remain review items. They are small functional metadata, not source-offset
inventories or artifact provenance bundles. Existing SBAS analysis products
retain their current body-only boundary; reconciliation relations belong to
the general import dataset, not a requirement to expand those products.

Keep canonical IDs stable for already imported occurrences. Reimporting the
same material should not add duplicate scientific records or source relations;
input order must not silently decide retained values. Candidate IDs for genuinely
different or conflicting records remain distinct. If an importer lacks enough
evidence to recognize a cross-source duplicate, report that limitation instead
of claiming full deduplication.

Conflict handling must be explicit in consumers: stop, exclude the affected
occurrence, or apply a declared source-selection policy. A tolerant importer
does not authorize a solver to count conflicting alternatives as separate
measurements. Source selection is a processing view and preserves alternatives.

### 7.2 Completion, late data, and state

Proposed logical completion events distinguish `epoch_complete`,
`stream_end`, and receiver/continuity events. Completion refers to records
emitted for an epoch, not a claim that all expected signals were observed.
Batch completion is a transport concern and carries no scientific meaning.

Source-local completion and merged-stream completion are different. Closing one
logger or receiving its EOE does not close other sources' contributions. The
merger uses a declared source set/completion policy or live buffering window;
no generic network synchronization protocol is prescribed.

An adapter emits epoch completion when its protocol-specific association rule
has closed that epoch. UBX EOE is one source of this evidence; other adapters
use their documented rules. An incomplete tail at daily input EOF must not be
declared complete solely because the import invocation ended.

Adapters can carry pending state across invocations or reread a bounded input
overlap to resolve boundary records. The format does not prescribe how that
state is encoded. A daily import must handle pending epochs and navigation
messages explicitly and report unresolved tails.

Stream/epoch/observation identities allow repeated timestamps without silent
overwriting. Overlap reconciliation maintains those identities; assigning new
IDs to every import is not deduplication. Exactly-once delivery is not guaranteed
by the schema or transport, so the importer must tolerate repeated input.

Ordering and late-data policy belong to the producer/consumer contract. A live
consumer may require ordered completed epochs, buffer a declared late window,
or decline immediate processing of late records. The importer still accepts
and reconciles recoverable late data for persistence/replay. V0 does not require
watermarks, record retractions, or an update protocol for already emitted
epochs. Proposed default: an epoch completed for a consumer is not silently
amended; later information is reported for explicit replay or reprocessing.

Report newly contributed records, duplicates, conflicts, unresolved associations,
and affected GPST ranges. A newly added reset/event or navigation record may
invalidate state beyond the overlap itself; each consumer determines the needed
replay interval, not merely the importer-reported bounds.

Combine actual complementary coverage; never interpolate missing observations
or infer gapless sampling from continuous file bounds. Coverage is specific to
record family, signal, and expected cadence where known. If every source missed
an occurrence, the gap remains. Logger handover itself is not a receiver reset,
but observed receiver resets and real discontinuities remain effective after
merging. Required processing history must carry across the handover.

Incremental storage and incremental computation are separate. A processor may
need earlier navigation, metadata, or filter history when consuming a new day.
It may restore its own checkpoint or replay sufficient earlier records.
CommonNEX does not serialize generic algorithm checkpoints or guarantee that
an isolated daily partition is sufficient to initialize every calculation.

## 8. ParquetNEX serialization proposal

ParquetNEX defines an interoperable mapping of CommonNEX logical records to
Parquet schemas, file metadata, and dataset layout. Unlike CommonNEX transport,
these mappings must eventually be specified rather than left writer-specific.

- Represent `gpst_ns` with Parquet's unsigned 64-bit integer logical annotation
  over its 64-bit integer physical storage. Do not use the standard Parquet
  `TIMESTAMP` annotation for GPST. Readers must preserve unsigned semantics.
- Preserve nullability, semantic identifiers/constraints, enums, units, and
  source-extension definitions. The schema version resolves standard semantic
  definitions; store additional field constraints where needed. Writers validate
  the logical rules rather than assuming Parquet physical types enforce them.
  Serialize logical null as Parquet null, not NaN or infinity. Convert any
  implementation-specific NaN missing-value representation at this boundary.
  Preserve declared integer widths and canonical rational scales. Retain a
  floating-point physical type only for an explicitly justified logical float
  field; do not expand scaled integers to floats on disk. Round trips must
  preserve integer counts, nullability, units, and scale exactly.
  Store schema versions, record family, GPST epoch/unit, and necessary schema
  interpretation in file metadata. Scientific interpretation does not require
  provenance sidecars, execution snapshots, or artifact hash inventories.
- Partition timed records by stream and GPST day. Each day may contain several
  complete part files per record family. Payload timestamps determine coverage;
  filenames alone are not evidence of observation time or continuity.
- Write bounded parts, close them, and publish by rename. Published part files
  are immutable. Disjoint additions can add parts; overlapping additions are
  reconciled with affected existing records before publishing the updated scope.
  No in-place append to a published Parquet file is required.
- Proposed observation layout: group related `C/L/D/S` observations for an
  epoch/satellite/system-specific signal into columns. Preserve full observable
  identities and per-observable quality. Repeated measurements must remain
  distinguishable. The exact schema is an open item to compare with scalar rows.
- Metadata records referenced by a part must be resolvable from its declared
  dataset scope. Include applicable metadata with daily exports so reading an
  export does not depend on an earlier live connection. Repeated metadata with
  the same identity must have identical content.
- Compression, row-group sizing, sorting, and optional compaction are physical
  choices to measure. Do not promise a compression ratio relative to RINEX;
  compare equal retained content, including compressed RINEX baselines.

Directory names, part naming, concrete family schemas, metadata keys, and the
representation of cross-day completion/events remain to be specified. A v0
implementation should choose one mapping rather than support many layouts.

Daily imports default to merging an already imported scope. Repeating an input
should be a scientific no-op; complementary late input extends coverage.
Readers must see the reconciled record set and explicit conflict alternatives,
not a bag of overlapping copies that every algorithm must deduplicate again.
Proposed initial strategy: read the affected stream/day scope, reconcile in
bounded batches, and publish replacement files as a complete scope by rename,
retaining a recoverable previous scope. Inspect adjacent partitions when
association or completion crosses midnight. Unaffected scopes remain untouched.

Routine overlap merging is not destructive replacement: old unique records,
diagnostics needed for interpretation, and conflict alternatives survive. An
explicit overwrite that intentionally discards existing records is a separate
operation with a resolved target. Preserve unrelated parts and all raw inputs.
No generic transaction log or resume framework is required. Per-file rename
alone does not promise atomic publication of a multi-file dataset; grouped
publication must avoid exposing both superseded and replacement parts as active
or losing concurrent additions. The initial implementation may serialize writers
per affected scope and must define the reader-visible publication boundary.

## 9. Implementation direction and review items

Preserve the dependency direction and responsibilities in
[Native architecture](native-architecture.md): reusable decoding and GNSS
interpretation in `libcppgnss`, Observatory association/state and batch bindings
in `libneognss-obs`, and file orchestration/Parquet in Python. Native processing
remains batched and releases the GIL. Choosing Arrow for a future implementation
is optional and does not introduce a C++ Arrow dependency through this draft.

Proposed first implementation slice: UBX/SBF/RINEX observations into CommonNEX,
ParquetNEX write/read, and a consumer using the existing GPS L1/L2 PPP subset;
also preserve receiver-exported raw bits across in-scope constellations,
through validated canonical mappings, independently of scientific message-decoder
coverage. Use the existing UBX/SBF SBAS L1 path to
verify a canonical body layout and ParquetNEX replay into SBAS grid processing,
plus representative non-SBAS and undecoded payloads to check bit preservation.
This is a validation scope, not a permanent restriction of the logical model.
Navigation and receiver-specific catalogs can then expand with actual consumers.
The initial importer must also emit available mapped telemetry in that same raw
pass; downstream plotting or clock-analysis integration can follow separately.

Review before implementation:

1. Approve or revise the proposed identity fields and their scope across daily
   imports, multiple antennas, logging sources, duplicates, and reimports;
   finalize source/conflict relations and occurrence-equivalence rules.
2. Specify canonical clock-offset sign and correction-application fields,
   plus exact RINEX scale-factor/phase-shift mapping rules.
3. Finalize quality/null semantics, source-extension schemas, and unsupported
   or untimed-record behavior.
4. Define protocol-specific epoch completion and late-data contracts.
5. Choose one Parquet observation layout and finalize file metadata, metadata
   resolution, partitions, and incremental publication/replacement rules.
6. Enumerate supported RINEX versions, systems, observation codes, and navigation
   variants for the first adapters. GLONASS remains excluded.
7. Define canonical navigation-family layouts, fragment context,
   source identity, scoped checks, contributing signals, and untimed storage.
   Use the UBX/SBF survey in section 5.4.1 to validate each mapping. Distinguish
   normalization coverage from scientific navigation decoding coverage; SBAS
   must not be the only preservable payload. Validate UBX/SBF equivalence for
   each family, including padding, parity/tails, and source-omitted information.
8. Finalize telemetry references, pulse association/sign conventions, temperature
   sensor mappings, and a UBX/SBF message coverage list for one-pass extraction.
9. Validate conversions into the agreed fixed-point telemetry scales and ranges.
   Keep raw-observable, uncertainty, and epoch clock-offset numerical types as
   specified; they are not pending conversion to scaled integers.

Use small representative inputs and direct inspection to compare raw-path and
Parquet replay inputs: timestamps, values, identities, validity, corrections,
and cross-day continuity. Measure size and read/write costs. No new automated
tests or full-archive conversion are authorized by this documentation draft.
Include overlapping logger inputs, repeated imports in different orders,
complementary gaps, conflicting observations/bodies, and a cross-midnight
handover in representative manual validation. Check that unique records survive,
duplicates do not multiply, and irrecoverable gaps remain visible.

## References

- [Current GPST policy](time-policy.md): existing processing representations.
  The CommonNEX nanosecond normalization and unsigned type above are proposed
  format decisions, not retroactive reinterpretations of existing products.
- [Current RINEX conversion limitations](rinex-conversion.md).
- [RINEX 4.02 specification](https://files.igs.org/pub/data/format/rinex_4.02.pdf):
  naming and semantics reference, not a promise of full v4.02 support.
- [Parquet logical types](https://parquet.apache.org/docs/file-format/types/logicaltypes/).
- [Parquet compression](https://parquet.apache.org/docs/file-format/data-pages/compression/).

This project-owned draft is licensed under GPL-3.0-only, as specified in
[LICENSE](../LICENSE). Referenced external standards retain their own notices.
