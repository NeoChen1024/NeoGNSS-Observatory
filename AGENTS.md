# AGENTS.md

## Project language

- Write project files in English, including documentation, source code,
  comments, identifiers, configuration keys, and test names.
- Preserve standardized GNSS terminology, protocol field names, receiver
  message names, filenames, and externally defined identifiers exactly as
  specified by their source standards.
- Existing non-English text should be translated when the surrounding file is
  modified, unless it is quoted source material or test data whose exact bytes
  are significant.
- This language policy applies only to files in the repository. It does not
  constrain communication with users; use the language appropriate for the
  current conversation.

## Development stage and scope

- This is a pre-Alpha research project. Algorithms, CLIs, intermediate schemas,
  and output layouts are experimental and may change freely.
- Prioritize clear scientific computation and fast iteration over compatibility,
  generalized infrastructure, and production-grade artifact management.
- Do not add or expand automated tests unless explicitly requested by the user.
  A feature or bug fix does not implicitly authorize new regression tests.
- Validate changes proportionately using builds, small representative inputs,
  and direct inspection of results. Report what was actually checked. Do not
  turn one-off validation into a new permanent test framework.
- Do not preserve obsolete interfaces or output formats solely to satisfy tests.
- Do not introduce provenance bundles, source/binary snapshots, dependency
  inventories, or chains of artifact hashes unless explicitly requested.
- Retain metadata needed to interpret or process data correctly: time scales,
  units, signal identities, validity flags, calculation parameters, and necessary
  source offset mappings. Keep reconstruction overlap proofs and downloader
  recovery/integrity checks that serve an actual processing purpose.
- Preserve raw archives and basic protection against accidental data loss.
- Experimental analysis outputs should be directly readable without provenance
  sidecars. Publish finished files or directories by rename; allow explicit
  overwrite without silently deleting unrelated files. Do not build a generic
  resume system; retain useful existing reuse paths.

## Documentation audience

- Keep `docs/` focused on the current implementation. Remove superseded
  workflows, migration logs, obsolete pilot paths and run statistics instead
  of preserving them as historical sections. Git history is the archive.
  Retain concise dataset/converter constraints only when they materially affect
  present behavior or scientific interpretation, and distinguish implemented
  features from research extension points.

- README files are public-facing documentation. Keep them focused on purpose,
  requirements, installation, building, APIs, usage, and limitations.
- Keep local development-environment instructions here, not in README files.
  This includes this workspace's uv-managed environment, machine-specific
  dataset paths, and references to sibling repositories.

## Data handling

- Treat GNSS archives under `/hdd` as read-only preservation masters.
- The user-authorized reconstruction outputs are
  `~/net/DATA_SSD/datasets/GNSS/era-a` and `era-b` beside it.
  The expanded source directories remain read-only.
- For downstream Era A analysis, use the reconstructed GPST segments in
  `era-a/` and exclude `era-a/unassigned/`. Leave excluded files and their
  provenance intact; do not invent missing GPST assignments.
- Read Era A-C processing inputs from `~/net/DATA_SSD/datasets/GNSS`:
  Era A uses `archive/ubx24h`, Era B uses `archive/ubx`, and Era C uses
  `gnss/mosaic-x5/BX4ACP`. Treat this expanded dataset as read-only too.
- Process the already expanded `.ubx`, `.25_`, `.25o`, and `.25p` files directly;
  do not decompress XZ during processing or silently fall back to `/hdd`.
  Ignore retained `.xz` copies and report missing expanded inputs explicitly.
- Keep generated and large observation products out of Git.
- Experimental artifacts need sufficient scientific metadata for interpretation,
  not a complete execution-environment snapshot.
- Never infer observation coverage or time scale solely from filenames; inspect
  payload timestamps and preserve original GNSS fields as provenance.

## Single GPST policy

- All project observation time axes, day/hour partitions, processing windows,
  map labels and derived artifact metadata use GPST. No optional UTC mode or
  compatibility reading of old UTC-derived products is maintained.
