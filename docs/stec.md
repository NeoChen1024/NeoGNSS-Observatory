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

Predownloaded products are mandatory. The resolver selects CODE MGEX Final SP3,
30-second CLK and daily code OSB, BRDC, and CODE operational Final hourly IONEX
over the observation interval plus the configured margin (default 6 h).
Contents and applicability, not filenames alone, determine usability. Missing
exact-signal corrections, orbit/clock coverage, fresh health ephemerides or hourly
GIM brackets cause an explicit error; there is no zero-bias or broadcast fallback.
Unhealthy satellites are excluded. Spatially missing GIM cells remain unavailable.

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
diagnosis, not clipped to zero. Plotting these finalized estimates is a separate
downstream extension; the current command produces numerical tables.

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
stability/sensitivity analysis, additional signal pairs and systems, and a plot
consumer for these tables. PPP-AR is not needed for this path.

References:

- [IONEX specification](https://files.igs.org/pub/data/format/ionex1.pdf)
- [Single-station receiver bias estimation](https://agupubs.onlinelibrary.wiley.com/doi/full/10.1029/2007RS003785)
- [GNSS bias statistical framework](https://arxiv.org/abs/1508.02957)
- [Modified mapping and GNSS ionosphere comparisons](https://www.cambridge.org/core/journals/publications-of-the-astronomical-society-of-australia/article/ionospheric-modelling-using-gps-to-calibrate-the-mwa-i-comparison-of-first-order-ionospheric-effects-between-gps-models-and-mwa-observations/5AD4292C5D6A5C2CA7D63EE0F039E99F)
