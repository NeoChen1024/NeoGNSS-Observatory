# CommonNEX events and scoped context

Status: selected v0 design; physical payload encodings and adapters remain
pending. [Overview](overview.md) | [Import policy](import-policy.md)

## Purpose

Events describe boundaries, discontinuities and source-declared observation
clock context. Observation and RawBits carry their own times; Events are not
epoch lookup tables. No science row must reference an event ID. Setup/Stream
metadata references remain shared descriptive context.

Measurement and navigation scopes are independent even at equal timestamps.
Protocol completion is not discontinuity, proof of all signals being present,
or an instruction to reset a solver. Signal-local slip/lock/quality indicators
remain on Observation rows.

## Event kinds

| Kind | Meaning |
| --- | --- |
| `OBSERVATION_GAP` | Adjacent measurement interval exceeds 1.2 times nominal period |
| `EPOCH_INTERVAL_SHORT` | Positive measurement interval below 0.8 times nominal period |
| `TIME_REVERSAL` | Time decreases in established acquisition order |
| `REPEATED_TIMESTAMP` | Distinct epochs have equal time; not duplicate proof |
| `RECEIVER_RESTART` | Supported restart evidence, not mere logger reconnection |
| `EPOCH_COMPLETION` | Complete/incomplete/unknown closure of an observation or navigation epoch, including epochs with no retained science rows |
| `CLOCK_CORRECTION_STATE` | Source declarations about corrections already applied to observation time, code and phase |
| `OBSERVATION_CLOCK_OFFSET` | Source-reported offset associated with a measurement epoch; not NAV-CLOCK or PVT telemetry |

## Shared fields and applicability

| Field | Type | Meaning |
| --- | --- | --- |
| `stream_id` | string | Affected logical Stream |
| `kind` | enum | Kind above |
| `scope` | enum | `STREAM`, `OBSERVATION` or `NAVIGATION` |
| `gpst` | GpstTimestamp | Event location, target epoch time, or state/interval start |
| `applicability` | enum | `POINT`, `EPOCH`, `INTERVAL` or `STATE` |
| `end_gpst` | GpstTimestamp? | Exclusive interval/state end, if explicitly supplied |
| `evidence` | enum | Reported versus inferred basis; exact adapter vocabulary pending |
| `payload` | typed union | Kind-specific fields below, not arbitrary JSON |

`GpstTimestamp`, `TimeDelta` and `Duration` use `DECIMAL(38,12)` seconds.
No event ID or epoch foreign key is required. Multiple events may share time.
`EPOCH` applies only to its declared epoch context, never implicitly forward.
`INTERVAL` is `[gpst,end_gpst)` with a required end later than the start.
`STATE` starts inclusively and lasts until explicit end or a superseding
declaration in the same scope. A date, file boundary or absence of a new event
does not end it. A restart does not silently establish a new correction value.
`POINT` records an occurrence, not an enduring state.

Equal timestamps cannot distinguish conflicting/repeated acquisition contexts.
Do not resolve ambiguous applicable events by file order or last-write-wins.
Expose ambiguity and require explicit source selection or exclusion; exact
source/conflict selectors are a remaining schema task, not permission to guess.

## Kind-specific payloads

Interval/discontinuity events contain `previous_gpst: GpstTimestamp?`,
`next_gpst: GpstTimestamp?`, `interval_s: TimeDelta?`, and
`expected_period_s: Duration?`. These are direct coordinates, not references.
For cadence/reversal events, `gpst=next_gpst`; previous/next describe acquisition
order even when time reverses. The interval is evidence, not an assertion of
exact missing sample times/count. Restart time is the first justified evidence,
not an invented exact reboot instant. RawBits-only events never inherit
measurement-cadence thresholds for asynchronous navigation messages.

`EPOCH_COMPLETION` uses `applicability=EPOCH`, with `completion` equal to
`COMPLETE`, `INCOMPLETE` or `UNKNOWN`, and `completion_basis` equal to
`PROTOCOL_BOUNDARY`, `RECORD_STRUCTURE`, `INCOMPLETE_TAIL` or `UNKNOWN`.
Its scope identifies observation versus navigation; closure does not tie their
timestamps together. A source-local boundary alone is not merged completion.
Source RINEX `rinex_epoch_flag: uint8?` belongs to the corresponding event;
special events retain their own typed mapping, not fabricated observations.

`CLOCK_CORRECTION_STATE` uses OBSERVATION scope and explicit STATE/INTERVAL
applicability. Its payload has `epoch_time`, `code` and `phase`, each
`APPLIED`, `NOT_APPLIED` or `UNKNOWN`. These are source declarations, not
claims of zero residual clock error. Missing applicable declarations yield
UNKNOWN, never an implicit NOT_APPLIED. Conflicting declarations remain conflicts.

`OBSERVATION_CLOCK_OFFSET` uses OBSERVATION/EPOCH and
`reported_observation_clock_offset_s: TimeDelta`. Never hold this value forward
or interpolate it during import. A source can report one every epoch, so Events
are not guaranteed to be small. Missing offsets remain unavailable.

Neither event instructs importers to apply/undo corrections. Preserve source
observation values and GPST scale normalization independently. Do not derive
these events from navigation clock bias/drift telemetry. Exact source sign and
application-state mappings require adapter validation.

## Reading and persistence

Consumers needing clock interpretation load the relevant Events context before
using observations. Read earlier partitions as necessary to obtain a still-valid
state: yesterday alone is not a guaranteed bound. A reader may scan the compact
event history, but the format does not require loading every event into memory.
Direct/live adapters provide the same required state or explicitly UNKNOWN;
absence of context must not be mistaken for an uncorrected source.

ParquetNEX stores `r00-events-part00.parquet` and subsequent parts/revisions per
Stream/GPST day, following [ParquetNEX naming](parquetnex.md#daily-revisions).
Assign by `gpst`:
an interval comparison belongs to the next available epoch's day even if its
previous coordinate is from another day; persistent state begins in its start
day and need not be copied every midnight. Select the latest Events revision
and all its parts for each required day, including earlier context partitions.
Read only after all related science/Events writes finish; revision numbers need
not match between catalogs. Tail completion may add a part without revising
published events. Rebuild only day/catalog scopes whose existing content changes. Counter renumbering
does not force later revisions; changed boundary/state interpretation may.

The initial importer does not automatically reconcile overlapping sources;
do not claim unique merged-stream continuity for such inputs. Preserve real reversal evidence; source arrival order
is not necessarily receiver acquisition order. File rollover and handover are
not discontinuities. No events file is necessary when no events/context exist.
Capability/coverage metadata distinguishes unavailable detection from no detected
event; neither is a universal continuity guarantee or prerequisite QA stamp.
Legitimate untimed RINEX special events remain separately mapped, not assigned
fictional GPST to fit this timed schema.

## Progress

- [x] Remove required epoch tables and row-to-row time references.
- [x] Separate observation/navigation time scopes and completion from discontinuity.
- [x] Move clock correction declarations and epoch-local offsets into Events.
- [x] Define interval/state applicability and cross-day context lookup semantics.
- [ ] Finalize typed payload serialization, evidence vocabulary, and selectors
  for conflicting acquisition contexts at equal times.
- [ ] Validate source-specific clock and completion mappings; define retained
  RINEX special/header-change events.
- [ ] Implement Events import, persistence and context-aware replay.
