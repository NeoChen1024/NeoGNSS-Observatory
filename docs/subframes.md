# SBAS grid snapshots and maps

The shared `SbasGridProcessor` consumes CommonNEX and uses the same typed SBAS
L1 decoder as [broadcast-message decoding](broadcast-sbas.md). It maintains
source-local MT18/26 grid state and MT0 research restrictions. It does not parse
UBX/SBF envelopes or apply satellite orbit/clock corrections.

## Commands

```sh
ngo-sbas-grid --input-dir /data/cnex --output /data/sbas-grid \
  --snapshot-interval 3600
ngo-sbas-plot --input-dir /data/sbas-grid --output /data/sbas-maps

ngo-sbas-realtime -p sbf --host RECEIVER_HOST --port 2006 \
  --setup /data/setup.json --snapshot-interval 60 > snapshots.jsonl
```

`ngo-sbas-grid` reads the latest ParquetNEX revisions and their parts. RawBits,
navigation completion Events and receiver-telemetry GPST provide ordered
navigation context. Observation completion is not substituted for navigation
time. Timed receiver restart Events clear the state; ordinary diagnostic notices
do not. Raw recordings are normalized by `ngo-cnex-import` before offline use.

`ngo-sbas-realtime` is an adapter over the shared CommonNEX normalizer. Choose
`--host` or `--input recording.sbf.xz` (UBX and plain inputs also supported).
The scientific processor and snapshot semantics are the same as offline replay.
TCP delivery defaults to five seconds; `--duration` limits TCP capture only.
JSONL goes to stdout unless `--output` exclusively creates a file. Diagnostics
go to stderr. Each line includes the snapshot summary and a `grid` list, which
may be empty. Decimal times are strings and missing values are null.

Both processing commands default to a 3,600-second snapshot interval,
600-second correction age and 1,200-second mask age. These are explicit
reception-context research policies, not an aviation integrity guarantee.
`--correction-age` and `--mask-age` can select another positive-integer policy.
There is no separate fixed-gap reset or mandatory interval-file intermediate.

## Time, masks and state

Snapshot targets are integer multiples of `interval_s` since the GPST origin.
The first target is strictly after the first reliable processing time. A target
emits state known before the first input at that newly reached context; it is
not precise RF-time reconstruction. Input timestamps retain picosecond precision.
Other reliable navigation progress can age grids even without new SBAS messages.

Mask/issue association is local to setup, broadcasting satellite and signal set.
MT26 is usable only with a fresh matching band/IODI mask. An acquired band can
produce research values before the whole advertised mask set is complete;
`mask_set_complete` distinguishes that case. Corrections received without the
matching mask are counted and not retrospectively applied. Original decoded
messages remain independently available through BroadcastMessageDecoder.

Issue changes, mask removal, same-issue mask changes and stale-mask reacquisition
invalidate dependent values. Fresh masks do not resurrect old corrections.
Do-not-use and not-monitored replace previous usable values immediately.
Correction and mask expiry are independent. Unknown-time grid messages cannot
update or refresh timed values. No host clock is used to invent GPST.

MT0 does not discard independently received grid values. Snapshots retain its
last known context and distinguish `ACTIVE` (within 60 seconds), `ELAPSED`,
`NOT_OBSERVED` and `UNKNOWN` (untimed MT0). These are research annotations:
elapsed/not-observed is not certification of safety or service recovery.
GIVEI remains an index, not a linear weight or measured standard deviation.

Midnight and file/batch boundaries do not reset scientific state. Explicit
discontinuity and receiver restart do. At a timed restart, an already-reached
snapshot boundary is emitted before the reset. A reset inside a window discards
that segment's unfinished statistics; the next window is marked partial.
Untimed restart Events are rejected because they cannot be ordered safely.
EOF emits no future or off-grid snapshot; the last unfinished window is not
published. There is no persisted processor checkpoint or append/resume workflow
in this command yet. Replay preceding CommonNEX context when rebuilding outputs.

## Snapshot products

Daily files are partitioned by **snapshot target GPST date**, not window start:

```text
YYYY/MM/DD/grid.parquet
YYYY/MM/DD/snapshots.parquet
```

Both use Zstandard level 3 and dictionary encoding. The summary file preserves
empty snapshots; grid files need not exist on days without any grid rows.
`--overwrite` replaces a completed output while retaining its backup. No raw
archive or existing CommonNEX dataset is modified.

All time/duration fields below are decimal128(38,12) seconds, indices/counters
are int64, booleans are bool and physical quantities/fractions are float64.

