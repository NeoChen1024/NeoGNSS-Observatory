# CommonNEX acquisition and processing policy

[Overview](overview.md) | [Importer operation](importer.md)

Use one continuous, non-overlapping recording path per import. Multiple
recording interfaces may belong to one station, but the importer does not
automatically merge them. A native antenna selection identifies the source
input for a single-antenna station; it does not create another CommonNEX ID.

Do not require prior QA, restitch indexes or scientific solvers to import data.
Decode supported families together, with persistent source context across
physical files and GPST days. Each consumer selects only the catalogs it needs.
Report excluded/unsupported mappings, incomplete tails and unavailable time;
configured signals are not proof of actual coverage.

Observation requires measurement GPST. RawBits and telemetry preserve missing
time according to [receiver time](receiver-time.md). Source scalar telemetry
duplicates and conflicts follow the [telemetry assembly contract](receiver-telemetry.md),
not a generic record deduplication rule. Distinct RawBits occurrences remain
distinct even when bodies and times match.

## Overlap and late data

Head probing orders files; the batch importer stops on observation/navigation
time reversal unless explicitly allowed. It does not deduplicate, reconcile overlaps or guarantee unique rows
at equal timestamps. See [input ordering](importer.md#head-only-ordering-and-time-reversal).

If reconciliation is explicitly implemented later, first-imported valid
information wins and later sources may supplement missing content. Conflicts
must be reported, not averaged or silently overwritten. This is a deferred
policy, not current automatic merge support.

Completed published rows are not silently amended. New tail content uses new
parts; insertion/correction within published coverage requires a complete
replacement revision under [ParquetNEX](parquetnex.md#daily-revisions).
Acquisition/FTP and concurrent readers during publication are outside the
batch importer. Continuation mechanics belong to [importer](importer.md#tail-continuation-and-reconstruction),
not the scientific schema.

## Cadence classification

The importer classifies the interval between successive closed observation
epochs, in acquisition order, and records findings as
[Events](events.md#cadence-and-time-order). The sequence is the one that
produces `COMPLETE` Events, so an epoch without retained rows still counts as
an epoch. RawBits, telemetry arrivals, companion blocks and per-satellite rows
are not classified.

The nominal period P is estimated from the data, not declared: it is the lower
median of the most recent positive intervals, up to 30 of them. Classification
starts once five intervals are available; before that no gap or short-interval
Event is produced. Every positive interval enters the window after it has been
classified, including one found to be a gap, so a genuine cadence change is
adopted while isolated outliers do not move the estimate. Setup's
`epoch_period_s` is the navigation epoch period and takes no part in this.

Using exact decimal differences, with dt the interval and a tolerance of +/-20%:

| Interval dt | Classification |
| --- | --- |
| dt < 0 | `TIME_REVERSAL` |
| dt = 0 | `REPEATED_TIMESTAMP` |
| 0 < dt < 0.8 P | `EPOCH_INTERVAL_SHORT` |
| 0.8 P <= dt <= 1.2 P | Within tolerance; no Event |
| dt > 1.2 P | `OBSERVATION_GAP` |

The endpoints are inclusive: for P = 1000 ms, 800 through 1200 ms is acceptable.
Each Event stores the P it was classified against. A gap is a coverage finding,
not proof of receiver failure or an exact missing-epoch count; a short interval
is a timing anomaly, not a gap, and no cause is asserted. Reversal and repeat
findings do not depend on P and are produced from the first interval.

The estimate is part of the continuation state: file, batch and GPST-day
boundaries, and receiver restarts, do not restart it. It follows the cadence
actually present in the imported recording, so a deliberately decimated
recording is classified against its own interval. Because the estimate depends
on preceding input, an import that starts elsewhere in a recording can classify
the first few intervals differently.

Observations, timestamps and findings are retained. Out-of-range intervals are
not a reason to reject the import, snap epochs, fabricate samples or reset
processing state; solver reset/timeout policies remain separate. A reversal is
recorded with both times in acquisition order and is never hidden by sorting;
the batch importer additionally stops on it unless told otherwise.

## Consumer continuity

Protocol completion is not proof of gapless signals. Observation and navigation
completion are independent; consumers apply [Events](events.md) by their scope,
not by transport batch order. Completion and restart records are distinct
from the cadence findings above.

Storage and computation are separately incremental. Consumers may require
earlier records or their own checkpoints; a daily partition is not guaranteed
to initialize a solver. File/day/batch boundaries do not reset scientific state.
Do not invent observations, interpolate gaps or silently ignore restart evidence.
Live transport boundaries are delivered separately under the [streaming API](live.md).
