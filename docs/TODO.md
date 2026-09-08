# Raw-observation STEC pipeline implementation plan

This is an agreed design and implementation checklist, not a description of
completed functionality. Unchecked items remain unimplemented or unverified.
Update this document as work lands; move implemented behavior into the relevant
usage/design guides and remove superseded plans rather than retaining history.

## Scope and architecture

The first version produces satellite-bias-corrected, phase-leveled STEC with
receiver bias explicitly uncorrected. No applicable receiver bias calibration
is supplied for these datasets. IONEX is an ionosphere model, not a receiver
calibration table; receiver grade alone does not determine whether a receiver
bias can be estimated. Receiver-bias estimation is outside this first version.

- [ ] Decode UBX/SBF observations with `libcppgnss`, normalize and process them
  in `libneognss-obs`, and use RTKLIB-EX internally for supported calculations.
- [ ] Keep the canonical observation representation independent of RTKLIB's
  structures, satellite numbering and fixed signal slots. Convert only at the
  calculation boundary; report unsupported signals/satellites explicitly.
- [ ] Make RINEX conversion optional for export and independent comparisons,
  not a required internal processing stage. Preserve the existing converters.
- [ ] Process expanded, ordered, nonoverlapping recordings directly without
  requiring reconstruction indexes or QA stamps. Era A still needs overlap
  removal; exclude its unassigned data. Do not repeat full QA in extraction.
- [ ] Keep all observation axes, windows and partitions in GPST. Preserve raw
  archives and avoid a mandatory full observation cache or provenance bundle.

Target flow:

```text
UBX/SBF -> libcppgnss -> canonical observation batches
                    -> libneognss-obs + internal RTKLIB adapter
                       + required predownloaded products
                    -> sample batches and finalized arc solutions
                    -> Python daily Parquet -> plots/maps
```

## 1. Canonical observation batches

One observation represents one receiver/antenna, measurement epoch, satellite
and signal. This is primarily a native in-memory representation, not a
mandatory Python object or persisted observation table.

- [ ] Define epoch association and `gpst_ns` as integer nanoseconds since the
  GPS epoch. Storage resolution is not a claim of measurement accuracy. Do not
  replace RAWX measurement time with NAV time, filename time or snapped seconds.
- [ ] Define receiver and antenna identity, constellation, PRN, precise signal
  identity and actual `frequency_hz`, including GLONASS frequency channel.
- [ ] Map signals to exact standardized observation codes such as `C1C` and
  `C2L` for product lookup. Sharing RINEX terminology does not require RINEX I/O.
- [ ] Preserve pseudorange in meters, carrier phase in cycles, Doppler in Hz,
  and C/N0 in dB-Hz with separate validity indicators. Do not conflate missing
  values with numeric zero.
- [ ] Normalize available lock duration, half-cycle state, loss-of-lock/slip
  indications and measurement uncertainties. Leave unsupported/absent fields
  unavailable rather than manufacturing values.
- [ ] Carry epoch/receiver events such as clock adjustments and observed
  restarts separately from repeated per-signal data. Do not equate every clock
  adjustment with a restart or blindly break all phase arcs.
- [ ] Implement UBX and SBF observation adapters using the generated decoders
  and protocol-specific measurement semantics, including supported SBF block
  variants and their epoch assembly requirements.
- [ ] Preserve all decoded signals before explicit pair selection. Do not
  silently discard measurements to fit RTKLIB frequency slots.

## 2. Required products and station configuration

CDDIS products are downloaded before processing; the solver neither downloads
on demand nor silently switches to broadcast-only or uncorrected processing.
The existing download configuration requests SP3, CLK, ERP, OSB, BRDC, IONEX and
ANTEX. Product availability and actual application are separate concerns:

| Product | Intended role |
| --- | --- |
| SP3 | Precise satellite orbit and line-of-sight/IPP geometry |
| CLK | Compatible satellite clocks for the selected timing/geometry model |
| OSB/DSB | Satellite code-bias correction for the exact selected observables |
| BRDC | Health and other supported auxiliary navigation information; no silent precise-orbit fallback |
| IONEX | Reference/background comparison; no receiver-bias fit in version one |
| ANTEX, ERP | Inputs to explicitly implemented antenna/geometric corrections, not implied corrections merely because files exist |

- [ ] Define station receiver/antenna identifiers, ECEF coordinates and their
  reference point, plus calculation window, signal-pair selection and IPP shell
  height. Do not rely on a RINEX header for station position.
