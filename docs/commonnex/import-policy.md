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

Head probing orders files; normal reading rejects observation/navigation time
reversal. It does not deduplicate, reconcile overlaps or guarantee unique rows
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

## Planned cadence classification

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

## Consumer continuity

Protocol completion is not proof of gapless signals. Observation and navigation
completion are independent; consumers apply [Events](events.md) by their scope,
not by transport batch order. Current completion/restart records are distinct
from the planned cadence findings above.

Storage and computation are separately incremental. Consumers may require
earlier records or their own checkpoints; a daily partition is not guaranteed
to initialize a solver. File/day/batch boundaries do not reset scientific state.
Do not invent observations, interpolate gaps or silently ignore restart evidence.
Live transport boundaries are delivered separately under the [streaming API](live.md).
