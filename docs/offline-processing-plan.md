# Offline GNSS Observation Data Processing Plan

Last updated: 2026-09-05

## 1. Goals and non-goals

The near-term goal is to use the existing archive to answer three testable
questions:

1. Can single-station, multi-frequency carrier phase produce stable relative
   dTEC, ROT/ROTI, and TID candidate observations?
2. How much measurable discontinuity is introduced by receiver changes and
   logging gaps?
3. Are contemporaneous MSAS broadcast ionospheric corrections spatially and
   temporally consistent with station-derived TEC?

Static PPP is an independent validation track for coordinate stability,
antenna or receiver changes, and possible earthquake displacement. It is not a
prerequisite for the TEC pipeline. External-reference receivers, rubidium
clocks, and real-time processing are also out of scope for this phase.

## 2. Verified archive inventory

This snapshot was obtained by directly reading the current archives. Date
ranges in this table are initially derived from filenames; actual coverage
must be established from GNSS payload timestamps.

| Era | Preservation source | Filename date range | Current inventory | Role |
|---|---|---:|---:|---|
| A | `/hdd/archive/tcserver-www/archive/ubx24h` | 2023-05-19 to 2023-11-23 | 188 UBX/XZ files, 187 filename-days, about 27 GB | Legacy approximately 24-hour rolling chunks; filenames include local `+0800` timestamps and overlaps must be resolved |
| B | `/hdd/archive/tcserver-www/archive/ubx` | 2023-09-21 to 2025-03-02 | 551 UBX/XZ files, 512 filename-days, about 82 GB | UTC-oriented daily logger; at least 30 days contain multiple restart segments |
| C | `/hdd/archive/tcserver-www/gnss/mosaic-x5/BX4ACP` | 2025-03-31 to 2025-08-24 | 147 days, 441 XZ files, about 75 GB | Daily SBF master plus 30 s RINEX OBS/NAV; the golden dataset |

Additional verified facts:

- Eras A and B share 60 filename-days from 2023-09-21 through 2023-11-23.
  This is a cross-logger validation interval; neither copy should be discarded
  arbitrarily.
- Era C contains exactly three files for every directory day: `*.25_.xz` is
  SBF, `*.25o.xz` is observation RINEX, and `*.25p.xz` is navigation RINEX.
  Compressed SBF totals about 75.16 GB; existing OBS and NAV files total about
  698 MB and 36 MB respectively.
- The first Era C day is partial. Existing RINEX begins at
  2025-03-31 05:15:30 GPST. The 2025-08-24 file ends at 23:59:30. Three files
  being present does not by itself prove 100 percent coverage.
- Sampled UBX data was produced by a ZED-F9P running HPG 1.32 and protocol
  27.31. It has a 1 Hz observation cadence and contains both `RXM-RAWX` and
  `RXM-SFRBX`. RTKLIB-EX successfully converted samples into observation,
  navigation, and SBAS-log outputs.
- Sampling confirms the relationship between UBX filename boundaries and
  payload time. Era A's `20230521T165000+0800` denotes 08:50:00 UTC, while the
  first converted observation is 08:50:19 GPST. Era B's
  `20250228T000000` produces a first observation at 00:00:19 GPST. This is
  consistent with GPS-UTC being 18 seconds plus the sample start offset; RINEX
  calendar fields must not be interpreted as UTC.
- Both 16 MiB UBX samples contain PRN 137. The Era A sample contains 26 MT18
  and 135 MT26 messages; the Era B sample contains 22 MT18 and 110 MT26
  messages. This proves the decode path exists, but it is not a full-archive
  coverage statistic.
- The existing Era C RINEX 3.04 header identifies a mosaic-X5 running firmware
  4.14.10.1 and includes GPS, Galileo, SBAS, GLONASS, BeiDou, QZSS, and NavIC.
  Existing observations are 30 s; raw SBF is the authoritative 1 Hz source.
- Complete existing Era C RINEX days begin at 00:00:00 GPST, unlike the UTC
  logger boundaries in Eras A and B. Cross-era canonicalization therefore
  requires an explicit common partition convention.

