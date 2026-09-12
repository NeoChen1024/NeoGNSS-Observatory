# GPS STEC and receiver DCB

`ngo-stec` reads expanded UBX RAWX or SBF MeasEpoch directly, levels dual-frequency
carrier phase to satellite-bias-corrected code, and estimates an effective
receiver differential code bias using CODE global ionosphere maps (GIM).
The result is a **GIM-constrained absolute STEC estimate**, not independently
calibrated TEC. RINEX OBS, PPP solutions and QA stamps are not required.

## Run

Copy [the example configuration](../config/stec.example.toml), set the station's
antenna reference-point ECEF coordinates, exact signal pair, and local product
root. Paths in the configuration are relative to that file.

```sh
ngo-stec -p sbf --input-dir /data/recordings \
  --config config/stec.example.toml --output work/stec
```

Use `-p ubx` for UBX. Overlapping Era A recordings must first be reconstructed;
ordered, nonoverlapping recordings need no re-stitch. `--start` and `--end` are
inclusive/exclusive GPST calendar times without a timezone suffix. The observation
payload determines time; filenames only establish traversal order.

The initial path supports GPS L1/L2, antenna 0, with explicitly selected signal
codes (`1C`/`2L` by default). It does not substitute another signal or constellation
when a selected pair is unavailable. The reader reports ignored observations and
Meas3 blocks; Meas3-only SBF fails explicitly. Valid foreign frames are skipped
with the shared warning policy. Raw archives are never modified.

The default output interval is 30 seconds. Native-rate observations still update
phase continuity; decimation happens after that update. Physical file, batch,
GPST midnight and product-window boundaries do not reset arcs. Gaps over 50 s,
lock decreases, half-cycle changes and conservative geometry-free jump candidates
close arcs. The jump threshold is `gf_jump_m + gf_rate_m_s * elapsed_seconds`;
this is an experimental exclusion rule, not a proof of a cycle slip. A common
receiver clock adjustment is not by itself a phase break. Missing code does not
break otherwise usable phase continuity. No missing samples are interpolated.

## Products and conventions

The solver uses predownloaded local products and never downloads on demand.
The resolver selects CODE MGEX Final SP3,
30-second CLK and daily code OSB, BRDC, and CODE operational Final hourly IONEX
over the observation interval plus the configured margin (default 6 h).
Contents and applicability, not filenames alone, determine usability. Missing
files or observation-specific product coverage generate warnings, not a whole-run
abort. There is no zero-bias or broadcast fallback. Unhealthy satellites are
excluded; malformed products and ambiguous local file matches remain errors.

Phase samples and arc continuity survive product gaps. Missing orbit/clock or
fresh health data leaves geometry unavailable; missing satellite biases leaves
the corrected code combination unavailable. Such samples cannot contribute to
elevation-weighted leveling. Missing GIM leaves the slant reference unavailable
and excludes that sample from receiver DCB fitting. Leveling and DCB can still
use other eligible observations within their arc/window; finalized STEC is only
available where those estimates are valid. No stale product is extended over a
gap: the precise orbit interpolation stencil must have consecutive 5-minute
epochs, clocks must bracket the epoch, and GIM interpolation needs hourly maps.

`product_issues` is a per-sample bitmask: 1 orbit, 2 clock, 4 health, 8 satellite
bias and 16 GIM unavailable. Geometry-dependent GIM is not evaluated when geometry
is unavailable. These flags record the checks actually reached, not an exhaustive
inventory of every missing dependency. `summary.json` records per-cause counts
and missing filenames. A run can finish as `partial_products` or
`partial_calibration`; neither status fabricates missing corrections. Missing
margin-day files alone do not invalidate epochs that have sufficient coverage.
Downloads that cannot be obtained can remain absent; the same rules apply.

The estimator deliberately aligns satellite interfrequency biases to the GIM's
own GPS C1W-C2W datum. For code-bias convention `P_corrected = P - b`, define:

```text
delta_intra = (b_signal2 - b_2W) - (b_signal1 - b_1W)  # MGEX OSBs, meters
satellite_P2_minus_P1_bias = delta_intra - DCB_GIM_C1W_minus_C2W
code_gf_corrected_m = P2 - P1 - satellite_P2_minus_P1_bias
```

IONEX DCBs in ns are converted to meters. Their UT day validity is converted to
GPST in native code; MGEX OSB validity is already GPST. The reference GIM DCB and
MGEX corrections are each applied once. Using the same producer does not make
the operational GIM and MGEX interfrequency bias solutions identical. The small
within-frequency transfer between these product streams is still a modeling
assumption, not proof of perfect consistency.

