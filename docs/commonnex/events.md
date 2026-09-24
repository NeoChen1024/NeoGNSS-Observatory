# CommonNEX events and scoped context

UBX/SBF completion and receiver restart evidence are implemented. Planned
cadence kinds and reserved applicability rules are explicitly marked below.
[Overview](overview.md) | [Import policy](import-policy.md)

## Purpose

Events describe boundaries and discontinuities. Observation and RawBits carry their own times; Events are not
epoch lookup tables. No science row must reference an event ID. Setup
metadata references remain shared descriptive context.

Measurement and navigation scopes are independent even at equal timestamps.
Protocol completion is not discontinuity, proof of all signals being present,
or an instruction to reset a solver. Signal-local slip/lock/quality indicators
remain on Observation rows.

## Implemented and planned event kinds

| Kind | Status and meaning |
| --- | --- |
| `OBSERVATION_GAP` | Planned: Adjacent measurement interval exceeds 1.2 times nominal period |
| `EPOCH_INTERVAL_SHORT` | Planned: Positive measurement interval below 0.8 times nominal period |
| `TIME_REVERSAL` | Planned: Time decreases in established acquisition order |
| `REPEATED_TIMESTAMP` | Planned: Distinct epochs have equal time; not duplicate proof |
| `RECEIVER_RESTART` | Supported restart evidence, not mere logger reconnection |
| `EPOCH_COMPLETION` | Complete/incomplete/unknown closure of an observation or navigation epoch, including epochs with no retained science rows |

Only completion and restart are emitted today. Time reversal currently fails
import instead of emitting a TIME_REVERSAL Event. Cadence kinds and their
payloads below are design targets, not implemented columns.

## Shared fields and applicability

| Field | Type | Meaning |
| --- | --- | --- |
| `setup_id` | string | Affected logical station |
| `kind` | enum | Kind above |
| `scope` | enum | `STREAM`, `OBSERVATION`, `NAVIGATION` or `RECEIVER` |
| `gpst` | GpstTimestamp? | Event location, target epoch time, or state/interval start; restart evidence may be untimed |
| `receiver_uptime_s` | Duration? | Reported uptime for receiver restart evidence; null for completion |
| `applicability` | enum | `POINT`, `EPOCH`, `INTERVAL` or `STATE` |
| `end_gpst` | GpstTimestamp? | Exclusive interval/state end, if explicitly supplied |
| `evidence` | enum | `REPORTED` for current completion; `INFERRED` for current restart |
| `payload` | struct | Nullable `epoch_completion` struct and `restart_reason` string; see below |

`GpstTimestamp`, `TimeDelta` and `Duration` use `DECIMAL(38,12)` seconds.
No event ID or epoch foreign key is required. Multiple events may share time.
Implemented completion Events require non-null GPST; receiver restart Events
permit null GPST. Other event kinds must define their time constraints before
implementation; nullable storage is not permission to omit required times.
`EPOCH` applies only to its declared epoch context, never implicitly forward.
`INTERVAL` is `[gpst,end_gpst)` with a required end later than the start.
`INTERVAL` and `STATE` are reserved applicability designs; current output uses
`EPOCH` for completion and `POINT` for restart. `STATE` starts inclusively and lasts until explicit end or a superseding
declaration in the same scope. A date, file boundary or absence of a new event
does not end it.
`POINT` records an occurrence, not an enduring state.

Equal timestamps cannot distinguish conflicting/repeated acquisition contexts.
Do not resolve ambiguous applicable events by file order or last-write-wins.
Expose ambiguity and require explicit source selection or exclusion; exact
source/conflict selectors are a remaining schema task, not permission to guess.

## Kind-specific payloads

Planned interval/discontinuity payloads contain `previous_gpst: GpstTimestamp?`,
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
Current import emits `COMPLETE` with `RECORD_STRUCTURE` (RAWX) or
`PROTOCOL_BOUNDARY` (EndOfMeas/matched NAV-EOE). Other completion values are
reserved; incomplete input is currently reported through importer state/counts,
not an emitted incomplete measurement Event.
These fields are stored in `payload.epoch_completion`; for restart that member
is null and `payload.restart_reason` is populated instead.
Its scope identifies observation versus navigation; closure does not tie their
timestamps together. A source-local boundary alone is not merged completion.

Clock-corrected observation input is out of scope; there is no clock-correction
application-state Event. Raw receiver adjustment evidence is independently
stored in the [receiver-telemetry measurement list](receiver-telemetry.md#ordered-report-lists).
Do not infer a restart, gap or loss of lock merely from a clock adjustment.

## Reading and persistence

Receiver restart Events use `kind=RECEIVER_RESTART`, `scope=RECEIVER`,
`applicability=POINT`, `evidence=INFERRED`, and a nullable `gpst`.
`receiver_uptime_s` retains the new uptime; `payload.restart_reason` is
`UPTIME_DECREASE` or `GPST_UPTIME_OFFSET_JUMP`. The completion payload is null.
These untimed Events follow the same [placement policy](receiver-time.md) as
receiver telemetry; they do not invent a GPST or require an epoch reference.

Consumers needing discontinuity interpretation load the relevant Events context before
using observations. Read earlier partitions as necessary to obtain a still-valid
state: yesterday alone is not a guaranteed bound. A reader may scan the compact
event history, but the format does not require loading every event into memory.
Direct/live adapters provide the same required state or explicitly UNKNOWN;
absence of context must not be mistaken for proof of continuity.

ParquetNEX stores `r00-events-part00.parquet` and subsequent parts/revisions per
station/GPST day, following [ParquetNEX naming](parquetnex.md#daily-revisions).
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

Remaining cadence/context work is tracked in [TODO](TODO.md).
