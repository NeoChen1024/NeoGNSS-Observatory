# Multi-GNSS STEC and receiver DCB

`ngo-stec` reads CommonNEX observations, selects L1-anchored dual-frequency
pairs, levels carrier phase to satellite-bias-corrected code, and estimates
pair-specific effective receiver biases against CODE GIM. It publishes both
pair results and equal-arithmetic-mean fused STEC. These are GIM-constrained,
product-dependent estimates, not independently calibrated absolute TEC.
PPP solutions, RINEX observation conversion and QA stamps are not required.

## Run and automatic pairs

Use [the example configuration](../config/stec.example.toml):

```sh
ngo-stec --input-dir /data/commonnex-station \
  --config config/stec.example.toml --output work/stec
```

Paths in the configuration are relative to that file. Station identity and ARP
ECEF position come from Setup marker XYZ plus NEU offset, unless explicitly
supplied. `--start` and `--end` select inclusive/exclusive GPST dates. Payload
GPST determines time, never a filename. GLONASS, NavIC and SBAS observations
are not processed by this estimator.

The observation progress bar counts input signal-observation rows, including
rows that signal selection later excludes, rather than compressed file bytes,
epochs or emitted STEC samples. It covers new input parts only; subsequent
receiver DCB and fusion stages are separate work.

Do not configure `signal1`/`signal2`; these obsolete settings are rejected.
The following families are automatically considered on each satellite:

| System | Families | Preferred exact codes |
| --- | --- | --- |
| GPS | L1/L2, L1/L5 | 1C/2L, 1C/5Q |
| QZSS | L1/L2, L1/L5 | 1C/2L, 1C/5Q; 1E is a separate alternative |
| Galileo | E1/E5b, E1/E5a | 1C/7Q, 1C/5Q |
| BeiDou | B1C/B2b, B1C/B2a | 1P/7D, 1P/5P |
| BeiDou legacy | B1I/B2I | 2I/7I; independent of the B1C group |

[The fixed code-priority table](../python-src/neognss_observatory/stec_pairs.py)
is the source of truth for alternatives. No L2/L5, same-frequency, cross-satellite
or B1I/B2a combinations are formed. At most one exact-code pair per family is
selected at an epoch. Prefer valid code/phase with applicable satellite bias;
retain an active covered pair rather than switching back to a higher-priority
code. If no covered alternative exists, retain usable phase for relative TEC.
Changing exact codes closes the previous arc. Observation row order does not
set priority. Data, pilot and combined-code alternatives never multiply the
fusion weight of one family.

## Time and continuity

Input observations and local products must remain unchanged during a run.
Processing preserves canonical system, satellite and signal identities.

GPST decimal seconds become integer nanoseconds with round-half-to-even.
Distinct epochs that collide after rounding and backward time are errors.
Raw lock, half-cycle and continuity-counter evidence is preserved. Samples use
one common emission cadence across all families, so their epochs can be joined.
File, batch and GPST-day boundaries do not reset arcs. Explicit half-cycle,
lock/counter changes, conservative GF jumps, clock resets, calibration-record
changes, signal switches and timeout close arcs. GF jumps are candidates, not
proven slips. Raw GF still drives continuity when geometry or products are missing.

Receiver phase PCO/PCV uses the [shared antenna model](antenna.md). Missing
frequency calibration disables calibrated phase for that family, not the entire
station; raw relative TEC remains available. Malformed or ambiguous calibration
is an error. `receiver_antenna = "none"` explicitly permits uncorrected research
processing, recorded in metadata. The optional extrapolation policy can enable
L5 when only L1/L2 calibration exists. Satellite antenna phase corrections and
phase wind-up are not applied in this STEC path. Receiver smoothing is reported,
not undone. Observation completion Events must describe complete contexts;
unsupported stream events are not silently ignored.

### Receiver restart Events

Timed CommonNEX `RECEIVER/RECEIVER_RESTART` Events with inferred
`UPTIME_DECREASE` or `GPST_UPTIME_OFFSET_JUMP` evidence are supported. Before
the first observation epoch at or after the Event GPST, all active pair arcs
close with reason 11 (`receiver_restart`). Relative phase and leveling restart,
and a new common emission cadence begins. The timestamp identifies restart
evidence, not necessarily the physical boot instant. No observation is invented
at that time.

