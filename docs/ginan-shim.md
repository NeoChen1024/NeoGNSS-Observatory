# Ginan shim: design and progress

Status: selected backend direction; shim and GIM-constrained calibration are
not implemented. Existing PPP/STEC commands still use their current backends.
This document tracks concrete integration work, not compatibility with Ginan's
public API or a promise of independently calibrated absolute STEC.

## Scope and ownership

Use Ginan as the primary candidate for PPP Float and joint ionosphere/receiver
bias research behind [CommonNEX](commonnex/overview.md). Keep upstream types,
global state and configuration out of the Python scientific interface.

- One worker process owns one Ginan context for its lifetime. That context may
  represent a single station or a jointly estimated station network.
- Independent solutions use separate Python worker processes with native
  extensions, not external `pea` workers exchanging files or text.
- Reject a second session in the same process. Do not swap snapshots of globals
  around calls or claim thread-safe, reentrant independent solver instances.
- Prefer fresh spawned workers; do not fork an already initialized Ginan
  context. Sequential context recreation is not promised until reset coverage
  is demonstrated; initially, start a fresh worker for a new solution.
- Keep reusable decoding in libcppgnss, the Ginan adapter/shim and estimator
  orchestration in libneognss-obs, and Parquet/file orchestration in Python.
- Use the [selected Arrow/nanoarrow interop](native-architecture.md#selected-commonnex-interop-design)
  for bulk input/output. A conversion into Ginan's own observation containers
  may copy data; do not promise zero-copy inside an upstream solver.

Ginan compatibility is internal maintenance work: pin its revision and update
the shim alongside any upstream change. The CommonNEX-facing contract stays
independent of Ginan class names and satellite/signal encodings. Initial scope
remains PPP Float, in-scope non-GLONASS systems and existing GPST policy. PPP-AR
is not implicitly enabled by selecting a backend that contains AR code.

## Confirmed upstream boundaries

Inspected source: Ginan v4.1.3, revision
`7baa32a70f819c475b03d0c6834d9a6c90a67c1b`. These findings must be rechecked
when updating it.

- [x] Identify the epoch filter entry: `ppp(Trace&, ReceiverMap&, KFState&,
  KFState&)` in `src/cpp/pea/ppppp.cpp`.
- [x] Identify shared context: global `nav`, `acsConfig`, `epoch` and `tsync`.
  This is not an exhaustive inventory of static caches or side effects.
- [x] Confirm `IONO_STEC` units: the model coefficient is
  `+/- 40.3e16 / frequency_hz^2`; the state is in TECU.
- [x] Confirm receiver `CODE_BIAS` keys distinguish receiver, constellation and
  observation code; model values enter code equations in meters.
- [x] Confirm `clock_codes` can fix reference-system receiver code biases to
  zero and `zero_dcb_codes` generates a near-hard equality constraint between
  two receiver code biases. Neither is a measured calibration.
- [x] Confirm an IONEX model route: `ionoModel()` with
  `TOTAL_ELECTRON_CONTENT` calls `iontec()` in `common/ionModels.cpp`.
- [x] Inspect the distinction between model initialization and constraints:
  `pppIonStec()` obtains a model value, then uses an existing KF state when
  available. With estimation enabled it adds the state to measurement design;
  this does not itself add a recurring GIM residual with GIM uncertainty.
- [x] Locate the external-ionosphere pseudo-observation hook:
  `ionoPseudoObs()` in `pea/ppp_pseudoobs.cpp` currently calls `getSSRIono()`.
  It does not directly consume an IONEX GIM in the inspected path.
- [x] Establish an initial Era C file-boundary smoke result: splitting a
  continuous input preserved printed position results. This does not validate
  the future shim, internal state bit equivalence or absolute calibration.
- [ ] Inventory the complete initialization/preprocessing/filter/finalization
  call sequence and all required persistent objects.
- [ ] Identify a narrow insertion point for project GIM constraints before
  filtering, without duplicating or bypassing required preprocessing.
- [ ] Identify native result/residual/covariance access without parsing TRACE,
  POS, MongoDB output or other serialized solver products.

The previous smoke configuration deliberately used a zero GPS DCB datum.
It demonstrated processing and state availability, not calibrated receiver DCB.
Do not carry those constraints into the calibrated mode without review.

## Scientific gate: GIM-constrained STEC and receiver bias

Target: multi-GNSS STEC tied to an explicit external product datum, with
receiver code-bias estimates and scientifically interpretable uncertainty.
GIM-constrained estimates are not independent absolute calibration.

### Bias reference and observability

- [ ] Write the code/phase equations and rank constraints for each supported
  constellation and exact signal pair, including clock and inter-system bias.
- [ ] Choose clock reference constraints that remove clock/bias degeneracy
  without fixing the receiver DCB being estimated to zero.
- [ ] Remove conflicting `zero_dcb_codes` constraints for calibrated pairs;
  disabling them alone is not enough to make the system observable.
- [ ] Map orbit/clock/code-bias products and GIM to a consistent datum. Existing
  GPS logic aligns MGX within-frequency OSBs with the GIM interfrequency datum;
  do not assume that exact mapping exists for Galileo/BeiDou. See
  [current STEC product interpretation](stec.md).
- [ ] Derive pair-specific DCB from receiver code-bias states with a documented
  sign convention. For code biases entering as additive meters, the proposed
  P2-minus-P1 pair is `b2 - b1`; nanoseconds are `(b2 - b1) / c * 1e9`.
  Verify this end-to-end against product conventions before exporting it.
- [ ] Propagate covariance for a bias difference, including cross-covariance;
  do not treat state marginal standard deviations as independent by default.

### GIM constraint implementation

Proposed observation: `GIM slant TEC = estimated STEC + model error`, using
the same receiver/satellite identity and GPST epoch. This is a soft constraint,
not replacement of measured TEC with a map value.

- [ ] Resolve IONEX time, grid coordinates, shell/mapping function, interpolation
  and RMS conventions explicitly. Model outputs in meters and variances in
  meters squared must be converted to TECU and TECU squared consistently.
- [ ] Add a GIM-backed pseudo-observation through the verified hook, or establish
  an equivalent explicit bias constraint. Do not route GIM through a fake SSR
  payload or assume `corr_mode` alone provides sustained calibration.
- [ ] Specify weighting, a model-error floor and constraint cadence. Repeated
  samples from one coarse GIM cell/time are correlated, not independent truth
  at every high-rate epoch.
- [ ] Require valid map/bias/time coverage. No nearest/stale map or zero-bias
  substitution when required calibration information is missing.
- [ ] Define what remains available during a product gap and how calibration
  age/uncertainty evolves. Preserve usable observations and estimator continuity
  where justified; do not label an unanchored state freshly calibrated.
- [ ] Expose datum, signal pair, constraint source, estimation window and quality
  with outputs. Negative estimates remain numerical values with quality, not
  values clipped to a plotting range.

## Shim contract and lifecycle

Conceptual operations, not finalized method signatures:

| Operation | Contract |
| --- | --- |
| initialize | Configure the one process context, station metadata, products and estimation policy |
| process | Consume bounded completed-epoch batches, run required preprocessing/model/filter steps, return typed results |
| update_products | Refresh product coverage without recreating the estimator or resetting arcs |
| finish | Finalize once and return remaining results; not called at a file/day boundary |

- [ ] Enforce lifecycle and single-session ownership with explicit errors.
- [ ] Preserve full epoch semantics, exact signal identities, code/phase units,
  nullability and correction-application state during input mapping.
- [ ] Keep partial epochs buffered under the agreed completion contract; batch
  delivery is not an instruction to finish scientific state.
- [ ] Retain required history but release obsolete observations/products so
  live or long Era processing has bounded storage where the estimator allows.
- [ ] Release the GIL during native work; retain Arrow owners for the duration
  of access and export immutable owned result buffers.
- [ ] Separate current measurement updates from retained/predicted states.
  A state in KFState is not proof of a usable observation at the current epoch.
- [ ] Export position/reference point, clock, residuals and relevant quality;
  export STEC, bias and covariance with explicit units and calibration status.
- [ ] Disable unwanted upstream file/database outputs and automatic downloads.
  Keep necessary structured diagnostics and throttled warnings.
- [ ] Translate failures without terminating the Python host; check actual
  solution availability, not just successful return from a top-level call.
- [ ] Define which failures end the worker and which permit continuation.

## Dependency and update work

- [ ] Add the chosen Ginan revision as a submodule when implementation starts;
  it is not currently a repository dependency.
- [ ] Build the required native core with its dependency/license notices;
  exclude unrelated applications where practical. Verify the local build rather
  than treating an executable smoke run as library-build validation.
- [ ] Keep minimal upstream patches separate from the project shim. Prefer
  exposing the needed hook over refactoring all upstream global state.
- [ ] At each revision update, inspect schema/model/constraint and lifecycle
  changes as well as compiler errors; update the shim in the same change.
- [ ] Preserve the current backend until replacement behavior is demonstrated.
  Do not maintain old Ginan APIs solely for compatibility.

## Era C validation sequence

Use representative Era C observations and already downloaded products read-only.
Temporary external RINEX comparisons are allowed; the final shim input remains
CommonNEX rather than a required RINEX file or external solver process.

- [ ] Establish matched signal selection, marker/ARP, antenna, tides/attitude,
  product and outlier models before comparing numerical accuracy.
- [ ] Verify continuous versus differently batched native input, then a GPST
  midnight boundary, without artificial filter reinitialization.
- [ ] Compare direct observations and ParquetNEX replay once those adapters exist.
- [ ] Inspect design rank and the fitted biases under each declared datum.
- [ ] Apply a controlled known code-bias perturbation to a disposable input
  copy; confirm expected bias response and calibrated STEC behavior. Leave
  preservation inputs untouched.
- [ ] Examine withheld arcs/time ranges, residuals, GIM sensitivity, covariance
  and inter-day receiver bias stability; do not infer accuracy from smooth plots.
- [ ] Exercise missing-product and unresolved-time cases, including recovery.
- [ ] Measure throughput and peak memory separately from verbose trace output.

Use proportionate one-off validation, not a new permanent test framework.
Keep run logs and experimental statistics in work outputs; this document tracks
current decisions, evidence boundaries and remaining work.

## Source references

- [Epoch filter and pseudo-observation dispatch](https://github.com/GeoscienceAustralia/ginan/blob/7baa32a70f819c475b03d0c6834d9a6c90a67c1b/src/cpp/pea/ppppp.cpp).
- [STEC and receiver code-bias measurement models](https://github.com/GeoscienceAustralia/ginan/blob/7baa32a70f819c475b03d0c6834d9a6c90a67c1b/src/cpp/pea/ppp_obs.cpp).
- [External ionosphere and zero-DCB constraints](https://github.com/GeoscienceAustralia/ginan/blob/7baa32a70f819c475b03d0c6834d9a6c90a67c1b/src/cpp/pea/ppp_pseudoobs.cpp).
- [IONEX model route](https://github.com/GeoscienceAustralia/ginan/blob/7baa32a70f819c475b03d0c6834d9a6c90a67c1b/src/cpp/common/ionModels.cpp).