- [ ] Specify the exact required product set and compatible product family/
  reference conventions for the first solver. State which corrections actually
  run and which downloaded inputs are reserved for comparison or later work.
- [ ] Validate product contents, time scales, time coverage and supported
  satellite/signal identities before processing where possible. A matching
  filename or successful decompression does not establish scientific coverage.
- [ ] Preload/cache products across the processing window and its interpolation
  margins. Choose margins from the selected interpolator rather than assuming a
  fixed guard day proves adequate coverage.
- [ ] Keep cache retention separate from validity. Reject invalid/stale data
  and forbidden extrapolation; do not indefinitely extend the last bias record.
- [ ] Implement time-aware bias lookup by satellite/receiver and observable.
  Normalize units, sign, datum and OSB/DSB interpretation at the adapter boundary;
  do not apply both representations of the same correction twice.
- [ ] Require applicable satellite corrections for selected calculations.
  Report any runtime coverage holes explicitly, never substitute zero bias or
  silently select a different signal/product mode. Distinguish unavailable
  corrections from unhealthy satellites and missing observations.
- [ ] Audit the pinned RTKLIB adapter's support and limits, including
  `MAXPRNCMP`, `NFREQ`, `NEXOBS`, bias validity handling and signal mapping.
  Avoid implying that bypassing RINEX removes those limits.

Geometry-free same-epoch combinations cancel common geometry and clock terms.
Precise products support the overall processing model; a precise clock alone
does not calibrate STEC or remove receiver differential code bias.

## 3. Geometry, signal pairs and phase leveling

- [ ] Define explicit supported signal-pair selection with `f1 > f2`; preserve
  actual signal identities instead of hard-coding only generic L1/L2 bands.
- [ ] Use the internal RTKLIB adapter for supported orbit/timing calculations,
  then compute azimuth, elevation, IPP and mapping factor with documented station
  and shell conventions. Keep unsupported combinations explicit.
- [ ] Define and implement the following sign convention, with `L` in cycles,
  `lambda` in meters/cycle and pseudorange `P` in meters:

  ```text
  code_gf_m  = P2 - P1
  phase_gf_m = lambda1 * L1 - lambda2 * L2
  K = 40.3e16 * (1/f2^2 - 1/f1^2)       # meters per TECU
  tecu_per_m = 1 / K
  level_offset_m = weighted_mean(code_gf_corrected_m - phase_gf_m)
  stec_leveled_tecu = (phase_gf_m + level_offset_m) * tecu_per_m
  ```

- [ ] Apply the selected satellite code-bias corrections consistently before
  leveling. Keep the convention for phase offsets and any future phase-bias
  correction explicit; do not silently mix code and phase bias products.
- [ ] Estimate one offset per continuous arc using explicit quality exclusions
  and elevation weights. Specify minimum usable duration/sample count and the
  initial outlier policy; mark insufficiently constrained arcs rather than
  claiming a valid level.
- [ ] Accumulate leveling sample count, weight totals and residual scatter.
  Do not present scatter as a complete absolute TEC uncertainty estimate.
- [ ] Label first-version results `satellite_bias_corrected` and
  `receiver_bias_uncorrected`; do not call them calibrated absolute STEC/VTEC.
  Keep relative phase changes distinct from the code-leveled estimate.

## 4. Continuity and scientific validity

- [ ] Track arcs by receiver, antenna, satellite and exact signal pair.
- [ ] Preserve parser, measurement, product and arc state across batches,
  physical files and GPST midnight. Close final arcs explicitly at stream end.
- [ ] Handle slip indications, half-cycle changes, signal/frequency changes,
  time reversal, observed restart and timeout with explicit reasons.
- [ ] Start with a configurable 50-second phase gap limit, not the current
  dSTEC tracker's fixed 1.5-second assumption. An allowed gap is not proof that
  no slip occurred; do not interpolate missing measurements.
- [ ] Make GF-jump detection sensitive to elapsed time and signal pair rather
  than copying a fixed 0.1-meter threshold. Keep candidates distinct from
  confirmed slips and avoid interpreting all ionospheric variability as slips.
- [ ] Retain a phase arc through temporary code loss when phase remains usable;
  exclude those samples from offset estimation.
- [ ] Keep phase continuity separate from geometric/product availability and
  elevation-based eligibility. Missing products do not prove receiver outage.

