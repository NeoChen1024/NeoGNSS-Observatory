# CommonNEX import and processing policy

Status: selected v0 contracts; the [UBX/SBF importer](importer.md) implements a
documented subset. Unimplemented reconciliation and other adapters remain explicit.

[Overview](overview.md)

These are importer/consumer contracts, not additional Core record families.
Core compliance does not require a transaction framework, QA manifest or a
particular deduplication algorithm. Do not expose unresolved alternatives as
independent normal observations.

RecordingSource metadata identifies each input source and its format (ubx, sbf,
rtcm3 or rinex), relevant version and explicit logical station association.
Multiple recording paths may contribute to one station without changing Setup.
Equal timestamps or coordinates alone do not establish the same acquisition.

One Recording Source may route to multiple stations when it carries multiple
antenna inputs. Conversely, multiple recording paths may feed one station.
Select the native antenna input for each single-antenna station import;
keep source/antenna decoder state separate before reconciling logical records.

Report family coverage independently: source not providing a family, configured
omission, no occurrences in the selected range, and unsupported importer mapping
are different situations. Do not infer actual coverage from Setup configuration
alone. Keep this a concise capability/coverage report, not a provenance framework.
Absence of Observation is valid for RawBits-only import and is not a QA failure.

## One-pass extraction contract

Observation requires valid measurement GPST; omit and count observations with
unusable time. Complete RawBits and receiver telemetry remain available with
null GPST and any available receiver uptime, without time-waiting buffering or
automatic repair. Follow the [receiver-time policy](telemetry-time.md) for
freshness, restart boundaries and archive placement. No timestamp is fabricated
from the directory date, and missing time does not alter canonical bits.

This rule distinguishes measurement records from receiver-associated records. RINEX special
events whose epoch is not meaningful may legitimately omit time; preserve their
event semantics and source ordering in the event/source mapping. They are not
unknown-time observations. Cross-source occurrence ambiguity between already
timed records remains a separate reconciliation concern.