- Scalar `gpst`, `start_gpst`, `end_gpst` and `hour_gpst` values are continuous
  seconds since 1980-01-06 00:00:00 GPST, not Unix timestamps. Archive indexes
  use integer `gpst_ms`; receiver-clock samples use `gpst_ns`. Raw fractional
  time remains intact. Do not snap navigation epochs to whole seconds.
- Name assigned UBX outputs `GPST-%Y-%m-%d--%H-%M-%S-mmm.ubx`, where `mmm`
  is exactly three millisecond digits (including `000`). Never use a UTC `Z`
  or `+0000` suffix for GPST. Keep native protocol fields and external product
  formats unchanged; decode their specified scales at the input boundary.
- Logger epochs are buffered until EOE. Every EOE requires a fresh valid
  NAV-TIMEGPS with matching iTOW; otherwise fail. Rotate before writing the
  complete first epoch of the new GPST day, including its EOE.
- Re-stitch splits at GPST midnight or a NAV-to-NAV interval greater than the
  configured timeout (default 50 seconds). A segment does not imply gapless
  sampling. Preserve subsecond epochs and do not fabricate missing observations.
- Old UTC analysis/reconstruction outputs were explicitly cleared. Preserve
  archives, downloaded products, downloader plans/configuration and map assets.
  Rebuild derived outputs with GPST tools; do not relabel old timestamps.

## Toolchain

- This workspace has Septentrio RxTools under `~/.local/RxTools`; its SBF to
  RINEX converter is `bin/sbf2rin`. Read the installed tool's help and record
  its version when investigating converter behavior. Do not vendor the installation.
- References to RTKLIB mean the RTKLIB-EX `main` branch from
  `rtklibexplorer/RTKLIB`, not upstream `tomojitakasu/RTKLIB`.
- Pin exact tool revisions or container digests when production processing is
  explicitly requested, not for every exploratory run.
- Preserve original observation codes and receiver message provenance during
  normalization.

## Python scripts

The library's build-time generator lives in `libcppgnss/scripts/`; Python
regression tests may live beside the C++ library. The generator uses Click;
tests are run through unittest discovery rather than custom Python CLIs.
Use the repository-root `./.venv`, managed by uv, for code generation and
all Python tests. Install dependencies with `uv pip install --python
.venv/bin/python ...`; do not create per-component environments or fall
back to system Python.

Local setup commands (create `.venv` only if it does not already exist):

```sh
uv venv .venv
uv pip install --python .venv/bin/python -r libcppgnss/requirements-codegen.txt
uv pip install --python .venv/bin/python -e .
uv pip install --python .venv/bin/python pre-commit
.venv/bin/pre-commit install
.venv/bin/pre-commit run --all-files
```

The current CMake configuration selects the repository-root
`./.venv/bin/python` for code generation and Python tests, including when
configuring `libcppgnss/` directly.

- Keep Python code under `python-src/neognss_observatory/`. Use setuptools with
  `pyproject.toml`; maintain runtime dependencies in `requirements.txt` as the
  single source consumed by the package metadata.
- Target Python 3.11 or newer. Format with Black (line length 132, Python 3.11
  target) and isort (Black profile), using the pinned pre-commit hooks.
- Use Click for CLI parsing in every Python script. CLI modules should expose
  a `cli` callable and use an `if __name__ == "__main__": cli()` entry point.
  Use `#!/usr/bin/env python` for directly executable scripts.
- For tasks where presentation matters and status changes at a moderate rate
  (such as downloads), prefer Rich throughout for user-facing messages and
  `rich.progress` for progress reporting.
- For processing or computation scripts where throughput matters, prefer
  direct use of `tqdm` for progress reporting. Keep terminal updates out of
  performance-critical inner loops or let `tqdm` throttle their refresh rate.
- Keep machine-readable output separate from status messages; send progress
  and diagnostics to stderr when stdout carries data.
- Declare Python dependencies with minimum versions (`>=`) in
  `requirements.txt`, allowing upgrades without a lockfile. Do not automatically
  inventory installed versions for experimental runs.

## Native library boundaries