## 5. Daily Parquet samples and finalized arc solutions

Full-arc leveling cannot be finalized at the first sample. Store the compact
phase/code combinations separately from arc solutions so raw input needs only
one observation pass, without retaining entire arcs in RAM or rewriting prior
daily sample files when an arc closes.

- [ ] Define a daily GPST `samples` table containing:
  - `gpst_ns`, receiver/antenna, constellation/PRN, exact `signal_pair`, `arc_id`;
  - `phase_gf_m`, nullable `code_gf_corrected_m`, `tecu_per_m`;
  - azimuth/elevation, IPP latitude/longitude and mapping factor;
  - scientific validity flags and leveling eligibility.
- [ ] Define an `arcs` table containing:
  - globally unambiguous within-dataset `arc_id`, signal identity and start/end GPST;
  - nullable finalized `level_offset_m` and explicit solution quality/status;
  - phase/leveling counts, effective duration and residual scatter;
  - boundary reasons and bias-correction status.
- [ ] Partition arc solutions by arc-start GPST day. Provide a reader that
  resolves every arc referenced by requested daily samples, including arcs
  starting before that day or finishing after it. Distinguish unfinished/missing
  solutions from valid finalized solutions.
- [ ] Compute finalized STEC by a batched samples/arcs join, not row-by-row
  Python processing. Do not repeat arc constants in every sample or require a
  second raw scan to apply them.
- [ ] Store only scientific interpretation metadata: GPST epoch/units, signal
  definitions, correction status, shell and calculation settings. Do not add
  transport envelopes, redundant observation copies or provenance bundles.
- [ ] Use bounded Python-side Parquet writers and daily publication. Keep data
  reduction for hourly plots separate from native-rate scientific samples.

## 6. High-level binding and orchestration

- [ ] Expose a high-level processor configured with protocol, station, products,
  window, signal pairs and calculation settings; batch raw input through it.
- [ ] Return columnar sample batches and finalized arc batches, with explicit
  finish and summary operations. Keep RTKLIB structs, indices, allocation and
  low-level calls private to native code.
- [ ] Release the GIL during native work. Avoid per-frame Python calls and
  per-observation JSON/dictionary serialization.
- [ ] Keep file scheduling, Parquet and plotting in Python. Use Click and an
  `ngo-` command name with `--protocol/-p ubx|sbf`; do not invent a public CLI
  contract until its configuration and processing API are implemented.
- [ ] Preserve the shared foreign-protocol warning/skip policy and required
  parsing/scientific checks without adding another full dataset QA pass.
- [ ] Parallelize independent streams and plotting/compression jobs where safe;
  do not split stateful observations arbitrarily into independent hour/day jobs.

## 7. Proportionate implementation checks

These are one-off validation tasks, not authorization to add permanent tests.

- [ ] Inspect small representative UBX and SBF batches against existing
  decoders/converters for time, observables, signal identity and validity.
- [ ] Verify combination signs, dimensions and bias application with inspectable
  numeric examples, including code loss and unavailable receiver calibration.
- [ ] Check cross-file, cross-day and subsecond continuity, arc finalization,
  missing-product errors and explicit unsupported-signal behavior.
- [ ] Compare supported geometry and observables with the existing RINEX route
  on equivalent samples/parameters. Account for converter filtering rather than
  treating RINEX as lossless ground truth.
- [ ] Measure same-input throughput, memory and output volume. Do not promise
  a speedup solely from eliminating RINEX serialization.
- [ ] Verify that downstream readers reconstruct finalized STEC from daily
  samples and cross-day arcs without reopening raw inputs. Update current usage
  documentation only after the corresponding path works.

## Deferred, not first-version requirements

- Receiver-bias estimation/calibration and calibrated absolute TEC.
- Fitting receiver bias against IONEX: any such output must disclose the model
  constraint and cannot use that same GIM as independent validation.
- PPP and additional geophysical/antenna models beyond explicitly implemented
  first-version corrections.
- A persistent full-observation cache, unless measured repeated-decoding cost
  justifies an optional cache later.

## Scientific references

- [ESA: signal combinations and clock/bias definitions](https://gssc.esa.int/navipedia/index.php/Combining_pairs_of_signals_and_clock_definition)
- [IGS: Bias-SINEX format](https://files.igs.org/pub/data/format/sinex_bias_100.pdf)
