# Receiver clock telemetry pipeline

The `ngo-receiver-clock` command exports receiver clock samples, temperature
telemetry, restart events and inferred integer-millisecond clock adjustments.
It consumes expanded raw or reconstructed UBX/SBF recordings directly, recursively
in path order. `unassigned/` is excluded. Inputs are read-only; no QA stamp,
completion manifest or reconstruction index is required. Run optional
`ngo-dataset-qa` beforehand when diagnostics are wanted. Clock extraction keeps
only its necessary parser/time/clock-validity checks, not a full QA pass.
Framing and clock state persist across files, including a frame split between
files; recorded local offsets refer to the file where each frame starts.

## Build and run

Install the project and its native extension, then run:

```sh
ngo-receiver-clock --input-dir /data/gnss/era-a --output work/era-a-clock
ngo-receiver-clock -p sbf --input-dir /data/gnss/mosaic-x5 --output work/era-c-clock
```

Research runs publish their output directory only after success. Use `--overwrite`
to replace an existing run and retain it as a backup. Failed temporary directories
are reported for inspection; automatic resume is not implemented.

The C++ library validates framing and UBX checksums, associates NAV-TIMEGPS/RAWX
with NAV-CLOCK and MON-SYS, and maintains clock/temperature/restart state. Python
feeds read-only byte batches and receives batches of final samples, events and
telemetry without subprocesses or text serialization. It writes Zstandard-compressed
Parquet and JSON Lines. Native processing releases the GIL.
One receiver stream is intentionally processed sequentially: file boundaries
are not independent tasks and must not reset its state. Independent receiver
runs can execute concurrently in separate output directories.

## SBF and PPS support

SBF uses PVTGeodetic clock bias (ms converted to ns) and drift (ppm converted
to ns/s), preserving fractional values as float64. Invalid PVT solutions,
Do-Not-Use values and non-GPS clock references are counted and skipped.
ReceiverStatus supplies temperature (raw minus 100 degrees C) and uptime;
uptime decreasing starts a receiver session, just as MON-SYS does for UBX.
Temperature associations use the status timestamp and configured maximum age.

MeasEpoch revision 1+ supplies cumulative millisecond adjustments modulo 256.
The signed modular difference is checked against the bias/drift residual before
being applied; jumps need not be exactly 1 ms. Inconsistent or ambiguous jumps
start a new arc instead of silently inventing a continuous bias. Long gaps still
break arcs. Missing counters permit explicitly labeled bias inference. Counter
wrap alone is not a receiver restart. SBF has no fabricated NAV-CLOCK accuracy
fields: those columns remain null and the plot shows unavailable.

PPS reports have their own time axis, separate from NAV/clock samples. TIM-TP
uses its next-pulse week/TOW, including fractional milliseconds. UTC pulses
require a valid leap-second offset from NAV-TIMEGPS; unsupported GNSS references
or unresolved UTC-to-GPST conversion retain a null GPST and native timing fields.
Multiple TIM-TP updates for one pulse are retained in arrival order, not averaged.
SBF xPPSOffset uses its own WNc/TOW, with TimeScale and SyncAge retained.
The pulse reference scale (for example UTC) is distinct from the GPST analysis axis.

TIM-TP qErr is converted from ps to ns; qErrInvalid produces null, never a
fabricated zero. No receiver/firmware capability is inferred from a zero value
alone. SBF invalid offsets are likewise null. Error signs remain the reporting
protocol's convention; no physical PPS correction is applied to clock bias.
The plotting command writes separate hourly PPS scatter plots under `pps/`,
with reference-scale legends and unavailable counts. Untimed PPS records remain
in Parquet but cannot appear on a GPST plot. `--no-hourly` also suppresses PPS plots.

SBF extraction carries all state across files; daily files are not reboot or
clock-arc boundaries. The separate Parquet re-unwrapping command currently
supports UBX only and rejects SBF rather than discarding counted jump evidence.

## Products

| File | Contents |
| --- | --- |
| `clock.parquet` | One row per NAV-CLOCK/PVTGeodetic: bias/drift, accuracy, session/arc, unwrapped bias, adjustment and arc-start fields |
| `status.parquet` | One row per MON-SYS/ReceiverStatus: temperature, uptime, synchronization status where available, session and restart flag |
| `pps.parquet` | Independently timed TIM-TP/xPPSOffset reports, pulse reference scale, error in ns and validity |
| `summary.json` | Settings, time/units conventions, source-ID/path table, counts, ranges and temperature/drift statistics |

