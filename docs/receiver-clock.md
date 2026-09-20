# Receiver clock analysis

`ngo-receiver-clock` reads one CommonNEX logical station. It does not open raw
UBX/SBF recordings, reconstruct epochs or repeat dataset QA. Import the raw
recording with `ngo-cnex-import` first.

## Run

```sh
ngo-receiver-clock --input-dir work/era-c-cnex --output work/era-c-clock
ngo-receiver-clock-plot --input-dir work/era-c-clock --output work/era-c-clock-plots --workers 8
```

There is no `--protocol/-p` option. The input directory contains `setup.json`
and GPST `YYYY/MM/DD/` partitions. For each catalog/day, select only the highest
revision and all its parts in order, including an empty replacement revision.
No filename-derived measurement timestamp or original receiver archive is needed.

The default clock-estimate reference is GPST. Use `--reference-time-scale` to
select another reported reference without changing the GPST sample axis.
If more than one clock source remains (for example both SBF PVT forms), select
`--clock-source SBF-PVTGeodetic` explicitly. Do not combine independent estimates
or silently deduplicate same-time reports. PPS/status records retain their
independent cadence and reference information.

Research outputs are published by directory rename after success. Use
`--overwrite` to replace an existing output with a retained backup. This is a
whole-selection analysis, not an incremental-analysis checkpoint workflow.
Read completed CommonNEX publications; concurrent import/publication is not supported.

## Inputs and processing

| CommonNEX catalog | Use |
| --- | --- |
| receiver-telemetry | Integrated navigation clock, status and ordered measurement/pulse reports |
| events | Receiver restart evidence and boundaries |

The native importer owns protocol interpretation and time association. Analysis
uses vectorized NumPy/Arrow batches, with state carried across rows, row groups,
parts and days. It does not serialize science batches through JSON.
Load at most one day's catalog data at a time; the restart prepass retains only
compact boundary/index information. Temperature statistics read the small
derived clock/status tables after publication staging has completed.

Adjustment evidence comes from each row's acquisition-cycle measurement list,
without changing its recorded measurement times. The unwrap adapter uses any
explicit reset and the last reported cumulative counter; the complete list is
retained in derived clock output. Pulse reports are flattened at their own times.
There is no old-catalog fallback or source-selection option.

## Restart and missing time

Follow the importer's [receiver-time contract](commonnex/telemetry-time.md).
Receiver restart Events are matched to their originating status report using
the reported uptime and nullable GPST, in record order. Analysis does not
re-detect a restart from a counter rollover or fit uptime to GPST.

A timed restart starts a new derived session at that coordinate. For an untimed
restart, the interval after the last preceding timed status and before the
first following timed status has uncertain cross-catalog association. Keep
the clock values and original GPST but set `continuity_known=false`,
`receiver_session_id=-1`, and unwrapped outputs to null there. A missing
following anchor leaves the interval open. This does not fabricate a restart
time or discard the raw estimates. Uptime-only records remain in the tables.

A new derived session clears accumulated adjustment. Session IDs describe the
available evidence, not proof that an unmonitored receiver never rebooted.
Missing clock time or unusable bias/drift also breaks reconstruction.
Non-increasing usable clock GPST within a session is an error; it is not sorted away.

## Unwrap

For successive usable clock samples within `--max-gap` (default 50 seconds):

```text
jump_ns = bias_now - bias_previous - drift_previous * elapsed_seconds
unwrapped_bias_ns = raw_bias_ns - accumulated_adjustment_ns
```

SBF-reported cumulative counts use the documented signed modulo-256 difference
only for known SBF PVT sources and counts in 0..255. The uint64 container itself
does not imply modulo semantics. Check the count-derived adjustment against the
bias/drift residual; inconsistent evidence starts a new arc.

Otherwise, round the residual to the nearest integer millisecond. Accept a
nonzero adjustment only within `--jump-tolerance-ns` (default 50,000 ns).
An exactly matched RAWX flag labels it `rawx_confirmed_adjustment`; otherwise
use `bias_inferred_adjustment`. Counted SBF evidence is labeled
`sbf_counted_adjustment`. An unresolved jump or adjustment flag starts a new
arc without claiming reboot.

Short gaps preserve accumulated adjustment; intervals greater than the timeout
start a new arc. Equality is accepted. Never reset merely at file/day/hour/plot
boundaries. A new arc starts at its reported bias, not artificially at zero.

## Products

| File | Contents |
| --- | --- |
| clock.parquet | Original selected clock columns plus session, continuity, arc, adjustment and unwrapped bias |
| status.parquet | Original status columns plus derived session/restart and plot aliases |
| pps.parquet | Original pulse columns plus nanosecond plot aliases |
| summary.json | Configuration, counts, ranges and temperature/drift statistics |

All writers explicitly use Zstandard level 3. No raw-source offset tables or
protocol envelopes are generated. Empty status/PPS tables permit plots to show
unavailable telemetry without manufacturing measurements.

Original `gpst` and time quantities retain DECIMAL(38,12). Derived `gpst_ns`
is signed integer nanoseconds since 1980-01-06 GPST, truncating less than 1 ns
for the existing plot interface; it is not Unix time. Bias/error seconds are
scaled in decimal before conversion to float64 nanoseconds, preserving sub-ns
values to float64 precision. Original frequency integers remain present;
plot drift is ns/s and frequency accuracy is ps/s.

PPS error has the CommonNEX sign: actual edge minus ideal edge, positive late.
Do not reapply the UBX sign mapping or infer invalidity from zero. Unsupported
pulse target-time conversions remain null exactly as imported.

Temperature joins use the latest timed status no later than the sample, within
`--temperature-max-age` (default 5 seconds) and in the same known session.
No interpolation, uptime-to-GPST conversion or cross-restart join is performed.
The summary retains temperature-bin drift statistics and Pearson correlation;
these are descriptive, not evidence of causation.

## Re-unwrapping and plots

```sh
ngo-receiver-clock-reunwrap --input-dir work/era-c-clock --output work/era-c-clock-reunwrap
```

Re-unwrapping shares the same vectorized algorithm and supports both imported
UBX and SBF evidence. It preserves source columns and receiver-session context;
rerun `ngo-receiver-clock` when changing that context.

The plot command produces separate hourly clock/PPS PNGs and daily overviews.
It reads only derived Parquet, never raw receiver messages. Unusable unwrap
rows are omitted from clock curves, not connected across; missing temperature
and accuracy are displayed as unavailable. The green unwrapped curve is not
rebased per hour or per file. PNG rendering remains multiprocessing-enabled.
