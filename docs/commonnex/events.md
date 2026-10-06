# CommonNEX events and scoped context

Events record boundaries, discontinuities and cadence findings for UBX/SBF input.
[Overview](overview.md) | [Import policy](import-policy.md)

## Purpose

Observation and RawBits carry their own times; Events are not epoch lookup
tables, and no science row must reference an event ID. Setup metadata
references remain shared descriptive context.

Measurement and navigation scopes are independent even at equal timestamps.
Protocol completion is not discontinuity, proof of all signals being present,
or an instruction to reset a solver. Signal-local slip/lock/quality indicators
remain on Observation rows.

## Event kinds

| Kind | Scope | Meaning |
| --- | --- | --- |
| `EPOCH_COMPLETION` | `OBSERVATION`, `NAVIGATION` | Closure of an observation or navigation epoch, including epochs with no retained science rows |
| `RECEIVER_RESTART` | `RECEIVER` | Supported restart evidence, not mere logger reconnection |
| `OBSERVATION_GAP` | `OBSERVATION` | Interval between successive closed observation epochs above 1.2 times the estimated period |
| `EPOCH_INTERVAL_SHORT` | `OBSERVATION` | Positive interval below 0.8 times the estimated period |
| `REPEATED_TIMESTAMP` | `OBSERVATION` | Successive closed observation epochs have equal time; not duplicate proof |
| `TIME_REVERSAL` | `OBSERVATION`, `NAVIGATION` | Time decreases in acquisition order |

