# Offline PPP Float

`ngo-ppp` runs static, forward-only GPS L1/L2 Float PPP from expanded UBX or SBF.
`libcppgnss` decodes observations, `libneognss-obs` adapts them to the pinned
RTKLIB-EX core, and Python writes daily GPST Parquet. There is no RINEX
observation intermediate or solver executable subprocess.

This initial implementation is GPS-only. Multi-GNSS extensions are tracked in
[TODO](TODO.md). PPP-AR is not enabled;
the pinned RTKLIB PPP-AR function is a stub. Required products are local inputs,
not downloaded by the solver.

## Run

Initialize `contrib/RTKLIB` as well as the other build dependencies before
installing the Python package. Copy [the example configuration](../config/ppp.example.toml)
and replace its station settings with the actual marker and antenna setup.
All relative paths in this configuration resolve relative to the configuration
file, not the working directory.

```sh
ngo-ppp -p sbf --input-dir /data/receiver/day \
  --config /data/config/ppp.toml --output /data/ppp \
  --start 2025-08-15T00:00:00 --end 2025-08-16T00:00:00
ngo-ppp-plot --input-dir /data/ppp --output /data/ppp-plots --workers 2
```

`--start` is inclusive and `--end` exclusive; both are optional GPST calendar
times without a timezone suffix. Without them, use the observed stream extent.
Do not infer that input filenames establish GPST. Files must be expanded,
stable, nonoverlapping and in stream order. The reader carries state across
files; overlapping Era A inputs still require reconstruction first.

Select `-p ubx` for RXM-RAWX or `-p sbf` for MeasEpoch. When both MeasEpoch and
Meas3 are present, only MeasEpoch is used and ignored Meas3 blocks are counted.
Meas3-only input is unsupported. Only the main antenna and the configured exact
GPS signal pair are used. Defaults are `1C` and `2L`; change them explicitly if
the observations use a different supported pair. Other signals/systems are not
silently renamed to GPS; unselected decoded signal counts are reported.

`interval` controls measurement-time decimation (default 30 s), not rounding of
measurement timestamps. Observation lock/slip state is tracked before decimation.
Lock decreases, half-cycle changes and gaps beyond `gap_timeout` (default 50 s)
inform phase discontinuity handling. The RTKLIB core also applies its own GF/MW
slip detectors. Physical files and GPST days do not reset the filter. This does
not constitute a receiver reboot detector for every UBX/SBF receiver.

## Local products and calibration

The current resolver supports the downloader's `COD0MGXFIN` SP3, 30-second CLK,
daily OSB and ERP filenames, plus `BRDC00IGS_R` navigation. It selects dates from
actual observation batches with a configurable safety margin (default 6 hours,
minimum 1 hour). It unpacks downloaded `.gz` products into temporary storage;
it does not decompress raw `.xz` archives. Products reload as the required date
window changes while filter state survives.

Exact product files must be unique. Code OSBs are selected by GPS satellite,
exact observable and validity interval, converted from ns to meters and applied
once. Phase OSBs are not used by this Float solver. No missing code bias is
replaced with zero. Precise orbit/clock coverage is checked, including bracketing
CLK values; there is no intentional broadcast-orbit or SP3-clock fallback.

`antenna_catalogs` lists local ANTEX files in precedence order. The first must be
the IGS20 satellite calibration catalog compatible with the precise products.
Receiver lookup searches all listed catalogs, so NGS `ngs20.atx` can extend the
available types. Select an exact antenna/radome record with G01/G02 calibration
valid for the batch. Duplicate candidates use explicit configuration order and
the number of matches is reported; no per-frequency splicing is performed.
Individual serial-number selection and a general conflict-resolution UI are
not implemented. Missing calibration is an error, not a zero-correction mode.

NGS20 is the NGS composite calibration catalog in the IGS20 system, not a
different reference frame. It includes IGS entries and additional NGS entries.
Prepare it separately through the [NGS calibration catalog](https://www.ngs.noaa.gov/ANTCAL/).

Station ECEF coordinates describe the marker. `arp_enu_m` is the ARP displacement
from that marker in east/north/up meters. Initial-position values are starting
estimates, not a fixed-position constraint. Do not compare ARP coordinates with
marker coordinates without applying the actual antenna offset.

## Outputs

- `epochs/GPST-YYYY-MM-DD.parquet`: native measurement `gpst_ns`, status (`6` for
  PPP Float, `0` for no valid PPP solution), used satellites, ECEF position and
  covariance, ENU differences from a priori, formal 1-sigma uncertainties, GPS
  receiver clock in ns, estimated ZTD and uncertainties.
- `satellites/GPST-YYYY-MM-DD.parquet`: selected complete dual-frequency GPS
  observations, azimuth/elevation, used flag, slip indication, and post-fit
  ionosphere-free phase/code residuals in meters. Unused residuals are NaN.
  These are not all tracked satellites and are not individual-signal residuals.
- `summary.json`: scientific settings, product/calibration selection, counts,
  observed extent, last valid static solution, geodetic coordinates, covariance
  and horizontal 95% error ellipse. No hash/provenance bundle is generated.

The final position is the last valid forward filter estimate, not an average of
epoch coordinates. Heights are ellipsoidal. Uncertainty is model-derived formal
precision, not proof of absolute accuracy. `valid_epoch_fraction` uses attempted
decimated epochs as its denominator; it is not raw observation completeness.

One invocation represents one solution over the actual input measurement span,
optionally restricted by `--start` / `--end`. The solver is not restarted at
file or GPST-day boundaries. Daily Parquet files are storage partitions only.

The plotting command reads all partitions and produces parallel whole-solution
`solution-*.png` figures for full-interval and
post-first-hour ENU detail, ZTD/clock/satellite counts, sky distribution, and
phase/code residuals. The first-hour exclusion is a display choice, not a
convergence detector; the detail figure is omitted for runs of one hour or less.
Time axes show elapsed hours from the first solution epoch in GPST, with no
midnight wrap or fixed 24-hour extent. All figures read only Parquet and the result summary.
The same command also writes `ppp-report.pdf`, a consolidated, shareable report
with solution settings, marker coordinates, formal uncertainty and a horizontal
95% ellipse, applied models, limitations and all whole-solution figures.
The report uses a scientific-summary layout with processing settings and
numerical diagnostics. Summary text, the ellipse and diagnostic charts are
vector content. PNG and PDF share the same plotting function, but PDF redraws
the numerical data through the vector backend rather than embedding PNGs.
Matplotlib supplies PDF generation without an external PDF tool.
Ambiguity timelines and automatic CSRS comparison are not yet implemented.

## Scientific limits

The initial model uses GPS ionosphere-free L1/L2, precise SP3/CLK, satellite code
OSB, GPS PCO and elevation-only (NOAZI) PCV, phase wind-up, solid Earth tides and
an estimated zenith tropospheric delay with Niell mapping. Ocean loading,
troposphere gradients and azimuth-dependent PCV are not applied. ERP is loaded;
the configured solid-Earth tide mode does not imply all loading/pole corrections.

The current RTKLIB antenna structure does not preserve a full independent
receiver calibration for every constellation/frequency. Multi-GNSS support
requires a deliberate adapter/model extension, not just enabling more systems.
Successful GPS validation is not evidence of validated Galileo/GLONASS/BeiDou
PPP, arbitrary SBF measurement variants or a general receiver calibration model.

CSRS-PPP is an independent reference, not exact ground truth. Compare the same
time span, reference frame/epoch and marker definition, and account for its AR,
multi-GNSS, antenna and atmosphere/loading models before interpreting differences.
