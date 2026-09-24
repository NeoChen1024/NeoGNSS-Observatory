# NeoGNSS Observatory — Agent guide

A pre-Alpha GNSS research project with native protocol/processing libraries and
Python orchestration, storage and visualization.

## Sources of truth

- [Documentation index](docs/README.md): routes scientific and tool questions.
- [Processing overview](docs/processing-overview.md): supported workflows and scope.
- [Native architecture](docs/native-architecture.md): library ownership and interop.
- [GPST policy](docs/time-policy.md): time interpretation and normalization.
- [CommonNEX](docs/commonnex/overview.md): logical records and ParquetNEX;
  [importer](docs/commonnex/importer.md) describes the implemented subset.
- [Dataset notes](docs/dataset-notes.md): acquisition and interpretation caveats.
- Tool documentation and executable `--help` own usage and implemented options.

## Documentation

- Describe the current supported contract, usage and explicitly agreed future
  scope. Distinguish implemented behavior, unsupported behavior and plans.
- Give each topic one primary home. Elsewhere, provide a necessary summary and
  link rather than duplicating the full content.
- Keep architecture and stable interfaces needed to understand, use or extend
  the project. Avoid source walkthroughs, internal scheduling, data structures
  and optimization details that duplicate code or are likely to change soon.
- Scientific and format documentation must let readers correctly interpret data
  and results without reading the implementation. Preserve field types, units,
  time semantics, null/validity rules, calibration assumptions and source
  selection policies; cite upstream specifications where needed.
- Do not turn ordinary documentation into decision histories, debugging notes,
  benchmark logs or test transcripts. Report validation evidence in the task
  response unless a report is explicitly requested.
- Omit incidental local paths, hostnames, personal hardware settings and test
  dates. Retain acquisition caveats or operational details when necessary for
  scientific interpretation, supported operation or an explicitly requested
  report, without preserving the troubleshooting history.
- Update the owning document when behavior changes; remove superseded plans,
  obsolete workflows and stale references. Git history is the archive. Keep
  READMEs public-facing, not local environment notebooks, and this guide limited
  to project-wide workflow, conventions and safety rather than per-tool schemas,
  defaults, CLI options or runbooks.

## Scope and scientific integrity

- Prioritize clear computation and fast iteration over compatibility and
  speculative infrastructure. Do not preserve obsolete interfaces for tests.
- GLONASS and NavIC scientific processing are intentionally out of scope;
  leave upstream protocol definitions intact and frame mixed input safely.
- Project-owned scientific time axes use GPST; follow the time policy rather
  than inventing local conversions or relabeling old products.
- Preserve units, signal identities, validity, missing values and necessary
  interpretation metadata. Do not fabricate missing observations or infer
  coverage/time scale solely from filenames.
- Do not add provenance bundles, dependency inventories, execution snapshots or
  generic recovery frameworks without an explicit request. Preserve functional
  integrity/recovery mechanisms that serve the requested workflow.

## Data safety

- Raw archives and expanded source datasets are read-only, including those under
  `/hdd` and `~/net/DATA_SSD/datasets/GNSS`. Write only to designated derived
  output locations; a destination beside source data is not permission to alter it.
- Keep generated and large scientific products out of Git. Preserve unrelated
  files and never silently overwrite or delete source data.
- Publish completed outputs by rename where appropriate. Do not rewrite existing
  datasets solely to adopt new defaults.
- Use Zstandard level 3 explicitly for project-owned Parquet writers:
  `compression="zstd", compression_level=3`.

## Language and style

- Write project files in English, including documentation and code comments.
  Preserve standardized GNSS names, external identifiers, quoted material and
  significant fixture bytes. Translate surrounding non-English project text
  when editing it. User communication need not be English.
- Python targets 3.11+. Use Black (132 columns, Python 3.11 target) and isort
  (Black profile), following the repository configuration.
- Use Click for Python CLIs, a `cli` entry point and the installed `ngo-` prefix.
  Directly executable Python scripts use `#!/usr/bin/env python`.
- Use Rich/`rich.progress` for moderately changing presentation-oriented status;
  use tqdm for throughput-sensitive processing. Keep updates out of inner loops.
  When stdout carries machine-readable data, diagnostics/progress go to stderr.
- Project-owned C/C++ uses C++20, CMake, root `.clang-format` and `.clang-tidy`.
  Exclude vendored and generated code from formatting/tidy edits. Review
  behavior-changing tidy suggestions before applying them.

## Code ownership and dependencies

- Python lives in `python-src/neognss_observatory/`. Keep runtime dependencies
  in `requirements.txt`, consumed by setuptools/`pyproject.toml`, using minimum
  versions (`>=`) without introducing a lockfile.
- Reusable protocol mechanisms belong in `libcppgnss/`; Observatory processing
  and bindings belong in `libneognss-obs/`, which depends on it, never the reverse.
  Python owns file orchestration, Parquet and plots; consult the architecture
  document for detailed boundaries.
- Search existing shared APIs before adding another parser or helper. Share
  mechanisms with identical semantics while keeping caller policy separate.
- Batch native/Python exchange and release the GIL during native work; do not
  introduce per-frame Python callbacks or executable-worker backends.
- Generate protocol definitions into the build directory from pinned submodules.
  Do not edit generated files or modify vendored code as an incidental cleanup.
- Original code and documentation use GPL-3.0-only. Preserve third-party licenses
  and imported notices. Submodule updates must be intentional.

## Local toolchain and verification

- Use the repository-root `./.venv`, managed by uv, for Python execution,
  code generation and any explicitly requested tests. Do not create component
  environments or fall back to system Python.
- Install with `uv pip install --python .venv/bin/python ...`; create `.venv`
  with `uv venv .venv` only if absent.
- Use the configured CMake build and compilation database in `build/`.
  Follow the component build documentation; pinned pre-commit hooks own Python
  formatting checks.
- Do not add or expand automated tests unless the user explicitly requests them.
  Validate proportionately with builds, representative inputs and direct output
  inspection; keep one-off experiments out of the permanent test suite.
- Check the actual scientific/output contract, including failure and resource
  bounds when relevant. Do not rely solely on a round trip through the same
  implementation as independent evidence. Report what was actually checked.
