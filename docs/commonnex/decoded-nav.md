# CommonNEX DecodedNav extension

Status: v0 design draft; not an implemented format or API.

[Overview](overview.md)

## Scope

DecodedNav is optional and independent of RawBits. It represents decoded
broadcast navigation parameters supplied by an adapter or derived by a decoder.
A Core observation consumer may instead obtain navigation products externally.
Do not manufacture raw navigation occurrences from decoded ephemerides.

Records belong to a navigation collection, not necessarily a receiver station.
Standalone or merged RINEX NAV input does not require a fabricated Setup or
station. Receiver-derived records may retain acquisition context; a
Navigation-context time is not the ephemeris reference epoch.

Unlike RawBits's epoch-only timing, decoded parameters retain the reference
times required by their scientific definitions, including native GNSS time
fields where necessary. Normalized processing axes use GPST. Do not replace
ephemeris reference times with the containing navigation epoch.
Navigation reference times retain native GNSS time and an additional Core
`GpstTimestamp` coordinate, as defined below. Normalized fit
intervals and ages use `Duration`; time-valued clock biases, group delays and
system-time offsets use `TimeDelta`. All three use `DECIMAL(38,12)` seconds.
Native week/TOW fields and model coefficients retain their standards-defined
units and meanings. Drift and drift-rate coefficients are not durations merely
because they describe clocks. Continuous non-time physical parameters may use
`float64`; issue identifiers, flags and counters retain integer types.

Use separate family schemas, not one universal GPS-like field set. Decoding
RawBits into DecodedNav is optional and need not happen during import.

## Common header and typed payloads

The collection identifier is collection/file metadata, not a repeated row
field. No mandatory record ID or RawBits derivation-reference graph is required.
Optional file-local counters have no cross-file/revision stability requirement.
Collection metadata encoding still needs a physical mapping definition.

| Field | Type | Meaning |
| --- | --- | --- |
| `record_kind` | enum | `EPH`, `STO`, `EOP`, or `ION` |
| `satellite_system` | `string` | System providing the navigation information |
| `message_type` | `string` | Applicable RINEX 4 navigation message classification |
| `message_subtype` | `string?` | Standard subtype when defined and known |
| `acquisition_context` | struct? | Actual receiver acquisition context, when known |
| `eph` | typed struct? | Ephemeris parameters |
| `sto` | typed struct? | System time offset parameters |
| `eop` | typed struct? | Earth orientation parameters |
| `ion` | typed struct? | Ionospheric model parameters |

Exactly one payload struct is non-null, matching `record_kind`. Within each
kind, the system/message type/subtype selects a defined model-specific struct;
do not use JSON or an arbitrary parameter-name-to-number map. The physical
nullable-struct branches must be enumerated as models are defined.

Logically this is a tagged union. EPH branches distinguish parameter models,
not only constellations: GPS LNAV/CNAV/CNAV-2, Galileo I/NAV-F/NAV,
BeiDou D1-D2/CNV1/CNV2/CNV3, QZSS LNAV/CNAV/CNAV-2, and SBAS.
Shared structures do not erase `message_type` distinctions. The discriminator
is the record kind, system, message type, and applicable subtype together:
`CNV2` alone, for example, cannot distinguish GPS from BeiDou.
Parquet represents unions using mutually exclusive nullable typed structs;
the active branch must match the discriminator. Native code may use
`std::variant`; no inheritance-based universal optional-field EPH is required.

See [additional navigation models](navigation-models.md) for the selected
BeiDou, GPS modernized, QZSS, SBAS, STO, EOP, and ION definitions. Their
source-specific ranges, equations and availability mappings remain explicitly
separate from implemented support.

`acquisition_context` contains `setup_id: string` and
`nav_epoch_gpst: GpstTimestamp?`, stored directly rather than referencing an
epoch table. Necessary multi-page acquisition context may retain multiple
explicit times, but they are not unique occurrence keys. No mandatory RawBits
row-reference graph is required. Do not fabricate receiver acquisition context
for external RINEX/RTCM3 records.

There is no ambiguous common `gpst` field. Each model defines its reference
times. EPH identifies the satellite it describes; STO identifies source and
target time systems; EOP and ION retain their model-specific reference and
applicability information. A known broadcasting satellite for system-level
parameters is not their applicability scope; its exact field mapping remains
to be specified with those models.