All three tables use Zstd compression and bounded row-group writes. Each keeps
its own message cadence; status and PPS are not forced into clock rows. Clock
adjustments are represented by `adjustment_ns` (zero when absent), nullable
`adjustment_evidence`, and nullable `arc_start_reason`. Restart is marked on
the status report even when there is no simultaneous clock solution.
`source_id` resolves through `summary.json.sources`; offsets refer to that
source. Long source paths and raw payload copies are not repeated in rows.
No separate event, telemetry, source-list or run-configuration JSONL/JSON files
are produced. Existing experimental outputs are not automatically rewritten.

All `gpst_ns` fields are signed integer nanoseconds since
**1980-01-06 00:00:00 GPST**, not Unix time and not Arrow UTC timestamps.
NAV-CLOCK retains the millisecond resolution of its `iTOW` in `gpst_ns`.
Nearest-second grouping establishes the full week from NAV-TIMEGPS or RAWX;
the clock's original subsecond offset is then restored, including across week
boundaries. `iTOW_ms` remains unchanged. The TIMEGPS fractional correction is
retained separately as `timegps_fTOW_ns`, not added to the NAV-CLOCK time axis. Without a valid
TIMEGPS anchor, a nonempty RAWX full week and nearest-second epoch can supply
time. Empty RAWX reports cannot split or anchor epochs and are counted separately.
The nominal **1 Hz** archives can contain subsecond navigation epochs; these
remain separate samples when delimited by EOE. File boundaries do not reset
clock state, and drift prediction uses the actual millisecond interval. It rejects
conflicting anchors and non-increasing clock epochs. Missing time leaves
raw clock fields intact, with null unwrapped values, and breaks the arc.

| Native field | Sample column | Display conversion |
| --- | --- | --- |
| `iTOW` | `iTOW_ms` | Divide by 1,000 for seconds of week |
| `clkB` | `clock_bias_ns` | Divide by 1,000 for microseconds |
| `clkD` | `clock_drift_ns_s` | Divide by 1,000 for microseconds/second |
| `tAcc` | `time_accuracy_ns` | Divide by 1,000 for microseconds |
| `fAcc` | `frequency_accuracy_ps_s` | Divide by 1,000,000 for ppm |

## Restart and unwrap policy

Any decrease of `MON-SYS.runTime` is a receiver restart. Equal values are
allowed because its resolution is one second. No packet reordering or
counter-rollover heuristics are applied. Restart begins a new receiver
session and a new clock arc, clearing accumulated clock adjustment.
Session IDs identify observed runtime continuity, not proof of uninterrupted
operation during periods with no MON-SYS observations.

For successive clock samples within `--max-gap` (default 50 seconds):

```text
jump_ns = bias_now - bias_previous - drift_previous * elapsed_seconds
adjustment_ns = nearest_integer(jump_ns / 1,000,000) * 1,000,000
unwrapped_bias_ns = raw_bias_ns - cumulative_adjustment_ns
```

A nonzero adjustment is accepted when the residual is within
`--jump-tolerance-ns` (default 50,000 ns). This is a configurable initial
heuristic, not a receiver specification. Both directions and multiple
milliseconds are supported; there is no fixed +1 ms trigger or exact-zero
requirement. A matching RAWX `recStat.clkReset` marks the event as confirmed;
otherwise it is explicitly bias-inferred. The flag does not provide the
adjustment magnitude. An unexplained jump or a reset flag without a resolvable
adjustment starts a new arc rather than forcing continuity.

Intervals exceeding the timeout break the clock arc, not the receiver session.
Short gaps preserve the accumulated adjustment when the bias/drift residual
passes the same consistency test; they do not fabricate intervening samples.
An interval of exactly 50 seconds remains eligible for continuity. Continuous
cross-file epochs retain all state. Unwrapped bias is continuous *within an
arc*, with an arbitrary offset at its start; it is not an absolute free-running
oscillator measurement. Never differentiate raw bias across clock adjustments
to estimate drift. `NAV-CLOCK.clkD` remains separately available.

## Temperature and statistics