| Grid fields | Interpretation |
| --- | --- |
| `setup_id`, `satellite_system`, `satellite_number`, `bitstream_source` | Station and original source identity; `S`/37 denotes S37; signals are a string list |
| `snapshot_gpst`, `window_start_gpst` | Evaluation target and nominal statistics window `[start,target)` |
| `band`, `mask_bit`, `latitude`, `longitude` | Fixed IGP identity and coordinates in degrees |
| `iodi`, `givei`, `reported_gpst`, `expiry_gpst` | Latest report's issue, index, reception context and dependent expiry |
| `status`, `mask_set_complete` | `USABLE`, `DO_NOT_USE`, `NOT_MONITORED`, `EXPIRED`, `MASK_CHANGED` or `MASK_REMOVED`; current complete-mask flag |
| `delay_m`, `vtec_tecu` | Current usable vertical delay and equivalent VTEC; null when not usable |
| `valid_duration_s`, `coverage`, `mean_vtec_tecu` | Usable duration, duration divided by the full nominal interval, and valid-time-weighted mean; mean is null if no valid duration |
| `last_mt0_gpst`, `mt0_restriction`, `mt0_duration_s` | Last timed MT0, current restriction annotation and restricted/unknown duration within usable statistics time |
| `output_sequence` | Run-local delivery order, not a foreign key |

VTEC is `delay_m * (1575.42e6)^2 / (40.3 * 1e16)` TECU. It is SBAS broadcast
equivalent VTEC, not receiver-observed STEC. Invalid current values can still
have a meaningful previous-window mean. The report's IODI/GIVEI describe current
state, not every contributing value in that window. Expired/removed cells can
appear once to deliver their final statistics/status, then disappear. All usable
sources are preserved, independent of presentation priority.

Summary fields are `setup_id`, `snapshot_gpst`, `window_start_gpst`,
`segment_start_gpst`, `partial_window`, `grid_rows`, `usable_cells`, `sources`
and `output_sequence`. `sources` counts known source identities in the current
segment, not necessarily currently usable providers. Initial/reset partial
coverage is not normalized to pretend a full interval was observed.

## Python API

```python
from neognss_observatory.sbas_grid import SbasGridProcessor

processor = SbasGridProcessor(setup_id, interval_s=60)
output = processor.feed(common_nex_batch_group)
# output maps "grid" / "snapshots" to lists of owned Arrow RecordBatch objects.
processor.finish()  # no extrapolated tail snapshot
```

RawBits-only callers can use `process(batch)`; input progress is then limited
to those rows. Explicit ordered `(gpst, is_restart)` delivery controls can be
passed as `progress_controls`. Native decoding/state/Arrow work releases the
GIL. Source identities are bounded to 64; one call can cross at most 4,096
snapshot targets and emit at most one million rows. Exceeding bounds fails
explicitly; a failed native processor must be reconstructed.

## Selection and rendering

`ngo-sbas-plot` makes one composite map per available snapshot, not separate
provider maps. `--quantity current` selects current usable values; `--quantity
mean` selects already-computed per-source window means. It does **not** rebuild
a time-varying source composite before averaging. Exact retrospective interval
selection is outside this tool's scope.

Default priority is MSAS, BDSBAS, KASS, GAGAN, SouthPAN; `--priority` reorders it.
Current selection uses the newest report within a provider; a tied/newer
do-not-use or not-monitored report prevents fallback to an older usable GEO of
that provider. Another provider may still supply the cell. Ties use satellite
number then grid identity. Unknown providers are labeled by Sxx and follow
configured providers. No weighting or cross-provider averaging is performed.

`--min-coverage` defaults to zero; color limits default to 0–200 TECU. The
bundled Natural Earth 10m coastline is reused per worker. `--start/--end` accept
GPST `YYYY-MM-DDTHH`, with exclusive end. Missing cells remain absent and MT0
contributions are marked. The five-degree tile overlay is illustrative, not a
precision service-coverage polygon. `images.json` records snapshot timestamps.

STEC's optional background uses the same current-snapshot selection at exact
STEC hour targets. It does not extrapolate missing snapshots or reinterpret a
window mean as an instantaneous value.

`ngo-sbas-map-video --images-manifest /data/sbas-maps/images.json --output /data/sbas.mp4`
encodes manifest-ordered PNGs without synthesizing gaps. Its frame sidecar retains
`snapshot_gpst`. Encoding defaults and validation options are in `--help`.