Use RINEX 4 record kinds and parameter semantics without its ASCII widths,
line positions, or floating-point representation of integer codes. Its
STO/EOP/ION classifications can aggregate navigation families (for example
`IFNV` and `CNVX`); `message_type` therefore does not uniquely select a RawBits
`body_format`. Keep those classifications separate. RINEX 3 adapters infer
classification only where the supported version and source fields justify it.
Do not guess a more specific subtype.

## Native GNSS reference times and GPST

Use `NavigationTime` for model reference times (including TOE/TOC and applicable
STO/EOP/ION reference times) and known transmission times across all systems.
Native time means the GNSS model's own time scale, not receiver-local time,
host time, file creation time, or an optional UTC processing mode.

| Field | Type | Meaning |
| --- | --- | --- |
| `native_time_system` | enum | Explicit native GNSS time scale, such as GPST, GST, or BDT |
| `native_seconds` | `DECIMAL(38,12)` | Continuous seconds from that time scale's specified origin |
| `gpst` | `GpstTimestamp` | Additional normalized GPST coordinate |
| `conversion_method` | enum | `identity`, `nominal_alignment`, or `broadcast_model` |
| `conversion_model_ref` | reference? | Actual time-offset model used for `broadcast_model`; otherwise null |

`identity` applies when native time is already GPST. `nominal_alignment` applies
the standard origins and fixed relationships without a fine inter-system
offset correction. `broadcast_model` additionally applies a supported broadcast
time-offset model, whose reference must resolve. Missing offset models permit
nominal alignment rather than rejection; never label nominal alignment as a
model-corrected synchronization. Native seconds remain authoritative for the
native navigation model, allowing downstream conversion to be recomputed.

Resolve week rollover at import. Native seconds are not ambiguous seconds of
week. Each supported time scale must define its origin and nominal conversion;
the exact registry and model equations remain adapter definition work. Use
Core decimal precision/rounding rules, not binary64 absolute-time intermediates.
Do not silently extrapolate a broadcast model beyond its supported applicability.

Preserve native clock/orbit coefficients without reparameterizing them merely
because a GPST coordinate has been added. In particular, a native clock model
is not automatically relative to GPST. If a physical schema shares a model-level
`native_time_system`, each time field must resolve it unambiguously; do not
force STO's distinct time-system roles into one inherited scale.

Project file handling, partitions, query axes, and output labels remain GPST.
For partitioning, use the selected native reference time's **nominal GPST
alignment**, irrespective of its stored `conversion_method`. A better offset
model must not move the record across a day boundary. The corrected `gpst`
remains available for scientific use; it is not necessarily the partition-day
coordinate near midnight. Do not store a redundant `partition_gpst` field.

## GPS LNAV EPH

