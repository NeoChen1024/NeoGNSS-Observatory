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

## Documentation audience

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
- Every derived artifact must be reproducible from source data, pinned tools,
  versioned configuration, and recorded provenance.
- Never infer observation coverage or time scale solely from filenames; inspect
  payload timestamps and preserve original GNSS fields as provenance.

## Single GPST policy

- All project observation time axes, day/hour partitions, processing windows,
  map labels and derived artifact metadata use GPST. No optional UTC mode or
  compatibility reading of old UTC-derived products is maintained.
- Scalar `gpst`, `start_gpst`, `end_gpst` and `hour_gpst` values are continuous
  seconds since 1980-01-06 00:00:00 GPST, not Unix timestamps. Integer-second
  indexes describe nominal receiver epochs; raw fractional time remains intact.
- Name assigned UBX outputs `GPST-%Y-%m-%d--%H-%M-%S.ubx`. Never use a UTC `Z`
  or `+0000` suffix for GPST. Keep native protocol fields and external product
  formats unchanged; decode their specified scales at the input boundary.
- Logger epochs are buffered until EOE. Every EOE requires a fresh valid
  NAV-TIMEGPS with matching iTOW; otherwise fail. Rotate before writing the
  complete first epoch of the new GPST day, including its EOE.
- Old UTC analysis/reconstruction outputs were explicitly cleared. Preserve
  archives, downloaded products, downloader plans/configuration and map assets.
  Rebuild derived outputs with GPST tools; do not relabel old timestamps.

## Toolchain

- This workspace has Septentrio RxTools under `~/.local/RxTools`; its SBF to
  RINEX converter is `bin/sbf2rin`. Read the installed tool's help and record
  its version and binary hash for each run. Do not vendor the installation.
- References to RTKLIB mean the RTKLIB-EX `main` branch from
  `rtklibexplorer/RTKLIB`, not upstream `tomojitakasu/RTKLIB`.
- Pin exact tool revisions or container digests for production processing.
- Preserve original observation codes and receiver message provenance during
  normalization.

## Python scripts

The library's build-time generator lives in `libcppubx2/scripts/`; Python
regression tests may live beside the C++ library. The generator uses Click;
tests are run through unittest discovery rather than custom Python CLIs.
Use the repository-root `./.venv`, managed by uv, for code generation and
all Python tests. Install dependencies with `uv pip install --python
.venv/bin/python ...`; do not create per-component environments or fall
back to system Python.

Local setup commands (create `.venv` only if it does not already exist):

```sh
uv venv .venv
uv pip install --python .venv/bin/python -e .
uv pip install --python .venv/bin/python -r libcppubx2/requirements-codegen.txt
uv pip install --python .venv/bin/python pre-commit
.venv/bin/pre-commit install
.venv/bin/pre-commit run --all-files
```

The current CMake configuration selects the repository-root
`./.venv/bin/python` for code generation and Python tests, including when
configuring `libcppubx2/` directly.

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
  `requirements.txt`, allowing upgrades without a lockfile. Record the actual
  installed versions in each processing run's provenance.

## Dependencies and licensing

- Maintain the C++ UBX parser in `libcppubx2/` using CMake and C++20.
  Expose reusable functionality through `cppubx2::cppubx2`; keep transport,
  recording policy, and automatic terminal output in applications.
- `libcppubx2/examples/ubxlogger.cpp` is the maintained logger application.
  Preserve its CLI flags; recording follows the single GPST policy above.
- Generate parsers into the build directory from the pinned
  `contrib/pyubx2` submodule. Python/pyubx2 is a build-time dependency, not
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