The cadence estimate and its tolerance are defined in
[import policy](import-policy.md#cadence-classification). The batch importer
treats a time reversal as an input error unless explicitly allowed; see
[importer](importer.md#head-only-ordering-and-time-reversal).

## Fields

| Field | Type | Meaning |
| --- | --- | --- |
| `setup_id` | string | Affected logical station |
| `kind` | enum | Kind above |
| `scope` | enum | `OBSERVATION`, `NAVIGATION` or `RECEIVER` |
| `gpst` | GpstTimestamp? | Event location or target epoch time; see the per-kind rules below |
| `applicability` | enum | `EPOCH` for completion, otherwise `POINT` |
| `evidence` | enum | `REPORTED` or `INFERRED` |
| `payload` | struct | Nullable `epoch_completion` and `cadence` structs and `restart_reason` string |
| `receiver_uptime_s` | Duration? | Reported uptime for receiver restart evidence; otherwise null |
| `anchor_gpst`, `frame_index` | GpstTimestamp?, uint64 | [Arrival-order coordinate](receiver-time.md#arrival-order-coordinate) of the source frame that produced the Event |

`GpstTimestamp`, `TimeDelta` and `Duration` use `DECIMAL(38,12)` seconds.
No event ID or epoch foreign key is required. Multiple events may share time,
and several Events produced by one source frame share its arrival coordinate.
`EPOCH` applies only to its declared epoch context, never implicitly forward.
`POINT` records an occurrence, not an enduring state. Exactly one payload
member is non-null for each kind.

Equal timestamps cannot distinguish conflicting/repeated acquisition contexts.
Do not resolve ambiguous applicable events by file order or last-write-wins.

## Kind-specific rules

### Epoch completion

`payload.epoch_completion` holds `completion` and `completion_basis`:

| Completion | Basis | Scope | `gpst` | Evidence | Condition |
| --- | --- | --- | --- | --- | --- |
| `COMPLETE` | `RECORD_STRUCTURE` | `OBSERVATION` | Epoch time | `REPORTED` | A structurally complete UBX RXM-RAWX |
| `COMPLETE` | `PROTOCOL_BOUNDARY` | `OBSERVATION` | Epoch time | `REPORTED` | SBF measurement group closed by its matching EndOfMeas |
| `COMPLETE` | `PROTOCOL_BOUNDARY` | `NAVIGATION` | Epoch time | `REPORTED` | UBX NAV-EOE matching a valid NAV-TIMEGPS |
| `INCOMPLETE` | `PROTOCOL_BOUNDARY` | `OBSERVATION` | Group time | `INFERRED` | SBF measurement group superseded by another epoch without its EndOfMeas; its rows are not emitted |
| `INCOMPLETE` | `INCOMPLETE_TAIL` | `OBSERVATION` | Group time | `INFERRED` | SBF measurement group still open at a declared stream end or discontinuity |
| `UNKNOWN` | `PROTOCOL_BOUNDARY` | `NAVIGATION` | null | `REPORTED` | UBX NAV-EOE without a matching valid NAV-TIMEGPS: the closed epoch cannot be identified |

Every closed observation epoch produces one `COMPLETE` Event, so the newest
one bounds the observations delivered so far. `INCOMPLETE` marks absent
observations and never advances that bound. A measurement group whose own time
is invalid produces no Event; it is reported through importer counts.
`INCOMPLETE_TAIL` requires an explicit end: ordinary end of input retains the
open group for continuation instead. Completion scope identifies observation
versus navigation; closure does not tie their timestamps together.

### Cadence and time order

`OBSERVATION_GAP`, `EPOCH_INTERVAL_SHORT`, `REPEATED_TIMESTAMP` and
`TIME_REVERSAL` use `evidence=INFERRED` and store `payload.cadence`:

| Field | Type | Meaning |
| --- | --- | --- |
| `previous_gpst` | GpstTimestamp | Preceding time on the same axis, in acquisition order |
| `interval_s` | TimeDelta | `gpst - previous_gpst`; zero for a repeat, negative for a reversal |
| `expected_period_s` | Duration? | Period estimate used for classification; null while it is not yet available |

`gpst` is the later record in acquisition order, which also assigns the Event's
GPST day. These are direct coordinates, not references. An interval is
evidence, not an assertion of exact missing sample times or counts.
Observation-scope Events compare successive closed observation epochs and
precede the `COMPLETE` Event of the epoch at `gpst`. A navigation-scope
`TIME_REVERSAL` compares successive receiver navigation times and carries a
null `expected_period_s`; asynchronous navigation messages have no cadence
classification.

### Receiver restart

`RECEIVER_RESTART` uses `evidence=INFERRED` and a nullable `gpst`: restart time
is the first justified evidence, not an invented exact reboot instant.
`receiver_uptime_s` retains the new uptime; `payload.restart_reason` is
`UPTIME_DECREASE` or `GPST_UPTIME_OFFSET_JUMP`. An untimed restart follows the
[placement policy](receiver-time.md) of receiver telemetry and is located among
RawBits and telemetry rows by its arrival coordinate.

Clock-corrected observation input is out of scope; there is no clock-correction
application-state Event. Raw receiver adjustment evidence is independently
stored in the [receiver-telemetry measurement list](receiver-telemetry.md#ordered-report-lists).
Do not infer a restart, gap or loss of lock merely from a clock adjustment.

## Reading and persistence

Consumers needing discontinuity interpretation load the relevant Events before
using observations, reading earlier partitions as necessary: yesterday alone is
not a guaranteed bound. Direct/live adapters provide the same Events; absence
of context must not be mistaken for proof of continuity. A consumer that
cannot process a kind it encounters fails explicitly rather than ignoring it.

ParquetNEX stores `r00-events-part00.parquet` and subsequent parts/revisions per
station/GPST day, following [ParquetNEX naming](parquetnex.md#daily-revisions).
Assign by `gpst`; untimed Events use the receiver-time placement policy. Select
the latest Events revision and all its parts for each required day. Read only
after all related science/Events writes finish; revision numbers need not match
between catalogs. Tail completion may add a part without revising published
events. Rebuild only day/catalog scopes whose existing content changes.

The importer does not reconcile overlapping sources; do not claim unique
merged-stream continuity for such inputs. Source arrival order is not
necessarily receiver acquisition order. File rollover and handover are not
discontinuities. No events file is necessary when no events exist. Absence of
a cadence Event before the period estimate is available is not evidence of
regular sampling.