This is the first selected complete model: `record_kind=EPH`,
`satellite_system=G`, `message_type=LNAV`, and `message_subtype=null`.
The parameter inventory follows
[RINEX 4.02, Table A9, pages 77-78](https://files.igs.org/pub/data/format/rinex_4.02.pdf),
with the CommonNEX types and normalization rules below. This is a schema
definition, not a claim of implemented or validated adapter coverage.

### Satellite, time, and clock

| Field | Type | Meaning |
| --- | --- | --- |
| `satellite_number` | `uint16` | GPS PRN whose ephemeris is described |
| `toe` | `NavigationTime` | Orbit reference time |
| `toc` | `NavigationTime` | Clock reference time |
| `transmission_time` | `NavigationTime?` | Source-provided navigation message transmission time |
| `af0_s` | `TimeDelta` | Clock polynomial constant coefficient |
| `af1_s_per_s` | `float64` | Clock polynomial linear coefficient |
| `af2_s_per_s2` | `float64` | Clock polynomial quadratic coefficient |
| `tgd_s` | `TimeDelta?` | Group delay parameter |
| `fit_interval_s` | `Duration?` | Decoded fit interval length |

Keep TOE and TOC separately even where a standard revision requires equality.
GPS LNAV uses native GPST and `identity`: native seconds and GPST are equal.
Resolve native week/TOW and week-boundary offsets into complete GPST values;
do not blindly attach the current week. Map unknown transmission-time
sentinels to null, never to an acquisition epoch. `af2` is the quadratic
coefficient, not twice that coefficient. `af1` and `af2` remain model
coefficients, not picosecond-quantized durations. Decimal fields follow Core's
picosecond rounding rules; exact represented arithmetic does not imply exact
preservation of finer source values.

A fit interval is a length, not an assertion of validity starting at receipt.
Do not create a generic validity window from acquisition time.

### Orbit parameters

All fields below are non-null `float64` with the specified units.

| Field | Unit / meaning |
| --- | --- |
| `sqrt_a` | sqrt(m), square root of semi-major axis |
| `eccentricity` | Dimensionless |
| `m0_rad` | rad, mean anomaly at reference time |
| `delta_n_rad_per_s` | rad/s, mean motion difference |
| `omega0_rad` | rad, longitude of ascending node at the standard weekly reference |
| `i0_rad` | rad, inclination at reference time |
| `argument_of_perigee_rad` | rad |
| `omega_dot_rad_per_s` | rad/s, ascending-node rate |
| `idot_rad_per_s` | rad/s, inclination rate |
| `cuc_rad`, `cus_rad` | rad, argument-of-latitude corrections |
| `cic_rad`, `cis_rad` | rad, inclination corrections |
| `crc_m`, `crs_m` | m, radius corrections |

Preserve `sqrt_a`; do not also store its derivable square as `a_m`. Convert
source semicircles to radians at import. Do not rewrite angles or orbital
parameters into a different fitted model.

### Issue, health, and accuracy

| Field | Type | Meaning / range |
| --- | --- | --- |
| `iode` | `uint8` | Ephemeris issue, 0-255 |
| `iodc` | `uint16` | Clock issue, 0-1023 |
| `sv_health_bits` | `uint8?` | Original six-bit GPS LNAV health field, 0-63 |
| `codes_on_l2` | `uint8?` | Original two-bit code, 0-3 |
| `l2_p_data_flag` | `bool?` | Explicit source flag |
| `ura_index` | `uint8?` | Broadcast URA index, 0-15 |
| `sv_accuracy_m` | `float64?` | Source value or standard-mapped nominal accuracy |
| `fit_interval_flag` | `bool?` | Original broadcast fit flag, not a duration |

Retain URA index separately from the meter representation. Do not arbitrarily
reverse-map a numeric accuracy into an index. Preserve the standard special
meaning of URA index 15 / RINEX accuracy 8192 (use at own risk); 8192 is not an
ordinary measurement standard deviation. `sv_accuracy_m` is not an Observation
`stddev` field. Map fit flag and IODC using the applicable standard before
deriving a duration; a flag value of one does not mean one hour. A source that
only supplies a duration does not justify inventing the original flag.

### Completeness and identity

Required orbit/clock coefficients, TOE/TOC, satellite identity, and issue
fields must be present to emit this complete EPH model. An incomplete RawBits
assembly remains represented by its RawBits occurrences; do not emit a
half-populated EPH. Missing optional values are null, not zero, NaN, or an
invented healthy status.

Preserve complete ephemerides even when reported unhealthy. Solver acceptance
is downstream policy; there is no importer-generated universal `usable` flag.
Raw assembly must establish a coherent data set under the family rules, not
combine unrelated issues merely to fill the fields.

PRN plus IODE/IODC is not a unique record identity. Issue identifiers can be
reused and differing source parameters must not overwrite one another on that
basis. Automatic deduplication is not required in v0; retain distinct rows
without requiring global IDs or using model reference times as unique keys.

### Persistence

Store DecodedNav separately in the `decoded-nav` catalog with ParquetNEX's
`r00-decoded-nav-part00.parquet` naming, using the same schema
for receiver-derived and standalone navigation collections. GPS LNAV EPH is
partitioned by the nominal GPST day of native `toc`, not acquisition time. Other models must
choose their own existing reference-time field; do not add a duplicate
`partition_gpst`. A day partition does not limit scientific validity or remove
the need to select relevant adjacent-day records.

## Galileo I/NAV and F/NAV EPH

Use `record_kind=EPH`, `satellite_system=E`, `message_type=INAV|FNAV`, and
`message_subtype=null`. Both use a Galileo-specific payload structure, but
remain separate records even when satellite, TOE, or IODnav match. Do not merge
I/NAV and F/NAV clock/accuracy information. I/NAV uses the E1/E5b clock reference
combination; F/NAV uses E1/E5a. Classification must agree with source declarations.
The source parameter inventory and applicability rules follow
[RINEX 4.02, section 5.4.3 and Table A13, pages 83-84](https://files.igs.org/pub/data/format/rinex_4.02.pdf).

### Shared numerical fields

Reuse the GPS LNAV field names, types, and units for `satellite_number`, `toe`,
`toc`, optional `transmission_time`, `af0_s`, `af1_s_per_s`, `af2_s_per_s2`, and
all fields in its orbit-parameter table. These are Galileo parameters, not
GPS coefficients copied into a different system. Native reference time is GST;
retain it in `NavigationTime` together with the explicit GPST conversion.
Partition by native TOC's nominal GPST day.

Do not add GPS `iode`, `iodc`, `tgd_s`, L2 flags, or fit-interval flags to this
model. Use the following Galileo-specific fields instead.

| Field | Type | Meaning |
| --- | --- | --- |
| `iod_nav` | `uint16` | Navigation data issue |
| `bgd_e5a_e1_s` | `TimeDelta?` | E5a/E1 broadcast group delay |
| `bgd_e5b_e1_s` | `TimeDelta?` | E5b/E1 broadcast group delay |
| `sisa_index` | `uint8?` | Directly supplied native SISA index |
| `sisa_m` | `float64?` | Available SISA meter value |
| `sisa_status` | enum | `available`, `napa`, or `unknown` |
| `signal_health` | struct | Signal-specific health and data-validity declarations |
| `data_sources` | list of enum | Known contributing navigation sources: `E1B`, `E5aI`, `E5bI`; empty if unknown |

`data_sources` describes navigation content origin, not observation tracking
codes or proof that this receiver directly tracked every listed signal. I/NAV
can combine E1-B and E5b-I contributions; F/NAV contributions must not be merged
into that record. Exact protocol/version mappings and reserved code ranges
must be validated when defining adapters.

### Health and BGD applicability

`signal_health` has nullable `e1b`, `e5ai`, and `e5bi` structs. Each contains
`health_code: uint8?` (the two-bit source health status, 0-3) and
`data_validity_status: bool?` (native DVS polarity: true means invalid).
Unknown is not false. Do not collapse these into a universal healthy boolean.

I/NAV supplies E1-B/E5b-I health and both BGD parameters; F/NAV supplies E5a-I
health and E5a/E1 BGD. Respect source applicability before interpreting numerical
values: RINEX zeros in fields declared inapplicable become null, not healthy
status or measured zero delay. Do not fill missing fields from a different
navigation record during import.

### Accuracy, completeness, and identity

`available` requires a finite nonnegative `sisa_m`; `napa` and `unknown` require
null `sisa_m`. Use `napa` only with explicit source evidence. RINEX's -1 sentinel
conflates NAPA and unknown, so it alone maps to `unknown`, not proven NAPA.
Preserve a supplied index; do not arbitrarily infer one from a meter value.
SISA retains its own accuracy semantics, not Observation standard deviation.

Satellite identity, coherent IODnav, complete orbit/clock coefficients and
TOE/TOC are required. Optional missing health, BGD, and accuracy information
does not justify invented defaults. As for GPS, incomplete assembly remains
RawBits; complete unhealthy records remain storable, with solver acceptance left
downstream. IODnav is not a unique record ID and is not a merge key across
I/NAV and F/NAV.

## Remaining definition work

- [ ] Enumerate the initial supported navigation families and source revisions.
- [x] Define the GPS LNAV EPH parameter inventory, types, nullability, and partition time.
- [x] Define native GNSS reference time plus explicit GPST conversion and nominal GPST partitioning.
- [x] Define the Galileo I/NAV and F/NAV EPH parameter inventory and signal applicability.
- [ ] Finalize the native time-scale origin registry and broadcast conversion equations/references.
- [x] Select additional model parameter structures and partition-time rules in navigation-models.md.
- [x] Select the conditional timed-ION policy without an untimed collection branch.
- [ ] Define model-specific validity interpretation; do not confuse fit length with receipt time.
- [ ] Map source time scales and issue/health semantics per family.
- [ ] Finalize collection metadata and direct acquisition-context encoding without required row references.
- [ ] Enumerate the physical typed payload branches and source mappings.
- [ ] Implement DecodedNav normalization and persistence; selected structures
  and research validation do not imply implemented decoders.

Current scope excludes GLONASS and NavIC. Recognizing an identifier does not imply a
working decoder. Report unsupported families without claiming navigation
coverage. This document establishes the boundary, not a finished field catalog.
