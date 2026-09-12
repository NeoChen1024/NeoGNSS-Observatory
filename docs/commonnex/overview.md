# CommonNEX and ParquetNEX v0

Status: design specification under review; not an implemented format or API.
This document set does not change existing schemas, CLI behavior, or datasets.

## Scope and document map

CommonNEX is a source-independent logical representation of RINEX-like
observations. It defines identities, types, units, nullability, quality, and
relationships, not a mandatory memory layout, language ABI, or wire protocol.

| Specification | Responsibility |
| --- | --- |
| [Core](core.md) | Shared types/context, observation epochs and wide observation records |
| [RawNav](raw-nav.md) | Optional standardized received navigation occurrences |
| [DecodedNav](decoded-nav.md) | Optional standardized decoded navigation parameters |
| [Auxiliary](auxiliary.md) | Typed receiver clock, pulse, environment, status and solution records |
| [Receiver profiles](receiver-profiles.md) | Input message requirements and adapter mapping contracts |
| [Import policy](import-policy.md) | Reconciliation, completion, late data and one-pass extraction |
| [ParquetNEX](parquetnex.md) | Optional Parquet persistence and replay mapping |

Core observation compliance does not require RawNav, DecodedNav or auxiliary
data. An optional family must obey its declared schema when supplied. A
RawNav-only dataset may use shared Setup/Stream/NavigationEpoch definitions
without inventing observations; it does not claim observation coverage.
Decoded ephemerides are not evidence that raw navigation occurrences survive.

V0 excludes GLONASS observations/navigation and processing. Mixed inputs must
be framed correctly and exclusions reported. Other constellation identifiers
do not imply complete adapter or scientific-algorithm support.
Raw archives remain the preservation masters, not CommonNEX or ParquetNEX.

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
- [ ] Specify adapter epoch association/completion, including Meas3 and RTCM3.
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