## 3. Data ownership and canonical layout

Raw archives are always read-only. Every output must be reproducible from raw
data, a pinned toolchain, and versioned configuration, with a provenance record
for each artifact.

```text
/hdd/...                         immutable preservation masters

repo/
  config/                        converter, QC, PPP, and TEC policies
  manifests/                     source inventories and run manifests
  src/                           parsers and processing code
  docs/

work/                            disposable, outside Git
  unpacked/                      temporary UBX and SBF
  rinex/<era>/<yyyy>/<doy>/      canonical 1 Hz RINEX 3.04
  sbas/<era>/<yyyy>/<doy>/       packet logs and decoded state
  qc/<era>/<yyyy>/<doy>/         daily QC tables
  tec/<era>/<yyyy>/<doy>/        per-arc Parquet
  ppp/<era>/<yyyy>/<doy>/        solutions and trace summaries
  products/                      versioned external GNSS products
```

Each run manifest must record at least:

```text
source path, compressed size and hash, XZ integrity
receiver and firmware, first and last GNSS time, cadence histogram
tool name, version, Git SHA, full command, and configuration hash
output path and hash, epoch count, warnings, and completion status
external-product URLs, hashes, and product family
```

Filename time never substitutes for payload time. Both the original GNSS time
scale and converted UTC must be retained to prevent leap-second and local-time
ambiguity.

Canonical partitions use `[00:00:00, 24:00:00) UTC`; RINEX headers and epochs
may still correctly identify GPST. During 2023-2025, a UTC day boundary occurs
at 00:00:18 GPST. Conversion and comparison must therefore read adjacent source
days with an 18-second guard interval. Existing Era C 30 s RINEX is aligned to
GPS days, so comparisons use absolute-epoch joins rather than matching
filenames or line numbers.

## 4. Toolchain policy

### RTKLIB-EX

