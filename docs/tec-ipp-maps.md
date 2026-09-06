# Experimental hourly IPP tracks

`tec-ipp-map` computes broadcast-orbit geometry from RINEX and renders hourly
ionospheric pierce-point (IPP) tracks over pale SBAS hourly VTEC cells. Track
colors represent slant TEC change relative to the first accepted observation
in each continuous satellite/signal arc within the displayed GPST hour.
They are not absolute STEC or VTEC.
Changing ray geometry contributes to dSTEC even for a stationary ionosphere.

## Build and run

Build project-owned tools against the unchanged, pinned RTKLIB-EX submodule:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DNEOGNSS_BUILD_RINEX_TOOLS=ON
cmake --build build --target neognss_convbin neognss_rinex_geometry -j 4
```

The CMake option is off by default. These targets use `NFREQ=4`, `NEXOBS=3`
and enable GPS, GLONASS, Galileo, QZSS, BeiDou, IRNSS and SBAS as supported
by the pinned source. This does not change upstream PRN limits; C59 remains
excluded. See the [Era A conversion experiment](era-a-rinex-experiment.md).

Convert each continuous group with one convbin invocation and complete UBX
frames in time order. Conversion state must persist across daily file cuts:

```sh
build/native/neognss_convbin -r ubx -v 3.04 -od -os -oi -ot -ol \
  -o group.obs -n group.nav 'ordered-group/*.ubx'

tec-ipp-map \
  --obs group.obs --nav group.nav \
  --geometry-worker build/native/neognss_rinex_geometry \
  --sbas-hourly hourly-sbas/hourly.jsonl \
  --coastline contrib/natural-earth/ne_10m_coastline.zip \
  --output new-ipp-directory --workers 4 --color-limit 50
```

`--station-ecef X Y Z` supplies station coordinates in metres. If omitted for a
single group, the RINEX approximate position is used and explicitly identified
as such in provenance; it is not a surveyed position. Multiple groups require
one shared explicit station position.

## Geometry and arc policy

- The native worker reads the OBS/NAV using RTKLIB, obtains transmit-time
  satellite positions and clocks with `satposs`, rotates the satellite ECEF
  position for Earth rotation during signal flight time, and computes azimuth,
  elevation and IPP. Broadcast navigation is used offline, including applicable
  records decoded later in the group; this is not a real-time causality test.
- The initial thin shell is 350 km; elevation cutoff is 20 degrees. Both are
  configurable. Satellite health and ephemeris availability are checked.
- The worker retains actual observation GPST week/TOW and GPS-epoch seconds. Map windows are
  GPST hours; fractional observation timestamps are not rounded to logger epochs.
- Experimental pairs are GPS/QZSS L1C-L2X, Galileo L1X-L7X, BeiDou L2I-L7I,
  and GLONASS L1C-L2C. RTKLIB supplies actual frequencies, including GLONASS FCN.
  Unavailable pairs are excluded and counted. This is not an all-signal decoder.
- Phase cycles are converted to metres. With
  `GF = c * (L1/f1 - L2/f2)`, the relative slant content is
  `(GF - GF_at_arc_start) / (40.3e16 * (1/f2^2 - 1/f1^2))`, in TECU.
- A gap longer than 1.5 seconds, LLI slip, changed frequencies, or a GF step
  above 0.1 metres starts a new arc. Missing phase, unresolved half cycles,
  unavailable/unhealthy orbit, and low elevation invalidate the current arc.
  The GF threshold is an experimental candidate detector; no MW detector,
  phase wind-up correction, ambiguity leveling, or absolute bias calibration is
  implemented. Do not interpret all candidates as confirmed cycle slips.
- GPST hour boundaries never reset the underlying arc reference or tracking
  state; only the display is rebased per hour. Separate `--obs` inputs
  explicitly mean independent continuity groups and do reset state. Do not
  supply consecutive daily files from one continuous group as separate jobs.
  Overlapping group times are rejected.

## Visualization and parallel processing

The SBAS background defaults to PRN 137, GNSS/signal/frequency IDs 1/0/0, and a
25% hourly coverage threshold. Its 0-100 TECU colormap is blended with white at
18% strength, including its own colorbar. It is only shown for hours with
available SBAS grid data. The track uses a separate red/blue dSTEC colorbar.
`--color-limit` is the symmetric track range (default ±50 TECU), fixed across
images. Each arc's first valid observation within an hour is the display zero;
if an arc starts later, its actual first sample is used without extrapolation.
This removes changes accumulated before the displayed hour, but large changes
within one hour can still saturate the color scale. Increase `--color-limit`
if needed. Hourly rebasing is for display only: `tracks/*.jsonl` retains the
original continuous-arc-relative `dstec_tecu`. Each image's `display_references`
in `images.json` records the reference GPST and original dSTEC for every arc.

The black star is the station. Dots mark the first plotted point in each
arc/hour and arrows indicate increasing time. PRNs label track endpoints.
There is no spatial interpolation or assigned satellite coverage area. Missing
arcs are not connected. First and last output hours may be partial.

All accepted 1 Hz points are retained in `tracks/*.jsonl`. Only display points
are reduced to `--plot-step` (default 10 seconds), retaining arc/hour endpoints.
The plotted line interpolates between retained display points in the same arc.
Raw geometry and phase fields are retained in `tracks/*.csv`.

`--workers` bounds a process pool (default up to four processes): independent
RINEX groups can compute geometry/dSTEC concurrently, and hourly PNGs render
and compress concurrently. Each render process owns its Matplotlib state.
One continuous group's decoding and arc tracking remain sequential. Large
RINEX groups are loaded into native memory, so reduce workers if needed.

PNG compression defaults to level 3 and can be set with `--png-compression`
from 0 through 9. Native output size is 1800x1200. SBAS-only `sbas-grid-render`
also supports these two options; its stateful aggregation remains sequential.

Outputs include `images.json`, per-group geometry and track files, logs, and
`completed.json` with input/binary/source checksums, the pinned RTKLIB revision,
station-position source, policy and diagnostic counts. The output directory
must be new; interrupted runs are not automatically resumed.