MON-SYS has no `iTOW`. A monitor message is associated with its surrounding
stream epoch, not an asserted measurement timestamp. Status rows preserve this
association, source ID and byte offset; raw payloads remain in the archive.
Plotting associates the latest status no later than a clock epoch, in the same
receiver session and within `--temperature-max-age` (default 5 seconds).
This bounded backward join is performed on read, not duplicated into every
clock row. Missing or stale temperatures are unavailable. Untimed status reports
remain in `status.parquet` but are not assigned an invented time. `runtime_s`
exists only in status and is the reported value, never extrapolated uptime.

Temperature is the receiver-reported internal value, not necessarily ambient
or crystal temperature. Summary bins give count, mean and population standard
deviation of drift per reported degree Celsius. Pearson correlation uses only
samples with associated temperatures and is null when variance is zero.
These descriptive statistics do not establish causation: time trends,
receiver sessions, tracking conditions and thermal lag can confound them.
Detailed plotting is a downstream consumer, not part of this extraction CLI.

## Plotting completed telemetry

```sh
ngo-receiver-clock-plot --input-dir /data/clock-telemetry --output /data/clock-plots \
  --title "Era A receiver clock" --workers 8
```

Use a new output directory, or `--overwrite` to replace a run with a retained
backup. `hourly/` contains 1600 × 1200 PNGs with
raw bias, unwrapped bias without plot-side rebasing, NAV-CLOCK drift, separate tAcc/fAcc
axes (symmetric logarithmic scale), and receiver temperature. Missing temperature
is explicitly labeled `unavailable`, never replaced with zero. Orange vertical
lines mark bias-inferred adjustments; green lines mark RAWX-confirmed adjustments.
These are not receiver-restart markers. The green curve directly displays
`clock_bias_unwrapped_ns / 1000`: no per-arc, per-file or per-hour subtraction
of the starting value is performed. Timeout/restart boundaries reset the
accumulated correction to zero, leaving the raw bias as the new starting value,
not forcing that value to zero. Unresolved adjustments or unavailable timestamps
remain explicit breaks in reliable reconstruction; they are not disguised as
receiver reboots. Different arcs are not connected. No missing samples are generated.

`daily/` contains 1600 × 1000 overviews with hourly drift P10/median/P90,
adjustments per observed hour, interval coverage and arc counts, and median
temperature or `unavailable`. Coverage sums positive adjacent sample intervals
within the same clock arc and extractor `max_gap`; intervals are attributed to
the hour of their later endpoint. It is not inferred from sample count or a
nominal sampling rate. Arc counts mean arcs present in an hour, not restarts.
Empty hours stay blank. Era runs are processed separately, without deduplicating
or combining their overlapping calendar coverage.

Preparation reads selected Parquet columns in bounded batches, computes statistics
from all assigned samples, and stores per-hour visual caches under `cache/`.
Extrema-preserving display reduction retains endpoints, extrema, arc boundaries,
and samples around adjustment events. It is not averaging of raw clock jumps.
PNG generation runs in separate worker processes and uses fast lossless compression.
Use `--no-hourly` for daily overviews only. `hours.json` records full-sample
statistics; `images.json` lists PNGs without checksums. Plotting reads
`clock.parquet`, `status.parquet` and `pps.parquet`.
Parquet metadata records GPST, its origin, gap timeout, adjustment tolerance and
temperature age. `summary.json` also provides the compact source-ID lookup.

### Recompute existing clock products without scanning UBX

```sh
ngo-receiver-clock-reunwrap --input-dir /data/clock-telemetry \
  --output /data/clock-telemetry-gap50 --max-gap 50
ngo-receiver-clock-plot --input-dir /data/clock-telemetry-gap50 \
  --output /data/clock-plots-gap50 --workers 8
```

This vectorized, bounded-memory pass preserves original sample columns,
the separate status/PPS tables, source mapping, session IDs and restart evidence. It replaces
only clock arc IDs, cumulative corrections, unwrapped bias and embedded
adjustment/arc-start fields. Input arc IDs do not prescribe the new segmentation.
The original extraction remains untouched. The new Parquet records the new
calculation parameters; no implementation or environment snapshot is copied.
Temperature associations are recomputed from status timestamps when plotting.

Protocol references: [u-blox interface description, including MON-SYS](https://content.u-blox.com/sites/default/files/documents/u-blox-F9-TIM-2.22_InterfaceDescription_UBX-23004791.pdf)
and [RAWX clock reset semantics](https://content.u-blox.com/sites/default/files/products/documents/u-blox8-M8_ReceiverDescrProtSpec_UBX-13003221.pdf).