Use the `main` branch of
[`rtklibexplorer/RTKLIB`](https://github.com/rtklibexplorer/RTKLIB). The project
is now named RTKLIB-EX, and further development on the old `demo5` branch ended
on 2025-07-26. The revision verified during this inventory was
[`06e8644287ff07efc4c53bbf3e7f9dafb0355605`](https://github.com/rtklibexplorer/RTKLIB/commit/06e8644287ff07efc4c53bbf3e7f9dafb0355605).
CMake successfully built `convbin` and `rnx2rtkp`, which report version
`EX 2.5.1`.

Recording only `main` is insufficient. Production runs must pin a commit SHA or
container digest.

RTKLIB-EX is used for:

- UBX to RINEX 3.04, broadcast NAV, and RTKLIB SBAS-log conversion;
- an independent SBF-to-RINEX cross-check;
- broadcast and precise satellite geometry;
- the static PPP baseline.

Source inspection shows that `rnx2rtkp` reads SP3, RINEX CLK, ERP, ANTEX, BLQ,
and DCB/BIA/BSX. Its BIA/BSX handling currently reads satellite code biases
only. It has no OBX reader. Consequently, OBX is not required by the RTKLIB PPP
profile and should only be mirrored after adding a solver that explicitly
consumes it.

### Septentrio converter

Era C canonical conversion should first use a pinned, data-compatible version
of Septentrio `sbf2rin`. RTKLIB-EX provides the second implementation. Existing
30 s receiver RINEX is a contemporaneous reference, not the 1 Hz master. Each
converter must retain the original RINEX observable codes; different tracking
codes must not be collapsed prematurely into abstract L1 and L2 labels.

## 5. Normalization and QC pipeline

```text
raw XZ
  -> XZ integrity and compressed SHA-256
  -> packet framing and checksum inventory
  -> era-specific normalization
  -> UTC-day canonical RINEX 3.04 at native 1 Hz
  -> observation and arc QC
  -> analysis-specific views (PPP 30 s, TEC 1 s)
```

### Era A: stitch raw packets before RINEX conversion

1. Parse every chunk using UBX sync, length, and checksum fields, recording byte
   offsets.
2. Use `NAV-EOE` `(GPS week, iTOW)` sequences to locate overlap anchors in
   adjacent files.
3. Verify that at least two adjacent EOE intervals contain identical packet
   bytes or sequences before removing a duplicated region.
4. Preserve the original stream order for asynchronous packets, especially
   `RXM-SFRBX`. EOE is an anchor, not a coarse boundary for deleting everything
   before an EOE packet.
5. Produce a lossless normalized UBX stream before splitting it into UTC days
   according to payload GNSS time.

If overlap verification fails, mark a conflict and retain both sources instead
of guessing which one is correct.

### Era B: segment-aware UTC-day assembly

1. Treat multiple files on the same day as restart segments. Do not concatenate
   them solely in lexical order.
2. Parse each segment's first and last payload times, checksum errors, RAWX
   epochs, and EOE sequence.
3. Resolve overlaps with the same anchor and byte/packet verification used for
   Era A. Preserve gaps exactly and never interpolate them.
4. `20250302T000000.ubx.xz` is about 1.0 GB, far larger than a typical 170 MB
   daily file. Its payload range must determine whether it spans multiple days
   or contains duplicate data.

### Era C: 1 Hz SBF master

1. Regenerate 1 Hz OBS and NAV from `*.25_.xz`.
2. Extract the regenerated RINEX at the exact epochs present in the existing
   30 s RINEX, then compare satellite sets, observable codes, phase, code, LLI,
   SNR, and numerical tolerances.
3. Convert the same SBF independently with RTKLIB-EX. Record any signal-mapping
   differences from `sbf2rin` in a machine-readable report rather than silently
   selecting one output.
4. Deterministically decimate to 30 s only for the PPP view. TEC and QC retain
   1 Hz observations.

UTC-day conversion must read the adjacent SBF day files as guard inputs. When a
neighboring source day is absent, record the boundary coverage loss instead of
mislabeling a GPS-day file as UTC-complete.

### Daily QC schema

At minimum, record:

```text
era, receiver, UTC day
first_epoch, last_epoch, nominal_interval_s
expected_epochs, observed_epochs, coverage_fraction
duplicate_epochs, checksum_errors
gap_count, largest_gap_s, gap duration histogram
restart/source-segment count
per-constellation and per-signal epoch/observation counts
LLI and half-cycle events, GF/MW jump candidates
arc count and usable arc-duration distribution
```

`coverage_fraction` cannot determine usability by itself. One long outage and
many short dropouts have different consequences for TID spectra, cycle-slip
detection, and daily PPP.

## 6. TEC processing

### 6.1 First MVP: relative dTEC

Start with the dual-frequency carrier geometry-free combination. It removes the
first-order common geometric range, receiver and satellite clock, and neutral
atmosphere terms, but every continuous arc retains an unknown ambiguity
constant. The first output is therefore arc-relative `dSTEC`, not purportedly
calibrated absolute TEC.

Start a new arc whenever any of these conditions occurs:

- an epoch gap exceeds the cadence policy;
- RINEX LLI or receiver half-cycle/sub-half-cycle state changes;
- a geometry-free phase jump is detected;
- a Melbourne-Wubbena jump is detected when compatible code and phase pairs
  are available;
- the receiver restarts or the observable code changes.

Signal pairs must be defined from the actual RINEX codes in each Era, not
guessed from generic frequency indices. Initial pairs should include GPS L1/L2,
Galileo E1/E5b, BeiDou B1I/B2I, and QZSS L1/L2. Era C can add GPS L1/L5,
Galileo E1/E5a, and other pairs with complete code and phase observations.

Each per-arc output row contains:

```text
time_gpst, time_utc, receiver, satellite, signal_pair, arc_id
phase_gf_m, dSTEC_TECU, ROT_TECU_per_min, ROTI
azimuth_deg, elevation_deg, ipp_lat, ipp_lon
cn0 values, slip detectors, quality_flags
```

The first implementation may use broadcast NAV for satellite positions. Use a
350 km thin-shell IPP model and record the shell height in metadata. Begin with
a 20-degree elevation cutoff and run a separate 30-degree sensitivity result.

### 6.2 Absolute and slant-leveled TEC

Only the second stage produces code-leveled carrier STEC:

1. Estimate the carrier ambiguity offset for each arc using a robust statistic.
2. Apply satellite DCB or OSB products exactly compatible with the observable
   pair.
3. Do not claim that receiver DCB can be independently solved from
   unconstrained single-station observations. Either estimate it as a daily
   nuisance parameter constrained by a GIM or spatial-temporal model, or
   publish only relative TEC.
4. Map STEC to VTEC on the 350 km shell.
5. Use IGS IONEX GIM as a bias and large-scale sanity check, not as 1 Hz truth.

IGS identifies DCBs as necessary corrections for extracting TEC and publishes
IONEX VTEC and multi-GNSS bias products. Analysis centers may use different
temporal resolutions, so manifests must retain the analysis center, solution
class, and sampling. See
[IGS ionosphere and DCB products](https://igs.org/products/#ionosphere) and the
[Bias-SINEX specification](https://files.igs.org/pub/data/format/sinex_bias_100.pdf).

### 6.3 TID and event products

Retain undetrended TEC. Detrending is a parameterized derived view. Store at
least:

- 1 Hz dTEC and ROT;
- ROTI with an explicit window;
- a robust 30 s or 60 s aggregate;
- the detrending method and window, plus a transfer-function note;
- an availability mask, with filtering or interpolation across gaps forbidden.

A single station can reveal time-satellite and IPP structure, but it cannot by
itself reliably infer horizontal propagation velocity. Keep the terms "TID
candidate" and "multi-station-confirmed TID" distinct.

## 7. Static PPP processing

Use RTKLIB-EX `rnx2rtkp` as the first reproducible baseline:

- `ppp-static`, dual-frequency ionosphere-free, with precise ephemeris and
  clocks;
- deterministic decimation of canonical 1 Hz OBS to 30 s;
- both fixed-window and daily station-coordinate solutions;
- explicit configuration and manifest fields for satellite and receiver
  antenna models, ERP, and ocean loading;
- no interpretation of millimeter-level repeatability as physical deformation
  while receiver or antenna metadata remains incomplete.

Products required by the RTKLIB profile:

```text
required: broadcast NAV, ORB.SP3, CLK.CLK, current analysis-frame ANTEX
recommended: ERP.ERP, compatible DCB/OSB BIA/BSX, BLQ ocean loading
not consumed by current RTKLIB-EX: ATT.OBX
```

ORB, CLK, and BIA must come from a compatible analysis center, solution family,
and solution class. Prefer Final products. Every fallback must be recorded
explicitly; product families must never be silently mixed within a time series.
See [IGS products](https://igs.org/products/) and
[MGEX data and products](https://igs.org/mgex/data-products/).

Use CDDIS as the primary mirror. The downloader should implement bounded
concurrency, a global rate limit, resume, atomic `.part` renaming, size or hash
verification, and a SQLite manifest. BKG, IGN, ESA, and SOPAC are fallbacks only
after inventory proves compatible content. Download one golden week before
mirroring the entire archive interval.

PPP validation gates include:

- adjacent-day coordinate discontinuities and seven-day repeatability;
- forward/backward or independent-window consistency;
- same-receiver, cross-logger solutions over the Era A/B overlap;
- elimination of antenna and receiver metadata, loading, product-family, and
  processing changes before interpreting a receiver-transition step as crustal
  motion.

## 8. MSAS broadcast ionosphere branch

`convbin -s` has been verified to emit PRN 137 MT18 and MT26 from the UBX
samples. The production decoder should preserve and parse original
`RXM-SFRBX` provenance with a state machine:

```text
MT18 IGP mask + IODI
        -> active grid definition
MT26 band/block + matching IODI
        -> GIVD, GIVEI, update time
        -> timestamped current-state snapshots with age
```

Retain native `GIVD_m`, `GIVEI`, usable/don't-use state, and age.
`equivalent_VTEC_TECU` is an explicitly labeled derived field. RTKLIB already
contains MT18/MT26 decoding, IGP tables, 0.125 m delay quantization, and an IODI
check. It can be reused or serve as a regression oracle, but a separate exporter
is needed to produce a complete time series rather than only the correction
state at one instant.

This is an operational integrity-correction field, not high-rate scientific TEC
truth. Comparison maps the station IPP onto the same 350 km shell and retains
MSAS spatial interpolation, GIVE, and age. MSAS should be expected to appear
smoother than 1 Hz carrier dTEC.

## 9. Implementation sequence and acceptance gates

### P0: inventory

- Build a complete archive file manifest with hashes, integrity status, payload
  start/end times, cadence, and message/block inventory.
- Identify Era A overlaps, Era B restarts and gaps, and the 60-day Era A/B
  transition coverage.
- Produce a machine-readable daily availability table.

Gate: every UTC day is traceable to source files or byte ranges, and rerunning
the inventory produces the same result.

### P1: Era C golden day

Choose one complete quiet day and one disturbed day. Use the quiet day first to
complete SBF to 1 Hz RINEX conversion, 30 s reference comparison, RTKLIB
cross-conversion, and a QC report.

Gate: matched epochs have consistent satellite and signal inventories; numeric
differences have fixed tolerances and a complete exception list; no epoch loss
is unexplained.

### P2: relative TEC MVP

Complete arc segmentation, dSTEC, ROT/ROTI, IPP, and one-day plots, then extend
the run to seven days.

Gate: overlapping arcs from different frequency pairs show consistent
disturbance structure. Major events do not appear or disappear solely because
of a reasonable change in elevation cutoff, slip threshold, or detrending
window.

### P3: expand to all 147 Era C days

Generate daily QC, availability, one-minute aggregates, hourly IPP coverage,
and a TID candidate list.

Gate: failed days can be rerun independently, and every result has a provenance
manifest.

### P4: normalize Eras A and B

Complete raw-level stitching, deduplication, and UTC-day splitting, then use the
same RINEX, QC, and TEC interface.

Gate: observations and derived dTEC at common epochs in the Era A/B overlap are
quantitatively consistent. Every gap starts a new arc, with no interpolation
across gaps.

### P5: PPP and absolute TEC

Mirror one product week and complete the RTKLIB static PPP baseline. Then add
code leveling, the DCB or receiver-bias model, and IONEX comparison.

Gate: relative TEC and absolute calibration remain separate data layers. All
PPP product and bias choices are reproducible from the manifest.

### P6: MSAS

Reconstruct the PRN 137 MT18/MT26 state history and compare it with station IPP
and VTEC using quality-aware matching.

Gate: test data covers IODI transitions, don't-use state, stale corrections,
and missing blocks. Stale state must never fill a broadcast gap.

## 10. First implementation slice

The smallest slice with scientific value is:

```text
one complete UTC day in April 2025
  -> canonical 1 Hz RINEX from SBF
  -> receiver 30 s cross-check
  -> GPS L1/L2 and Galileo E1/E5b arc QC
  -> relative dTEC, ROT/ROTI, and 350 km IPP
  -> one machine-readable manifest and one QC summary
```

After it succeeds, expand to seven continuous days. This slice requires neither
new hardware nor an up-front mirror of the full PPP product archive. Broadcast
NAV is sufficient for the initial IPP geometry. It is the shortest path to
determining whether the archive consistently reveals reproducible ionospheric
structure.

## 11. Reference specifications

- [RTKLIB-EX repository](https://github.com/rtklibexplorer/RTKLIB)
- [RINEX 3.04 specification](https://files.igs.org/pub/data/format/rinex304.pdf)
- [RINEX 4.02 specification](https://files.igs.org/pub/data/format/rinex_4.02.pdf)
- [IGS precise and ionosphere products](https://igs.org/products/)
- [IGS MGEX data and products](https://igs.org/mgex/data-products/)
- [IGS Bias and Calibration Committee](https://igs.org/wg/bias-and-calibration/)
- [Septentrio RxTools v25 release notes](https://www.septentrio.com/system/files/support/rxtools_v25.0.0_release_notes.pdf)