Follow the [conditional RINEX preservation goal](overview.md#rinex-interoperability-and-strings).
Report unsupported RINEX content and mapping limitations; a partial import must
not be labeled lossless. Preserve source metadata and interpretation alongside
normalized fields without requiring a lossless reverse RINEX export.

Decode each source stream once and route all supported records to observation,
raw-bit, telemetry, metadata, and event batches. Share time/epoch
association and maintain state across file/day boundaries. Consumers may read
only the families they need; recording telemetry must not require enabling a
PPP, SBAS, or clock-analysis algorithm.

Bounded buffering may be needed to assemble companion blocks or protocol
fragments. Complete RawBits/telemetry does not wait for a later time anchor;
unresolved timing is immediately explicit.
Telemetry is not automatically deduplicated either; equal numerical readings
at different occurrences or from different sensors remain distinct. A navigation EOE does not finalize an unrelated future pulse record.

"One pass" means a single sequential raw decode for all supported extraction
families, not a guarantee that unknown/proprietary fields or information never
logged can be recovered. Report extraction coverage and unsupported relevant
messages. Adding a new quantity requires a typed semantic mapping, not a generic
packet dump. Existing archives remain available for future unsupported analyses;
overlap reconciliation may still read affected existing ParquetNEX partitions.

## Incremental and live processing

Routine daily ingestion handles new observations against an initialized Setup.
Retroactive repair is an exceptional, explicit rebuild of affected GPST days,
not a requirement for a generic automatic repair/resume system. Preserve older
daily revisions as described in [ParquetNEX](parquetnex.md#daily-revisions).
Multi-day repair creates a new revision for each affected day, not a new Setup
or a dataset-wide version. Each affected catalog is a complete replacement;
unaffected catalogs can retain their current revision.

### Initial batch importer scope

The first implementation does not perform automatic deduplication, overlap
merging, or idempotent repeated import. The caller selects the recording inputs;
do not claim that repeated imports are scientific no-ops. Multiple recording
paths can describe the same logical station without implying that the importer
can automatically fuse them. Restitch/QA is not an enforced prerequisite.
The importer head-probes and stable-sorts selected files, then rejects backwards
observation and receiver navigation time during the normal read. RawBits uses
the last usable receiver navigation epoch, otherwise null without waiting
for a future anchor; source SIS timestamps are not retained
or used as a fallback. Canonical payloads remain unchanged. This does not
add deduplication, tail probing or automatic overlap repair; see
[ordering and reversal checks](importer.md#head-only-ordering-and-time-reversal).

Keep framing and association separate for independent recording sources. Do not
splice unrelated packet fragments because timestamps appear adjacent. Continuous
files from the same recording path may retain decoder state across boundaries.

If overlap reconciliation is added later, the agreed preference is first-imported
valid information wins; later input supplements missing records/nullable values,
not existing values. Actual conflicts must be reported, never averaged or silently
overwritten. This is a future policy, not functionality promised by the initial
importer. Editing published rows or inserting records inside published coverage
requires a replacement revision rather than a deferred-tail part.

RawBits has no payload-based deduplication: equal payloads can be distinct
broadcast occurrences. Equal navigation-epoch timestamps likewise do not prove
identical occurrences. Consumers must not assume overlapping logging inputs
have been reduced to unique measurements.

### Epoch interval classification

For the required positive nominal observation period P (`epoch_period_s`), compare
successive distinct measurement-epoch GPST timestamps within the same station.
Use exact decimal timestamp differences and P in seconds without rounding
observations. The standard per-epoch tolerance is +/-20%:

| Interval dt | Classification |
| --- | --- |
| dt < 0 | Time reversal |
| dt = 0 | Repeated timestamp requiring explicit identity/time interpretation |
| 0 < dt < 0.8 P | Too-short interval: timing/cadence anomaly, not a gap |
| 0.8 P <= dt <= 1.2 P | Within the nominal cadence tolerance |
| dt > 1.2 P | Observation cadence gap |

The endpoints are inclusive: for P = 1000 ms, 800 through 1200 ms is acceptable.
"20% below the period" means below 80% of P, not below 20% of P. A gap is a
coverage finding, not proof of receiver failure or an exact missing-epoch count.
Too-short intervals may reflect a wrong declared period or timestamp problems;
do not assert a specific cause from this check alone. Setup requires an explicit
positive nominal period; missing or unknown cadence is not silently defaulted.

Apply this to logical observation epochs, not per-satellite rows, companion
blocks, RawBits or telemetry arrivals. Reconcile proven logging duplicates before
classifying the merged station; conflict alternatives are not extra normal
epochs. Preserve evidence of time reversals rather than hiding them by sorting.
Physical file, batch and GPST-day boundaries do not restart the comparison.
Explicit new continuity contexts are handled separately.

A deliberately decimated Recording Source can have a different declared output
cadence. Source-local checks use that declared cadence; merged-stream coverage
against Setup's P may still show gaps. Label the scope and period used instead
of silently changing the Setup or blaming the receiver for decimation.

Retain observations, timestamps and anomaly findings. Positive out-of-range intervals
are not a reason to reject the entire import, snap epochs, fabricate samples or
automatically reset processing state. This shared rule does not mandate another
full QA pass in every consumer; solver reset/timeout policies remain separate.
Live absence beyond 1.2 P can be provisional; final interval classification uses
observation time, not network arrival latency.
The current ordered-input importer rejects time reversals as stated above;
cadence-gap/too-short-interval Events remain unimplemented.

### Completion, late data, and state

Persist cross-epoch findings in the [continuity Events family](events.md),
separately from per-observable quality and epoch completeness. Recompute affected
events when repairing daily revisions. Consumers use these events without
repeating full importer QA, but retain algorithm-specific state requirements.

Proposed logical completion events distinguish `epoch_complete`,
`stream_end`, and receiver/continuity events. Completion refers to records
emitted for an epoch, not a claim that all expected signals were observed.
Batch completion is a transport concern and carries no scientific meaning.

Source-local completion and merged-stream completion are different. Closing one
logger or receiving its EOE does not close other sources' contributions. The
merger uses a declared source set/completion policy or live buffering window;
no generic network synchronization protocol is prescribed.

An adapter emits completion for its particular epoch context. A complete,
length/checksum-valid UBX RAWX completes its measurement record without waiting
for NAV-EOE. EOE closes navigation messages only. SBF Measurements requires the
matching EndOfMeas. Missing satellites, nullable measurements or optional telemetry
do not alone mean an incomplete epoch. Only actually unfinished frames/groups
are withheld at EOF; normal message-boundary rollover does not defer RAWX.

A per-day `import-state.json` sidecar may retain the input position already
published and the raw context restart position for a withheld tail. This is
importer bookkeeping, not scientific metadata or a serialized native decoder.
Update it only after corresponding parts are published. Downstream processing
does not depend on it. Complete crash recovery is not promised.

For the initial batch importer, withhold incomplete tail epochs from publication
and report them. Once the next local recording is supplied, reread the necessary
preceding raw context and publish newly completed records as the next part of
the same revision, on their GPST day. Do not re-emit the published prefix during
context replay. Prior Parquet alone cannot restore an unpublished fragment.
If the tail still cannot be completed, omit and report it. Do not fetch receiver
files through FTP, wait for acquisition, or require a generic checkpoint system.
See [daily revisions](parquetnex.md#daily-revisions) for naming and publication.

Repeated timestamps do not authorize overwriting. Exactly-once delivery and
automatic recognition of repeated input are not guaranteed by the schema or
initial importer; optional file-local counters are not deduplication evidence.

Ordering and late-data policy belong to the producer/consumer contract. A live
consumer may require ordered completed epochs, buffer a declared late window,
or decline immediate processing of late records. Recoverable late data can be handled through an explicit rebuild for
persistence/replay. V0 does not require
watermarks, record retractions, or an update protocol for already emitted
epochs. Proposed default: an epoch completed for a consumer is not silently
amended; later information is reported for explicit replay or reprocessing.

Report emitted records, unresolved tails/associations and affected GPST ranges.
Report detected conflicts or duplicates without claiming exhaustive detection. A newly added reset/event or navigation record may
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