RTKLIB-EX supplies precise orbit interpolation and line-of-sight geometry.
Emission time is iterated from station/satellite range and transmit-frame ECEF
is rotated to reception-frame ECEF. CLK coverage is checked explicitly, although
the geometry-free observable cancels common clock terms and this range-based
geometry does not require subtracting CLK from the GF measurement. Satellite
centre-of-mass coordinates are used, without antenna phase-centre offsets.

IONEX map epochs are external UT, not GPST. The native adapter converts them
once using RTKLIB's UTC/GPST conversion. It uses strict spatial bilinear and
sun-fixed temporal interpolation, with no missing-cell fill or extrapolation.
CODE's grid coordinates are geocentric: IPPs come from intersection of the
reception-frame LOS with the 6821 km sphere (6371 km base + 450 km).
The modified single-layer mapping is separately defined by
`1/sqrt(1 - (6371/(6371+H) * sin(0.9782*z))^2)`, with geodetic zenith angle `z`
and default effective mapping height `H=506.7 km`. This approximation and its
height are explicit scientific parameters; they are not the IPP shell height.
GIM sensitivity to this mapping choice remains part of the absolute error budget.

## Leveling and receiver bias

For frequencies `f1 > f2`, phase in cycles and code in meters:

```text
K = 40.3e16 * (1/f2^2 - 1/f1^2)       # meters/TECU
phase_gf_m = lambda1*L1 - lambda2*L2
level_offset_m = robust_weighted_mean(code_gf_corrected_m - phase_gf_m)
stec_leveled_tecu = (phase_gf_m + level_offset_m) / K
stec_absolute_tecu = stec_leveled_tecu - receiver_bias_tecu
```

Leveling uses a Huber location estimate with elevation-squared sine weights.
Only samples above 30 degrees enter leveling by default. At least 20 eligible
samples spanning 600 s are required. An insufficient arc retains phase samples
but has no usable level offset. A low leveling scatter is not evidence that
systematic code multipath is small.

Receiver bias is fitted jointly across satellites, never independently for each
arc. The fit residual is `stec_leveled_tecu - gim_stec_tecu`. Five-minute arc/time
bins are represented by their median; weights account for elevation and GIM RMS
(2 TECU floor), and each arc has equal total weight. A second Huber fit produces
one effective receiver bias per window. The default 24-hour windows start at the
first emitted sample, not at daily Parquet boundaries. A window is an estimation
partition, not an assumption of a physical midnight jump.

After rejecting gross residual outliers, a valid window requires at least six
hours of occupied five-minute time bins, four satellites, six arcs, three azimuth
quadrants and 20 degrees of elevation span. Residual scatter must not exceed
15 TECU. These are configurable experimental gates, not a calibrated confidence
interval. `receiver_dcb_ns` is the additive receiver **P2-P1** delay:
`receiver_bias_tecu * K / c * 1e9`, the opposite pair ordering from the GIM's
satellite C1W-C2W reference label.

An insufficient or inconsistent window keeps a diagnostic candidate estimate,
but its usable receiver bias and absolute STEC are null. The CLI reports
`partial_calibration` without discarding the extracted samples. Do not lower
coverage thresholds merely to obtain a non-null result. Receiver configuration
changes should use separate runs; this first version does not estimate temperature
dependence or automatic receiver-DCB change points.

## Intermediate files and reading

- `samples/GPST-YYYY-MM-DD.parquet`: time, PRN, arc ID, phase/code GF, geometry,
  mapping and GIM slant reference/RMS. Day partitions are storage only.
- `arcs.parquet`: finalized offsets, counts, scatter, validity, start/end times
  and boundary reasons. Arc IDs are unique within the output directory.
- `receiver_bias.parquet`: window validity, effective receiver DCB, coverage,
  residual diagnostics and model status.
- `summary.json`: compact counts and scientific settings. Parquet metadata
  also carries the information needed to interpret the tables.

No full observation cache or per-message JSON is generated. Finalized numerical
arrays and robust estimation run in C++ with the GIL released; Python handles
file scheduling, compressed Parquet and vectorized joins. Only one calibration
window is collected in memory. There are no Python callbacks per raw frame.

```python
from neognss_observatory.stec import read_stec

for table in read_stec("work/stec"):
    # Arrow tables, with nullable finalized columns appended by a batched join.
    absolute = table["stec_absolute_tecu"]
    leveled = table["stec_leveled_tecu"]
    receiver_bias = table["receiver_bias_tecu"]
```

This reader does not reopen raw observations or CDDIS products. Phase/code
combinations and small solution tables are authoritative; the finalized columns
are not duplicated in another large file. Negative estimates are retained for
diagnosis, not clipped to zero. `read_stec(root, start_ns=..., end_ns=...)` can
select a GPST interval and exposes `receiver_window_id` alongside the finalized
columns. Plotting uses this reader without recalculating calibration.

