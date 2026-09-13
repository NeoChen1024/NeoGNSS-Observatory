# CommonNEX and ParquetNEX v0

Status: design specification under review; not an implemented format or API.
This document set does not change existing schemas, CLI behavior, or datasets.

## Scope and document map

CommonNEX is a source-independent logical representation of RINEX-like
observations and received raw navigation content. It defines identities, types, units, nullability, quality, and
relationships, not a mandatory memory layout, language ABI, or wire protocol.

| Specification | Responsibility |
| --- | --- |
| [Core](core.md) | Shared types/context, observation epochs and wide observation records |
| [Events](events.md) | Shared continuity event semantics and daily storage |
| [Setup JSON](setup-json.md) | Station metadata, named antennas, configuration references and initialization schema decisions |
| [RawNav](raw-nav.md) | First-class core record family for received navigation occurrences; presence is capability-dependent |
| [DecodedNav](decoded-nav.md) | Optional standardized decoded navigation parameters |
| [Auxiliary](auxiliary.md) | Typed receiver clock, pulse, environment, status and solution records |
| [Receiver profiles](receiver-profiles.md) | Input message requirements and adapter mapping contracts |
| [Import policy](import-policy.md) | Reconciliation, completion, late data and one-pass extraction |
| [ParquetNEX](parquetnex.md) | Optional Parquet persistence and replay mapping |

Core model membership and mandatory data presence are different. Observation
and RawNav are first-class core record families sharing Setup, Stream, epoch
and continuity semantics; neither must accompany the other. DecodedNav remains
a standardized optional extension, and auxiliary families remain optional.
Each supplied family obeys its schema; consumers need only implement the
families they use, not every navigation-content decoder.

| Source capability | Legal record set |
| --- | --- |
| Observation-only | ObservationEpoch and Observation, with shared context |
| Observation + RawNav | Both families with their respective epoch associations |
| RawNav-only | RawNav and applicable NavigationEpoch/context, without ObservationEpoch or Observation |

RawNav-only is a valid CommonNEX dataset, not an incomplete observation dataset.
Do not synthesize empty observations, require RAWX, or reject it because PPP/TEC
cannot run. Processors declare their required capabilities and reject only an
unsupported requested operation. RawNav remains first-class even when stored
in separate Parquet files. Decoded ephemerides do not reconstruct received bits.

Observation and RawNav require usable GPST epoch association. Records whose
time remains unresolved after bounded importer buffering are skipped and counted;
raw archives allow later reconstruction. No unknown-time Observation/RawNav
storage branch is required. RawNav-only remains legal with valid NavigationEpoch
context. Legitimate untimed RINEX special events retain their separate semantics.

V0 excludes GLONASS observations/navigation and processing. Mixed inputs must
be framed correctly and exclusions reported. Other constellation identifiers
do not imply complete adapter or scientific-algorithm support.
Raw archives remain the preservation masters, not CommonNEX or ParquetNEX.

## RINEX interoperability and strings

The format-wide design requirement is lossless RINEX import, not lossless
RINEX export. This applies to observations, navigation, events, quality,
correction semantics and metadata across core families and extensions, not only
`setup.json`. Preserve source information in its appropriate record family or
source metadata; do not force all RINEX headers into Setup.

Lossless means preserving information and scientific interpretation, not
reconstructing the original whitespace, line wrapping or byte layout. A raw
archive or opaque copy alone is not a substitute for mapping supported scientific
fields. Numeric precision, source time interpretation, missing values and
correction state must be accounted for explicitly during normalization.

CommonNEX strings allow Unicode and special characters without RINEX fixed-width
or ASCII restrictions. Do not silently truncate, transliterate, normalize Unicode
or case-fold stored text. Standardized codes retain their prescribed spelling
and semantics; reference equality and field-specific validation still apply.
ParquetNEX and other bindings must preserve this content rather than narrowing
it to RINEX's representation limits.

CommonNEX may contain information RINEX cannot express. An exporter may reject
such an export or use an explicitly chosen lossy mapping, reporting what cannot
be represented. Silent information loss is not an acceptable export policy.

This is a format requirement, not a claim that the current draft/importer covers
all RINEX versions and records. In particular, the existing v0 GLONASS exclusion
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
Processors select their required families: observations for PPP/TEC, RawNav
for SBAS decoding, auxiliary records for receiver-clock analysis. External
orbit/bias products are processing inputs, not mandatory Core records.

| Mode | Processing contract |
| --- | --- |
| Archive batch | Read an archive range as bounded batches; no whole-archive in-memory requirement |
| Incremental daily | Add/reconcile new records; preserve or replay required earlier processing state |
| Live | Produce records and epoch completion incrementally, without knowing the final stream length |

Neither a day nor a Setup is a mandatory computational unit. The format must
not require a complete day, total record count, known stream end, prior QA,
a reconstruction index, or a Parquet round trip before processing.
File, batch and GPST-day boundaries do not reset tracking, clocks, RawNav
assembly or SBAS aging. Daily storage does not imply a daily solution.

[Continuity events](events.md) describe detected discontinuities using shared
Stream and epoch identities. Their interpretation does not depend on a
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

Review in this order:

- [x] Define Setup, Observation Stream and Recording Source boundaries.
- [x] Select initialization-only Setup JSON/configuration and complete daily revisions.
- [x] Select native Arrow batches with Python/PyArrow Parquet I/O.
- [ ] Finalize Setup JSON fields, stream declarations and signal capability semantics.
- [ ] Finalize Core quality records, correction semantics and identity keys.
- [ ] Specify adapter epoch association/completion, including SBF Measurements and RTCM3.
- [ ] Validate canonical RawNav family layouts and source mappings.
- [ ] Complete auxiliary and DecodedNav field catalogs as consumers require.
- [ ] Finalize ParquetNEX metadata, nested field schemas and publication layout.
- [ ] Implement nanoarrow-backed columnar export/import and buffer ownership in the bindings.
- [ ] Implement bounded import/replay and compare representative direct/replay inputs.

Implementation priority is UBX/SBF import and incremental daily ParquetNEX
storage/replay before the downstream PPP/IPP engine integration. The initializer
imports station metadata/configuration separately from routine daily recordings.
Retroactive repair creates new revisions only for affected days; it is not the
normal ingestion path. See [ParquetNEX](parquetnex.md) for initialization and
reader selection, and [Core](core.md) for source-independent Stream identity.

Validation should cover cross-file/day state, repeated imports, complementary
coverage and conflicts using small representative inputs. This draft does not
authorize a full archive conversion or new permanent automated tests.

Project-owned specifications are GPL-3.0-only; see [LICENSE](../../LICENSE).
External standards retain their own notices.