Receiver DCB fits are independent within each intersection of a nominal fit
window and a receiver segment. No fit or fallback bias crosses a restart;
each segment must meet the usual coverage gates independently. Samples, arcs,
bias fits and fused rows carry `receiver_segment_start_ns`. Zero denotes the
initial processing context, not a known boot time. Fusion segments also change
at receiver boundaries, including intervals without calibrated contributors.

Event-only partitions are retained. Future boundaries stay pending in native
checkpoints until observations reach them; duplicate timed evidence is
idempotent. Restart GPST uses the same nanosecond rounding as observations,
and distinct restart times colliding after rounding are rejected. Untimed
restart evidence cannot be safely placed and fails with its input path. A newly
discovered restart at or before already processed observations requires
`--rebuild`; it is never silently applied to a later epoch.

## Products and bias datum

The local resolver uses COD0MGXFIN SP3/CLK and BRDC00IGS_R navigation, plus
COD0OPSFIN IONEX. `bias_product_family` selects one daily code-bias family
(default `COD0MGXFIN`). It reads that family's `*_OSB.BIA[.gz]` or
`*_DCB.BSX[.gz]` files under `products_root`. No automatic download, producer
fallback or cross-family signal splicing occurs. Only complete GPST Bias-SINEX
with bounded validity, nanosecond satellite code biases and explicit observable
identities is accepted. OSBs require ABSOLUTE BIAS_MODE; DSBs give differences.
Missing files and per-pair satellite coverage are reported. Populating a
calibration family does not establish that its exact codes exist in a product.

For `P_corrected = P - b`, graph edges represent `b(second)-b(first)`.
OSB differences and connected DSB paths can supply this difference. Duplicate
or overlapping solutions and inconsistent graph constraints are rejected.
No C7I/C7D or pilot/combined code equivalence is inferred from shared frequency.

CODE IONEX reference satellite DCBs are GPS C1W-C2W and Galileo C1X-C5X.
Let `B_t` be the selected product's second-minus-first bias for the target pair,
`B_r` its second-minus-first reference bias, and `D_r` the IONEX
first-minus-second DCB. The applied target bias is:

```text
K = 40.3e16 * (1/f_low^2 - 1/f_high^2)     # meters / TECU
B_applied = B_t - (K_target / K_reference) * (B_r + D_r)
```

This reduces to the previous GPS within-frequency transfer for L1/L2. The
frequency ratio is essential when transferring to another pair. The procedure
assumes compatible ionospheric bias conventions; sharing a producer alone is
not proof of perfect consistency. Both product/reference intervals must apply.
IONEX UT validity and map epochs are normalized to GPST once in native code.

For BeiDou/QZSS, where this IONEX route supplies no reference satellite DCB,
use the selected product's pair difference and estimate a separate effective
receiver bias against GIM. Its datum remains explicitly product-dependent.
No GPS receiver DCB is reused across systems or codes. Both members of a fusion
group must use the same declared bias-datum policy.

## Geometry, leveling and availability

Precise satellite centre-of-mass geometry iterates emission time and rotates
transmit ECEF to reception ECEF. There is no broadcast orbit or SP3-clock
fallback. The orbit stencil requires consecutive 5-minute epochs, precise CLK
samples must bracket transmission, and broadcast health must be usable.
The current health selection conservatively limits ephemeris age to two hours.

CODE IPPs intersect the geocentric 6821 km shell. GIM uses strict spatial
bilinear and sun-fixed hourly interpolation; missing cells/maps are not filled.
The separate MSLM mapping uses geodetic zenith angle `z`:
`1/sqrt(1 - (6371/(6371+H) * sin(0.9782*z))^2)`, default `H=506.7 km`.
GIM interpolation, mapping assumptions and local departures remain errors.

For each exact pair and satellite arc:

```text
raw_phase_gf_m = lambda_high * L_high - lambda_low * L_low
relative_stec_tecu = (raw_phase_gf_m - first_raw_phase_gf_m_in_arc) / K
phase_gf_m = raw_phase_gf_m - receiver_phase_correction_high + receiver_phase_correction_low
code_gf_corrected_m = P_low - P_high - B_applied
level_offset_m = robust_weighted_mean(code_gf_corrected_m - phase_gf_m)
stec_leveled_tecu = (phase_gf_m + level_offset_m) / K
stec_absolute_tecu = stec_leveled_tecu - receiver_bias_tecu
```