## Hourly trajectory plots

`ngo-stec-plot` renders IPP trajectories from these Parquet tables, not raw
UBX/SBF, RINEX or CDDIS products. Each PNG covers an observed GPST hour; colours
represent individual absolute STEC samples, **not** an hourly STEC average.
All images share one map extent and colour scale. No hour/arc origin is subtracted.

```sh
ngo-stec-plot --input-dir work/stec --output work/stec-plots \
  --coastline contrib/natural-earth/ne_10m_coastline.zip --workers 4
```

The repository's existing Natural Earth 10m coastline ZIP supplies the black
coastline on a white background. `--coastline` can select another local ZIP.
The renderer does not download map assets. Degree graticules, station marker,
GPS PRN labels and track start/end markers provide context; the geographical
display is not an equal-area representation of sampled ionospheric coverage.

Options:

- `--start YYYY-MM-DDTHH` / `--end YYYY-MM-DDTHH`: inclusive/exclusive GPST hours.
  Selection uses stored timestamps and Parquet statistics, not filename dates.
- `--vmin 0 --vmax 200`: a fixed absolute STEC colour scale in TECU. Values are
  not numerically clipped: below-scale points use magenta, above-scale points
  use black, and the figure reports their count.
- `--extent WEST EAST SOUTH NORTH`: override the common map bounds. Longitudes
  use a station-centred interval; a dateline-crossing extent may extend past 180.
- `--sbas-grid /data/sbas-grid --sbas-prn 137`: optional daily SBAS grid Parquet.
  Valid-time-weighted hourly mean **VTEC** is a separate faded background using
  the same hue-based `turbo` colourmap as `ngo-sbas-grid-plot`, not a correction
  applied to the plotted STEC. Its colourbar uses the same palette independently
  of the STEC colour scale.
  `--background-alpha` defaults to 0.18, `--sbas-vmax` to 200 TECU and
  `--min-coverage` to 0.25. Missing SBAS cells remain absent and are reported;
  no nearest-hour or alternate-PRN substitution occurs.
- `--workers 4 --png-compression 3`: process-parallel rendering and PNG encoding.
- `--overwrite`: publish the completed directory by rename, retaining the
  previous output as a backup.

Lines do not cross arc or receiver-calibration-window boundaries, unavailable
samples, dateline discontinuities, or display gaps exceeding
`max(gap_timeout, 1.5 * output_interval)`. Start/end markers indicate plotted
pieces within the hour, not necessarily the physical beginning/end of an arc.
Only usable absolute estimates are coloured. An observed hour without valid
leveling/DCB gets an explicit unavailable panel; entirely absent hours are not
fabricated. Calibration failure never falls back to uncorrected or zero-based TEC.

Outputs are `png/GPST-YYYY-MM-DD--HH-00-00.png` (1800 x 1200 pixels) and a small
`images.json` with GPST ordering, sample counts and rendering settings. This
manifest is accepted by the existing map-video tool. No duplicate scientific
sample tables or persistent hourly caches are created. Preparation spools one
hour at a time; each worker loads coastline geometry once and the render queue
is bounded. Optional SBAS aggregation reads each selected day once.

## Interpretation and remaining limits

The GIM provides the absolute anchor, so comparison with that same GIM is only
a residual/consistency check, not independent validation. Model structure,
mapping errors and local ionosphere departures can leak into receiver DCB.
No absolute accuracy is inferred from GIM RMS or residual scatter alone.
The constant bias assumption can fail with temperature, hardware or tracking
changes. Per-arc code multipath and receiver code smoothing remain limitations.
Phase wind-up and antenna phase-centre corrections are not yet applied here;
the PPP pipeline's model list must not be attributed to STEC.

Future extensions include independent-reference comparisons, receiver-bias
stability/sensitivity analysis and additional signal pairs and systems.
PPP-AR is not needed for this path.

References:

- [IONEX specification](https://files.igs.org/pub/data/format/ionex1.pdf)
- [Single-station receiver bias estimation](https://agupubs.onlinelibrary.wiley.com/doi/full/10.1029/2007RS003785)
- [GNSS bias statistical framework](https://arxiv.org/abs/1508.02957)
- [Modified mapping and GNSS ionosphere comparisons](https://www.cambridge.org/core/journals/publications-of-the-astronomical-society-of-australia/article/ionospheric-modelling-using-gps-to-calibrate-the-mwa-i-comparison-of-first-order-ionospheric-effects-between-gps-models-and-mwa-observations/5AD4292C5D6A5C2CA7D63EE0F039E99F)
