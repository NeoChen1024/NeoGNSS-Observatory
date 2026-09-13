# CommonNEX continuity events

Status: v0 design draft; not an implemented schema or API.

[Overview](overview.md) | [Import policy](import-policy.md)

## Purpose

Events are shared model records describing cross-epoch discontinuities and
receiver events. They are separate from observations and RawNav but share Stream
and epoch identities. A gap is not a receiver restart, proof of carrier-phase
loss of lock, or an instruction to reset every downstream algorithm.

| Kind | Meaning |
| --- | --- |
| `observation_gap` | Adjacent observation interval exceeds 1.2 times the declared nominal period |
| `epoch_interval_short` | Positive observation interval is below 0.8 times the nominal period |
| `time_reversal` | Time decreases in the established acquisition sequence |
| `repeated_timestamp` | Distinct epochs have the same timestamp; interpretation is required |
| `receiver_restart` | Explicit restart evidence, including supported runtime-decrease interpretation |

The interval endpoints and exclusions follow the
[cadence classification](import-policy.md#epoch-interval-classification).
Signal-local loss-of-lock and cycle-slip flags remain observation quality fields;
do not inflate them into stream-wide events. Partial epoch completeness remains
an epoch field. Protocol completion is not itself a discontinuity.

## Selected fields

| Field | Type | Meaning |
| --- | --- | --- |
| `stream_id` | string | Affected logical Stream |
| `event_id` | uint64 | Event identity within that Stream |
| `kind` | enum | Event kind listed above |
| `scope` | enum | `stream`, `observation`, or `raw_nav` impact scope |
| `gpst_ns` | uint64 | Valid GPST locating the event |
| `epoch_family` | enum? | `observation` or `navigation`, when epoch references are supplied |
| `previous_epoch_id` | uint64? | Previous epoch in the relevant acquisition sequence |
| `next_epoch_id` | uint64? | Following epoch in that sequence |
| `evidence` | enum | `receiver_report`, `runtime_decrease`, or `epoch_interval` |
| `interval_ns` | int64? | Signed difference of the referenced timestamps for interval events |
| `expected_period_ms` | float64? | Positive nominal period used for cadence classification |

References resolve within the Stream and selected epoch family, including
across days. They must not imply a one-to-one relationship between ObservationEpoch
and NavigationEpoch. Event IDs are identities, not timestamps; several events
can share a timestamp. Detailed receiver evidence may use a typed auxiliary
record rather than arbitrary per-event JSON. Additional evidence mappings remain
to be defined when adapters need them.

For an interval event, `gpst_ns` is the next available epoch's time and both
epoch references identify the measured interval. In a reversal, "next" means
acquisition order, not later GPST. For a restart, locate it at the first valid
epoch establishing the evidence, not an invented exact reboot instant.
Null interval/reference fields mean not applicable or unavailable, not zero.

An observation gap records the interval between available epochs. It does not
claim exact missing sample times or an exact count of lost epochs. RawNav-only
datasets may have navigation-associated events without observations; do not
apply observation cadence thresholds to asynchronous navigation occurrences.
Legitimate untimed RINEX special events remain in their separate event/source
mapping, not fabricated as timed continuity events here.

## Import and persistence

Generate merged-stream events after reconciling proven recording duplicates
and complementary coverage. Source-local loss repaired by another recording
is not a remaining logical-stream gap. Preserve genuine reversal evidence;
neither sort it away nor mistake arrival order across overlapping sources for
receiver acquisition order. File rollover, GPST midnight and logger handover
alone generate no discontinuity event.

ParquetNEX stores `events.parquet` inside each Stream/GPST-day/revision directory.
An interval event belongs to the day of its `gpst_ns` (the next available epoch),
even when the previous epoch is in another day. Use the same revision selection
as the related tables. Repair recomputes affected boundary events; prior versions
remain only in prior revisions. Include an adjacent day in repair when its event
changes. There is no append-only global event file.

No events need be written when none were found. Absence of a file alone is not
proof that all event kinds were detectable: concise import capability/coverage
metadata distinguishes supported evaluation from unavailable evidence. This is
not a QA stamp, provenance bundle or a prerequisite QA pass for processing.

No reported discontinuity means no detected event under the available evidence,
not guaranteed continuity or validity of every signal. Event records do not
replace per-observable quality fields.
