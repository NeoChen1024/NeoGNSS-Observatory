# CommonNEX import and processing policy

Status: v0 design draft; not an implemented format or API.

[Overview](overview.md)

These are importer/consumer contracts, not additional Core record families.
Core compliance does not require a transaction framework, QA manifest or a
particular deduplication algorithm. Do not expose unresolved alternatives as
independent normal observations.

RecordingSource metadata identifies each input source and its format (ubx, sbf,
rtcm3 or rinex), relevant version and explicit logical stream association.
Multiple recording paths may contribute to one Stream without changing Setup.
Equal timestamps or coordinates alone do not establish the same acquisition.

## One-pass extraction contract

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

## Incremental and live processing

Routine daily ingestion handles new observations against an initialized Setup.
Retroactive repair is an exceptional, explicit rebuild of affected GPST days,
not a requirement for a generic automatic repair/resume system. Preserve older
daily revisions as described in [ParquetNEX](parquetnex.md#daily-revisions).
Multi-day repair creates a new revision for each affected day, not a new Setup
or a dataset-wide version. Reconciliation rules below also apply during repair.

### Permissive overlap reconciliation

Permissive import is an agreed requirement. The default accepts overlapping
time ranges from multiple logging sources of the same logical acquisition,
including repeated imports and late additions to an existing day. It must not
require a separate restitch/QA run or reject an entire input because its bounds
overlap earlier data. [Core normalization](core.md), framing, scientific validity,
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
canonical family/observable and applicable corrections. Nanosecond
rounding can make distinct source timestamps equal. Do not snap nearby epochs
together or apply an implicit numeric tolerance to observation values.

For navigation bits, identical content may be broadcast repeatedly. Payload
equality alone does not identify the same received occurrence. Native source
timestamps can denote different events and are not directly equal occurrence
keys. RawNav exposes only NavigationEpoch association. A justified association
rule is required before collapsing
cross-source copies. Without it, preserve both with unresolved association.

Different source formats may expose different precision or correction states.
A RINEX-derived value is not an exact duplicate of a raw-derived value solely
because they are numerically close. Any future equivalence/preference rule
must account for those transformations explicitly. Contradictory flags remain
source-attributed evidence rather than being combined into apparent certainty.

Proposed minimal logical relations for reconciliation:

- `RecordSource`: canonical record reference and `recording_source_id`.
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

### Completion, late data, and state

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
