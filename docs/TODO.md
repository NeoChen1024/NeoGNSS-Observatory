# Project implementation roadmap

This checklist contains remaining implementation and scientific validation work.
Current behavior belongs in the linked tool and format guides; completed tasks
and superseded designs are removed rather than retained as history.

Detailed owning checklists:

- [CommonNEX](commonnex/TODO.md): remaining receiver mappings, input validation,
  consumer integration and explicitly deferred acquisition work.
- [Broadcast decoding](broadcast-message-decoder.md#remaining-work): remaining
  assembly, applicability and independent field validation.
- [BeiDou](broadcast-beidou.md#remaining-scope) and
  [SBAS](broadcast-sbas.md#remaining-downstream-work): family-specific extensions.
- [Ginan shim](ginan-shim.md): selected backend integration direction, lifecycle,
  observability, GIM constraints and validation; the shim is not implemented.

## STEC models and scientific validation

[Offline multi-GNSS STEC](stec.md) already implements exact-pair processing,
receiver antenna correction, phase-arc continuity, leveling, receiver DCB fits,
incremental publication and calibrated-family fusion.
[Realtime relative STEC](stec-realtime.md) shares the phase engine but uses
broadcast geometry and receiver-exported phase, without absolute calibration.

- [ ] Add satellite antenna phase corrections and phase wind-up to offline
  STEC, with explicit signal/model coverage and independent sign/unit checks.
  Receiver PCO/PCV is already implemented; realtime correction policy requires
  a separate decision rather than silently changing its raw-phase contract.
- [ ] Validate STEC and receiver DCB against independent references and examine
  sensitivity to mapping height, elevation masks, leveling scatter, fit-window
  boundaries, antenna approximations and receiver temperature. The constraining
  IONEX GIM cannot serve as an independent absolute-calibration reference.
- [ ] Evaluate pair-specific GF-jump thresholds under gaps and rapid ionospheric
  variation. The current detector already includes elapsed-time dependence;
  candidates must remain distinct from confirmed slips.
- [ ] Independently check multi-GNSS geometry, exact-code bias/datum transfer
  and product-gap behavior against equivalent reference calculations, including
  the pinned backend's satellite/signal limits and unsupported combinations.
- [ ] Validate throughput and peak memory on long multi-GNSS intervals,
  including long open phase arcs and incremental append/rebuild processing.
- [ ] Investigate independent absolute calibration and time-varying receiver
  bias models separately from the existing GIM-constrained estimates.

## PPP Float extensions

[GPS static forward Float PPP](ppp.md) already consumes raw UBX/SBF without a
RINEX intermediate, maintains state across files/days, checks local product
coverage, applies antenna models and exports numerical tables, PNGs and a PDF.
The [shared antenna model](antenna.md) owns implemented calibration selection
and approximation rules.

- [ ] Extend Float PPP beyond GPS L1/L2, with exact signal mapping, justified
  constellation clock/bias datums, compatible products and antenna coverage.
  Check backend satellite/frequency limits before exposing supported modes.
- [ ] Export remaining diagnostics: ambiguity arcs/reset reasons, tracked versus
  selected/used/rejected observation counts, available rejection diagnostics,
  coordinate epoch and separate hydrostatic/wet troposphere interpretation.
  Preserve the current ionosphere-free residual identity and unavailable states.
- [ ] Extend plots with tracked/used distinctions, initial-convergence detail,
  ambiguity-reset statistics and supported ambiguity timelines. Float processing
  must not display invented fixed percentages or reference-ambiguity states.
- [ ] Add explicit NGS ANTEX preparation/download configuration alongside the
  existing local multi-catalog lookup; keep acquisition separate from solving.
- [ ] Compare equivalent observations, products, antenna/reference-point and
  model settings with an independent positioning solution such as CSRS-PPP.
  Similar plots or formal covariance do not establish absolute accuracy.
- [ ] Validate long-interval throughput, memory and scientific continuity under
  product-window changes, calibration changes and actual receiver outages.

Kinematic PPP, backward/combined solutions and PPP-AR remain deferred pending
an explicit scope decision and compatible backend/product support. Ginan
integration does not implicitly enable these modes.

## SBAS source combination

[Grid snapshots](subframes.md) and their live/offline adapters share decoding,
aging, restart rules and per-source statistics. Provider selection is already
shared by historical plots and live displays.

- [ ] Investigate optional source weighting, including spatial alignment,
  uncertainty interpretation and correlated broadcasts. GIVEI is not a linear
  weight; multiple satellites from one provider are not independent evidence.

## Regional Network RTK and VRS

Planned research, not implemented positioning or correction services. Start
with a small regional relative Network RTK solver, then evaluate virtual
reference station (VRS) synthesis and rover delivery. Solver/backend selection
and synthesis equations remain open; neither multi-frequency processing nor
independent implementation establishes freedom to operate.

Use [CommonNEX](commonnex/overview.md) UBX/SBF observations for historical replay
and live processing. Initial signal scope is GPS and Galileo, followed by
BeiDou and QZSS with independently verified signal coverage. Preserve SBAS
observations but defer their use in network ambiguity resolution and synthesis.
GLONASS and NavIC scientific processing remain out of scope.

### Observation model and relative solver

- [ ] Inventory station geometry, coordinate frame/epoch and reference point,
  antenna calibration, measurement cadence and actual common signal coverage.
  Enabled receiver signals are not evidence of simultaneous usable observations.
- [ ] Define uncombined code/phase equations, clock and signal-bias datums,
  estimable parameters and rank constraints. Support varying frequency counts;
  establish integer ambiguity resolution in a justified relative subspace
  rather than fixing every undifferenced phase state independently.
- [ ] Build a two-station GPS dual-frequency baseline, then add Galileo and
  further supported systems/frequencies. Evaluate WL/EWL combinations as
  optional search/validation tools without making a satellite MW-bias or
  phase-leveled clock product a prerequisite.
- [ ] Validate against known baselines and an independent positioning solution,
  including incorrect fixes, code/phase loss, half-cycle changes, clock
  adjustments and discontinuities across batches, files and GPST midnight.

### Regional correction estimation

- [ ] Estimate relative ionospheric and tropospheric residuals on a small
  station network. Define spatial support, uncertainty and behavior outside
  supported geometry; do not average raw carrier phases across receivers.
- [ ] With sufficient station coverage, withhold a station from estimation
  and inspect positioning errors and phase residuals there. Existing
  GIM-constrained STEC is auxiliary evidence, not an independently calibrated
  centimeter-level phase correction or validation reference.

### VRS synthesis and patent review

- [ ] Compare candidate estimator, synthesis and delivery operations against
  relevant independent claims and patent families for intended deployment
  jurisdictions, including the United States and Taiwan. Verify official legal
  status and review continuation applications before treating a design as
  cleared. Independent code, CommonNEX input and three frequencies do not by
  themselves avoid method or apparatus claims.
- [ ] Prioritize synthesis review of [Swift US12216211B2](https://patents.google.com/patent/US12216211B2/en)
  and its [continuation application](https://patents.google.com/patent/US20250130332A1/en),
  [Trimble US9594168B2](https://patents.google.com/patent/US9594168B2/en)
  and [Sejong US12332361B2](https://patents.google.com/patent/US12332361B2/en).
  Also review network ambiguity/bias estimation, atmospheric representation
  and delivery claims; this starting list is not an exhaustive clearance.
- [ ] Define synthetic code/phase equations, integer-consistent phase datum,
  antenna/reference-point treatment, ephemeris/correction conventions and
  continuity/reset behavior before finalizing the synthesizer. Keep synthesis
  separate from the network estimator and output codec.

### Live delivery and rover validation

- [ ] Adapt [CommonNEX live delivery](commonnex/live.md) for per-epoch processing
  with bounded multi-station alignment, waiting deadlines and explicit late or
  missing-station behavior. Preserve equivalent scientific rules in replay.
- [ ] Add RTCM observation encoding and required station/navigation context
  after the synthesis contract is established. Initially evaluate 1 Hz output
  with a virtual location fixed for each session; defer cross-region switching,
  static VRS grids and large-scale distribution architectures.
- [ ] Validate with real rover receivers and an independent coordinate
  reference. Measure time to fix, position error, incorrect-fix frequency,
  correction age, latency and outage behavior; successful encoding or a FIX
  status alone does not establish accuracy.