- `ngo-dataset-qa` defaults to optional, read-only `scan`; `--profile restitch`
  explicitly enables overlap indexes/proofs and reconstruction. QA is not a
  prerequisite enforced by extraction. Do not require QA stamps, manifests,
  or reconstruction indexes in downstream readers, and do not rerun complete QA
  there. Share epoch/field interpretation in native code; retain necessary
  parser bounds, usable time and scientific validity checks.
- New project CLI names use the `ngo-` prefix. Existing other entry points are
  migrated separately, not renamed implicitly during unrelated changes.

- Raw processing CLIs expose `--protocol/-p ubx|sbf` (default `ubx`). Validate
  complete foreign-protocol frames before skipping them atomically; report
  throttled warnings on stderr and retain skipped frame/byte counts. Never
  parse an embedded sync sequence inside a valid foreign frame. Unsupported
  analysis/protocol combinations must fail explicitly before producing outputs.

- `libcppgnss/` contains reusable UBX/SBF framing, generated protocol decoders,
  signal routing, SBAS L1 decoding, and standard IGP coordinate/ordinal rules.
  Keep transport, recording policy and automatic terminal output out of the library.
- `libneognss-obs/` contains Observatory-specific epoch association, archive
  segmentation, receiver-clock state, SBAS mask/aging policy, and batch bindings.
  It depends on `libcppgnss`, never the reverse.
- Python calls the native extension directly. Do not resurrect archive-index,
  clock-scan, subframe-export or inspection worker executables as alternate backends.
  Keep the actual `neoubxlogger` application and its CLI.
- Keep file orchestration, Parquet, plotting and external converter invocation
  in Python. Batch data across the binding; do not call Python once per raw frame.
  Release the GIL during native processing. Batch/file/day boundaries must not
  reset state; use explicit timeout/restart/continuous-group policies instead.
- Generate every available pinned pysbf2 block schema. Keep unknown or empty
  definitions explicit and preserve raw bytes; schema coverage is not proof of
  receiver-firmware/revision or scientific validation.
- Keep JSON serialization primarily in Python; `contrib/json` is available to
  native processing without introducing C++ Parquet or plotting dependencies.
- Grid processing consumes protocol-neutral timed SBAS records, not UBX wire
  identifiers. Use `constellation`, `prn`, and `signal` in derived grid and
  hourly products. Persist only SBAS's 250-bit body, normalized GPST and signal
  identity, validity and generic continuity/end information in frame Parquet.
  Do not persist UBX/SBF envelopes, field dictionaries or source offsets in
  SBAS intermediate products; raw archives preserve those. Resolve protocol
  timestamps at extraction, never by reopening raw data in grid processing.
  Preserve the distinction between navigation epoch context and receiver message time. Do not
  reset signal state solely at a physical file or GPST day boundary.
- Bump the native archive-policy identifier when changing index interpretation
  or epoch grouping, so functional inventory caches cannot silently reuse old results.

## Dependencies and licensing

- Maintain the C++ UBX/SBF parser in `libcppgnss/` using CMake and C++20.
  Expose reusable functionality through `cppgnss::cppgnss`; keep transport,
  recording policy, and automatic terminal output in applications.
- `libcppgnss/examples/ubxlogger.cpp` is the maintained logger application.
  Preserve its CLI flags; recording follows the single GPST policy above.
- Generate parsers into the build directory from the pinned
  `contrib/pyubx2` and `contrib/pysbf2` submodules. Python schemas are build-time dependencies, not
  a runtime dependency of the C++ library. Do not edit generated files.
- The historical `rpi-gnss-server` repository is no longer the maintenance
  location for these components. Preserve imported BSD-3-Clause notices;
  new project-owned code follows the project GPL-3.0-only policy.

- License original project code and documentation under GNU GPL version 3 only
  (`GPL-3.0-only`); keep the full license text in `LICENSE`.
- Keep RTKLIB-EX as the `contrib/RTKLIB` Git submodule, sourced from
  `https://github.com/rtklibexplorer/RTKLIB.git` with `main` as its update branch.
  The recorded submodule commit is the reproducible revision; do not replace
  it with a floating branch checkout during production processing.
- Preserve third-party copyright and license notices. The RTKLIB submodule
  retains its upstream license; project formatting and language policies
  apply to project-owned files, not vendored submodule contents.
