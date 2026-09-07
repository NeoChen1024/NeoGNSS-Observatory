# Receiver clock telemetry pipeline

The `receiver-clock` command exports receiver clock samples, temperature
telemetry, restart events and inferred integer-millisecond clock adjustments.
It consumes completed, quality-controlled **GPST reconstruction** directories,
not raw archives. `unassigned/` is excluded. Inputs are read-only and their
sizes are checked against reconstruction metadata.

## Build and run

Install the project and its dependencies, then build the native scanner:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target cppubx2_clock_scan -j
receiver-clock --input-dir /data/gnss/era-a \
  --worker build/libcppubx2/cppubx2_clock_scan \
  --output work/era-a-clock
```

Research runs publish their output directory only after success. Use `--overwrite`
to replace an existing run and retain it as a backup. Failed temporary directories
are reported for inspection; automatic resume is not implemented.

The C++ scanner validates framing and UBX checksums. Only NAV-CLOCK, NAV-TIMEGPS, NAV-EOE,
MON-SYS and the RAWX header cross the process boundary. Python performs
bounded-memory state tracking and batched Zstandard-compressed Parquet writes.
One receiver stream is intentionally processed sequentially: file boundaries
are not independent tasks and must not reset its state. Independent receiver
runs can execute concurrently in separate output directories.

## Products

| File | Contents |
| --- | --- |
| `samples.parquet` | One row per NAV-CLOCK, native units, associated temperature, session/arc IDs, unwrapped bias |
| `events.jsonl` | Receiver restarts, clock arc boundaries and clock adjustments |
| `mon-sys.jsonl` | Every MON-SYS payload, decoded runtime/temperature and association provenance |
| `sources.jsonl` | Input paths, byte sizes and frame counts |
| `run.json` | Human-readable calculation parameters |
| `summary.json` | Counts, ranges, temperature/drift bins and Pearson correlation |

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
stream epoch, not an asserted measurement timestamp. Raw payloads and byte
offsets are retained. A sample can reuse that temperature for at most
`--temperature-max-age` (default 5 seconds), with its source, reference epoch
and age recorded. No stale temperature is carried across a detected clock gap
or restart. Messages with no time association remain in `mon-sys.jsonl`.
`runtime_s` in samples is the last reported value, not an extrapolated uptime.

Temperature is the receiver-reported internal value, not necessarily ambient
or crystal temperature. Summary bins give count, mean and population standard
deviation of drift per reported degree Celsius. Pearson correlation uses only
samples with associated temperatures and is null when variance is zero.
These descriptive statistics do not establish causation: time trends,
receiver sessions, tracking conditions and thermal lag can confound them.
Detailed plotting is a downstream consumer, not part of this extraction CLI.

## Plotting completed telemetry

```sh
receiver-clock-plot --input-dir /data/clock-telemetry --output /data/clock-plots \
  --title "Era A receiver clock" --workers 8
```

The output directory must be new. `hourly/` contains 1600 × 1200 PNGs with
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
`samples.parquet` and `events.jsonl`, not an execution-provenance bundle.
Parquet metadata records GPST, its origin, gap timeout, adjustment tolerance and
temperature age. `run.json` and `summary.json` are informational, not read gates.
Older experimental files lacking required scientific metadata must be regenerated
or explicitly reprocessed; there is no automatic compatibility layer.

### Recompute existing clock products without scanning UBX

```sh
receiver-clock-reunwrap --input-dir /data/clock-telemetry \
  --output /data/clock-telemetry-gap50 --max-gap 50
receiver-clock-plot --input-dir /data/clock-telemetry-gap50 \
  --output /data/clock-plots-gap50 --workers 8
```

This vectorized, bounded-memory pass preserves original sample columns,
temperature associations, runtime/session IDs and restart evidence. It replaces
only clock arc IDs, cumulative corrections, unwrapped bias, unwrap quality and
derived adjustment/arc events. Old short-gap arc IDs do not instruct new resets.
The original extraction remains untouched. The new Parquet records the new
calculation parameters; no implementation or environment snapshot is copied.
It does not recover temperature associations missing from the original extraction.

Protocol references: [u-blox interface description, including MON-SYS](https://content.u-blox.com/sites/default/files/documents/u-blox-F9-TIM-2.22_InterfaceDescription_UBX-23004791.pdf)
and [RAWX clock reset semantics](https://content.u-blox.com/sites/default/files/products/documents/u-blox8-M8_ReceiverDescrProtSpec_UBX-13003221.pdf).
