# CommonNEX and ParquetNEX v0

Status: design specification under review; not an implemented format or API.
This document set does not change existing schemas, CLI behavior, or datasets.

## Scope and document map

CommonNEX is a source-independent logical representation of RINEX-like
observations and received raw navigation content. It defines identities, types, units, nullability, quality, and
relationships, not a mandatory memory layout, language ABI, or wire protocol.

Canonical timestamps use `DECIMAL(38,12)` seconds since the GPST origin, with
1 ps representation resolution. A value of one means one second, not one
picosecond tick. See [Core time representation](core.md#time-representation)
for range, rounding and nullability, and [ParquetNEX](parquetnex.md) for storage.

| Specification | Responsibility |
| --- | --- |
| [Core](core.md) | Shared types/context and directly timed wide observation records |
| [Events](events.md) | Completion, continuity and daily storage |
| [Setup JSON](setup-json.md) | Single-station receiver/antenna metadata, config and ANTEX companions, initialization |
| [RawBits](raw-bits.md) | First-class core record family for received navigation occurrences; presence is capability-dependent |
| [RawBits layouts](raw-bits-layouts.md) | Canonical bit layouts, verified receiver mappings, check scopes and pending validation |
| [RawBits registry](raw-bits-registry.md) | Primary-source signal vocabulary, legal family/format pairs and reserved extensions |
| [DecodedNav](decoded-nav.md) | Optional standardized decoded navigation parameters |
| [Additional navigation models](navigation-models.md) | Additional EPH, STO, EOP and ION structures and mapping constraints |
| [Auxiliary](auxiliary.md) | Typed receiver clock, pulse, environment, status and solution records |
| [Receiver profiles](receiver-profiles.md) | Input message requirements and adapter mapping contracts |
| [RINEX mapping](rinex-mapping.md) | RINEX-only observation fields and source metadata |
| [Import policy](import-policy.md) | Reconciliation, completion, late data and one-pass extraction |
| [ParquetNEX](parquetnex.md) | Optional Parquet persistence and replay mapping |

Core model membership and mandatory data presence are different. Observation
and RawBits are first-class core record families sharing Setup identity, epoch
and continuity semantics; neither must accompany the other. DecodedNav remains
a standardized optional extension, and auxiliary families remain optional.
Each supplied family obeys its schema; consumers need only implement the
families they use, not every navigation-content decoder.

| Source capability | Legal record set |
| --- | --- |
| Observation-only | Directly timed Observation and applicable Events/context |
| Observation + RawBits | Both families with their respective epoch associations |
| RawBits-only | Directly timed RawBits and applicable Events/context, without Observation |

RawBits-only is a valid CommonNEX dataset, not an incomplete observation dataset.
Do not synthesize empty observations, require RAWX, or reject it because PPP/TEC
cannot run. Processors declare their required capabilities and reject only an
unsupported requested operation. RawBits remains first-class even when stored
in separate Parquet files. Decoded ephemerides do not reconstruct received bits.

Observation and RawBits require usable GPST epoch association. Records whose
time remains unresolved after bounded importer buffering are skipped and counted;
raw archives allow later reconstruction. No unknown-time Observation/RawBits
storage branch is required. RawBits-only remains legal with valid navigation
time stored in `nav_epoch_gpst`. Legitimate untimed RINEX special events retain
their separate semantics. No independent epoch tables or mandatory row-to-row
references exist. Timestamps are not unique keys. Setup metadata remains
shared; continuity-sensitive consumers load applicable Events, including
earlier still-effective state, rather than joining per-row event IDs.

The project excludes GLONASS and NavIC observations/navigation and processing.
These are intentional scope exclusions, not a future implementation backlog. Mixed inputs must
be framed correctly and exclusions reported. Other constellation identifiers
do not imply complete adapter or scientific-algorithm support.
Raw archives remain the preservation masters, not CommonNEX or ParquetNEX.

## RINEX interoperability and strings

The format-wide goal is lossless preservation of reliably mappable RINEX
scientific information, subject to declared scope and mapping limitations,
not unconditional field-by-field or byte-reversible import or lossless
RINEX export. This applies to observations, navigation, events, quality,
correction semantics and metadata across core families and extensions, not only
`setup.json`. Preserve source information in its appropriate record family or
source metadata; do not force all RINEX headers into Setup.

Lossless means preserving information and scientific interpretation, not
reconstructing the original whitespace, line wrapping or byte layout. A raw
archive or opaque copy alone is not a substitute for mapping supported scientific
fields. Numeric precision, source time interpretation, missing values and
supported correction semantics must be accounted for explicitly during normalization.
Inputs declaring applied observation clock-offset correction are excluded, not
silently normalized or undone. Measurement-clock adjustment evidence remains
in Auxiliary; internal clock steering is not this excluded correction workflow.

CommonNEX strings allow Unicode and special characters without RINEX fixed-width
or ASCII restrictions. Do not silently truncate, transliterate, normalize Unicode
or case-fold stored text. Standardized codes retain their prescribed spelling
and semantics; reference equality and field-specific validation still apply.
ParquetNEX and other bindings must preserve this content rather than narrowing
it to RINEX's representation limits.

CommonNEX may contain information RINEX cannot express. An exporter may reject
such an export or use an explicitly chosen lossy mapping, reporting what cannot
be represented. Silent information loss is not an acceptable export policy.

Content that cannot be reliably mapped, lacks required timing, or is outside
scope may be discarded with diagnostics and counts. Preserve usable records;
do not guess times, fabricate parameters, or replace unknowns with zeros.
Raw archives permit later reconstruction but do not replace mapping supported
fields. ION coefficients with neither transmission time nor reliable acquisition
epoch are one explicit exclusion; no untimed-model storage branch is required.

This is a design goal, not a claim that the current draft/importer covers
all RINEX versions and records. Only standard RINEX 3.x/4.x is targeted; RINEX 2
is excluded. In particular, the GLONASS/NavIC exclusions
and fixed-station scope remain limitations, not exceptions that can be called
fully lossless import. Report unsupported content explicitly; only a verified
complete mapping for the accepted input can be described as lossless.

## Processing pipeline

```text
SBF / UBX / RTCM3 / RINEX
           |
      Input adapters
           |
       CommonNEX --------------------> Processing facilities
           |                                    ^
      ParquetNEX writer                         |
           |                                    |
       ParquetNEX --> reader --> CommonNEX ------+
```

Writing ParquetNEX is optional. Direct input and replay expose the same logical
records, identities, quality and scientific interpretation to consumers.
Processors select their required families: observations for PPP/TEC, RawBits
for SBAS decoding, auxiliary records for receiver-clock analysis. External
orbit/bias products are processing inputs, not mandatory Core records.

| Mode | Processing contract |
| --- | --- |
| Archive batch | Read an archive range as bounded batches; no whole-archive in-memory requirement |
| Incremental daily | Add new days and completed tail parts; preserve or replay required earlier processing state |
| Live | Produce records and epoch completion incrementally, without knowing the final stream length |

Neither a day nor a Setup is a mandatory computational unit. The format must
not require a complete day, total record count, known stream end, prior QA,
a reconstruction index, or a Parquet round trip before processing.
File, batch and GPST-day boundaries do not reset tracking, clocks, RawBits
assembly or SBAS aging. Daily storage does not imply a daily solution.

[Events](events.md) describe completion and discontinuities
using shared station identity and explicit time applicability. Their interpretation does not depend on a
particular processing execution model.

Ordering, bounded buffering, backpressure, checkpoints, scheduling and replay
ranges belong to processing facilities. CommonNEX does not guarantee that an
isolated daily partition initializes every solver, or define a generic
checkpoint/transport framework. Completion and actual continuity evidence
remain interpretable without encoding a particular execution model.

## Implementation boundaries and review order

Preserve [native architecture](../native-architecture.md): reusable protocol
decoding in libcppgnss, Observatory association/state and high-level bindings
in libneognss-obs, orchestration and Parquet in Python. Use native batches and
release the GIL; no per-frame Python callbacks or worker-executable backend.

The Observatory implementation selects Arrow-compatible columnar batches for
native/Python exchange, using nanoarrow and the Arrow C Data/PyCapsule interface.
Python/PyArrow owns Parquet I/O. This implementation choice does not make Arrow
a CommonNEX compliance requirement, mandate persistence, or restrict batch,
incremental or live use. See the [interop design](../native-architecture.md#selected-commonnex-interop-design)
for ownership, bidirectional replay and bounded-memory requirements.

### Agreed design

Checked items mean a design decision or stated research validation, not shipped code.

- [x] Define single logical station Setup and Recording Source boundaries.
- [x] Define initialization-only Setup JSON, a single antenna, static-station
  metadata, Unicode strings and decimal nominal periods (see setup-json.md).
- [x] Select independent direct Observation/RawBits timestamps, with no epoch
  tables or mandatory occurrence IDs; optional counters are file-local.
- [x] Select wide C/L/D/S rows, nullable quality/lock fields and decimal time types.
- [x] Exclude clock-corrected Observation input; preserve measurement-clock
  adjustment evidence in Auxiliary without inference or correction.
- [x] Define Events completion/continuity versus persistent context semantics,
  including earlier-day context lookup and UNKNOWN when declarations are absent.
- [x] Separate RawBits satellite identity, bitstream_source, semantic family and
  unpacking format; record validated bit layouts and check scopes.
- [x] Select DecodedNav typed model families, native/GPST reference times and
  partition-time policies; model-specific mappings are not implied complete.
- [x] Select GPST date directories and `r00-<catalog>-part00.parquet` naming;
  tail completion adds parts, while reconstruction replaces affected day/catalog
  revisions. Local counter changes do not force subsequent revisions.
- [x] Scope the initial importer to supplied local files, without automatic
  deduplication, acquisition/FTP, or reading while writing.
- [x] Select native Arrow batches and Python/PyArrow Parquet I/O.

### Remaining specification and source validation

- [x] Select uppercase broadcast-signal names and source-granularity rules;
  keep protocol revision handling in importers, not the storage contract.
- [x] Enumerate signal entries and legal family/format pairs for reviewed
  layouts against primary sources (raw-bits-registry.md).
- [ ] Resolve reserved service-family mappings and source-discriminator gaps
  listed in the registry; names alone do not establish importer support.
- [ ] Finalize adapter association/completion and source-specific quality,
  lock, phase and clock mappings (receiver-profiles.md).
- [ ] Finalize Events payload encodings, evidence and equal-time conflict scopes
  (events.md); avoid restoring a row-reference graph.
- [ ] Complete missing receiver/family validation only when supported output
  is available (raw-bits-layouts.md); do not block verified families on it.
- [ ] Complete DecodedNav model mappings and auxiliary catalogs as needed;
  see their focused checklists rather than treating selected structures as code.
- [ ] Finalize full Parquet schemas/enum encodings and metadata keys beyond the
  observation pilot; station location and revision/part naming are decided.

### Implementation

- [x] Implement initialization and RAWX/MeasEpoch observation pilot with completion Events.
- [x] Implement native-to-Python nanoarrow batches and capsule buffer ownership.
- [x] Import UBX/SBF SBAS L1 RawBits through Arrow and daily ParquetNEX, with
  independent navigation context and scoped CRC checks; feed the SBAS grid reader.
- [x] Extend RawBits to the documented GPS/Galileo/BeiDou/QZSS/SBAS containers,
  retaining unclassified services and distinguishing sample/documentary validation.
- [ ] Add native Arrow input/replay consumers and remaining catalog mappings.
- [x] Implement bounded daily observation writes and raw-tail continuation.
- [x] Implement filename validation, next revision/part allocation and latest
  catalog selection without overwriting existing parts.
- [x] Withhold incomplete measurement tails and replay preceding raw context when subsequent
  input arrives, without emitting the already published prefix again.
- [x] Validate observation pilot Arrow/Parquet round trips, tail parts and revision
  selection without adding permanent tests.
- [ ] Extend validation to remaining RawBits, quality and scoped Event mappings.

See [the implemented importer pilot](importer.md) for actual coverage and commands.

Implementation priority is UBX/SBF import and incremental daily ParquetNEX
storage/replay before the downstream PPP/IPP engine integration. The initializer
imports station metadata/configuration separately from routine daily recordings.
Retroactive repair creates new revisions only for affected days; it is not the
normal ingestion path. See [ParquetNEX](parquetnex.md) for initialization and
reader selection, and [Core](core.md) for source-independent station identity.

Validation should cover cross-file/day state, repeated imports, complementary
coverage and conflicts using small representative inputs. This draft does not
authorize a full archive conversion or new permanent automated tests.

Project-owned specifications are GPL-3.0-only; see [LICENSE](../../LICENSE).
External standards retain their own notices.