Relative TEC is uncalibrated and arc-relative, not an absolute estimate.
Leveling uses Huber location with sine-squared elevation weights, at least
20 samples over 600 s above 30 degrees by default. Receiver DCB is estimated
independently per system/exact pair/window using satellite arcs jointly, not an
independent zero point per satellite. The defaults retain 24-hour windows,
5-minute bins, equal total arc weights, a 2 TECU GIM RMS floor, and quality
requirements of six hours, four satellites, six arcs, three azimuth quadrants,
20 degrees elevation span and at most 15 TECU residual scatter. Sparse QZSS
coverage may legitimately fail these gates; they are not automatically lowered.
Receiver DCB nanoseconds use each pair's own K and the low-minus-high sign.

Issue bits are 1 orbit, 2 clock, 4 health, 8 satellite bias, 16 GIM, and
32 receiver antenna model/direction unavailable. They describe checks reached,
not an exhaustive missing-dependency inventory. Raw relative phase can survive
these gaps. Unavailable dependent fields stay NaN/null. Only samples with all
required products, finite leveling and an accepted receiver bias become usable
absolute estimates. A partial run never fabricates missing corrections.

## Equal-mean fusion and outputs

Within a satellite/epoch/fusion group, average the two eligible calibrated
families with weights 0.5/0.5. One eligible family has weight 1; none gives null.
B1I/B2I is its own group and never adds a third vote to B1C fusion. Preserve
negative estimates. There is no geometric mean, temporal smoothing, fabricated
fill, forced offset alignment or arbitrary difference-rejection threshold.

The fused table retains the signed pair difference (family-ID order), exact
pair IDs, arc/window IDs, weights and a consistency status indicating that the
difference is reported without a validated rejection threshold. It does not
claim that the pairs agree or infer uncertainty from their count. Shared L1,
GIM errors and approximate antenna corrections make the estimates correlated.
Combination, arc/window and gap changes start a new fusion segment; ROT/ROTI
consumers must respect those boundaries.

- `samples/GPST-*.parquet`: per-pair raw/receiver-corrected phase GF, relative TEC,
  corrected code GF, canonical satellite identity, geometry and GIM.
- `arcs.parquet`: pair-specific leveling, validity and reasons (8 calibration
  change, 9 exact signal switch, 10 receiver clock reset, 11 receiver restart).
- `receiver_bias.parquet`: exact-pair window fits, candidate/accepted biases,
  scatter, datum and coverage. Failed fits retain diagnostics and null bias.
- `fused/GPST-*.parquet`: calibrated equal-mean output, contributors, weights,
  difference, status, provisional flag and `segment_start_gpst_ns`.
- `read_stec()` exposes finalized pair estimates using published tables only;
  `read_fused()` reads the published fusion product.
- `summary.json` and Parquet metadata carry scientific policies and quality.
  `state.json` is the incremental continuation checkpoint.

Incompatible outputs require explicit `--rebuild`; no automatic rewriting
occurs. Configuration, antenna-model or previously consumed input changes
require rebuilding. Additional
parts/days preserve native state and refit only affected windows; fusion and plot
day generations are updated for affected days. Changes to already-used external
products require an explicit rebuild as well. Products and observations are
always read-only; derived outputs publish by rename with the previous result
retained as a backup.

`ngo-stec-plot` renders hourly fused multi-GNSS IPP tracks from published tables.
It labels satellite system and BeiDou family, does not join across fusion
segments, and preserves provisional/negative/out-of-range values. Natural Earth
10m coastline is bundled; `--coastline PATH` overrides it. Incremental plots
identify the coastline ZIP by SHA-512, not its path or modification time.
Older plot states using path-based identity require `--rebuild` once.
The optional SBAS grid remains a separate input; SBAS background VTEC is not
the STEC estimate. Use `--rebuild` to regenerate old GPS-only plot outputs.

References: [Bias-SINEX](https://files.igs.org/pub/data/format/sinex_bias_100.pdf),
[IONEX](https://files.igs.org/pub/data/format/ionex1.pdf).
