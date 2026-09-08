# NeoGNSS Observatory

This pre-Alpha project provides offline GNSS research tools for receiver
telemetry, SBAS grids and relative carrier-phase TEC. The repository stores
code and configuration. Raw archives remain
read-only preservation masters, and large derived data stays out of Git.

Era A-C processing reads expanded input files directly, without an XZ
decompression stage.
Both the expanded dataset and preservation masters remain read-only.

See the [documentation index](docs/README.md) for current tools and the
[processing overview](docs/processing-overview.md) for implemented paths and
scientific limits. PPP and calibrated absolute TEC remain extension goals.
All observation processing uses [one GPST time policy](docs/time-policy.md),
including day/hour boundaries and GPST-prefixed derived filenames.

All references to RTKLIB mean the RTKLIB-EX `main` branch from
[`rtklibexplorer/RTKLIB`](https://github.com/rtklibexplorer/RTKLIB), not the
upstream `tomojitakasu/RTKLIB` repository.

## Research status

This is a pre-Alpha research project. CLIs, algorithms, intermediate schemas and
output layouts may change without compatibility layers. Validation focuses on
representative data and a small set of protocol/numerical checks, not fixed
experimental script contracts. Raw archives remain read-only.

## Python tools

The Python package requires Python 3.11 or newer. Runtime dependencies are
declared in `requirements.txt`. The `cddis-download` command inventories,
downloads and verifies external GNSS products. `ngo-dataset-qa` performs optional
read-only QA by default; `--profile restitch` reconstructs overlapping UBX
archives such as Era A. Nonoverlapping Era B/C inputs need no reconstruction.
`sbas-frame-parquet -p ubx|sbf` extracts raw or reconstructed UBX or
expanded SBF into source-independent daily SBAS frame Parquet.
`sbas-grid-parquet` reads only these frames to calculate daily GPST
grid validity intervals; `sbas-grid-plot` reads those
daily files to produce experimental hourly VTEC maps without reopening raw recordings.
`sbas-map-video` encodes
maps as a manifest-ordered HEVC/MP4 preview.
`sbf-rinex` wraps an installed Septentrio RxTools converter for native-rate SBF
exports; `rinex-observation-audit` inventories the resulting observations.
See the [RINEX conversion guide](docs/rinex-conversion.md) for options and limitations.

```sh
git submodule update --init contrib/pyubx2 contrib/pysbf2 contrib/json
python -m pip install .
```

Python dependencies use minimum versions to allow upgrades. Building the package
also requires CMake 3.24+, a C++20 compiler/standard library with `std::format`,
and OpenSSL Crypto development files. Installation builds the native extension used by UBX/SBF processing. The
optional RINEX geometry backend is built separately.

See [the downloader guide](docs/cddis-downloader.md) and
[example configuration](config/products.example.toml) for Earthdata setup,
product selection, optional checksum snapshots and resumable downloads.
See the [subframe and SBAS guide](docs/subframes.md) for extraction and hourly
map semantics.
The [IPP track experiment](docs/tec-ipp-maps.md) adds relative dSTEC trajectories
over pale SBAS backgrounds, with parallel hourly PNG export.

### Receiver clock analysis

The [receiver clock pipeline](docs/receiver-clock.md) separates extraction,
clock reconstruction, and plotting:

- `receiver-clock` uses a native UBX scanner to export NAV-CLOCK samples to
  Parquet, together with clock-adjustment events and available MON-SYS
  temperature/runtime telemetry. Runtime decreases identify observed restarts;
  missing MON-SYS does not imply uninterrupted receiver operation.
- `receiver-clock-reunwrap` recalculates clock arcs and accumulated bias
  corrections from existing Parquet, without rescanning UBX. Short gaps retain
  the accumulated correction; the default timeout is 50 seconds. Unresolved
  jumps remain explicit uncertainty boundaries, not inferred reboots.
- `receiver-clock-plot` produces parallel hourly detail PNGs and daily overviews
  showing raw/unwrapped bias, drift, accuracy, adjustment rates and coverage.
  Missing temperature is labeled `unavailable`. The unwrapped curve is not
  manually rebased at arc, file or hour boundaries. Display reduction preserves
  extrema and adjustment boundaries; statistics use the full assigned samples.

Research outputs are published by directory rename after successful processing.
Use `--overwrite` to replace a run; the previous directory is retained as a
backup. Parquet carries scientific metadata; environment snapshots and artifact
hash chains are not generated.

## Native libraries

[`libcppgnss/`](libcppgnss/README.md) contains the maintained C++20 UBX
and SBF protocol library and the maintained logger as `examples/ubxlogger.cpp`.
CMake exposes `cppgnss::cppgnss`; codegen covers UBX and all available pinned
`contrib/pysbf2` block definitions. The generic library has no Python runtime
dependency. SBAS L1 decoding accepts receiver-independent air-frame bits.

[`libneognss-obs/`](libneognss-obs/README.md) contains Observatory-specific
archive segmentation, clock reconstruction and SBAS grid state. Its pybind11
extension passes batches directly to Python, releasing the GIL during native
processing. Python owns orchestration, file/Parquet I/O and plotting.
`neoubxlogger` is a standalone application.

See the [architecture guide](docs/native-architecture.md) for API boundaries
and SBF schema limitations.

The [dataset QA tool](docs/dataset-qa.md) defaults to a read-only scan.
Its explicit restitch profile proves overlaps and writes GPST segments.
Its millisecond indexes preserve subsecond navigation epochs. It splits at
GPST midnight or a NAV-to-NAV interval exceeding `--gap-timeout` (default
50 seconds), supporting both high-rate and 30-second low-rate input without
fabricating missing observations. Nonoverlapping inputs need no reconstruction;
input traversal is recursive by default.

### Logger recording and diagnostics

The maintained `neoubxlogger` supports file/stdin input and TCP with reconnection.
Both logger and reconstruction outputs use
`GPST-%Y-%m-%d--%H-%M-%S-mmm.ubx`, with exactly three millisecond digits.

- The logger's optional first positional argument selects the output root
  (default `./`); recordings retain their `YYYY-MM/` subdirectories.
- `--expected-period-ms` sets the expected navigation cadence (default 1000;
  use 33 for approximately 30 Hz). `--epoch-tolerance-percent` defaults to
  **±20%**, independently of the reconstruction timeout.
- Gap diagnostics report payload GPST intervals and monotonic host EOE arrival
  intervals. `--expect-nav-clock` additionally checks for missing NAV-CLOCK;
  duplicate/mismatched CLOCK messages, TCP problems and checksum failures are
  also reported, with cumulative diagnostic totals.
- Valid TIME-only solutions count toward `FIX` statistics alongside 2D/3D
  solutions; `gnssFixOK` is still required.

Diagnostic warnings do not stop recording. Existing fatal TIMEGPS/EOE checks
remain enforced. See the [logger guide](libcppgnss/README.md#overnight-continuity-diagnostics)
for an overnight TCP test example and the limits of attributing gaps to a
receiver, transport bridge, or recording software.

## Submodules

Initialize the recorded revision after cloning:

```sh
git submodule update --init --recursive
git -C contrib/RTKLIB rev-parse HEAD
```

The submodule lives in `contrib/RTKLIB`. Its update branch is `main`, while
the parent repository records an exact commit. Updating that revision is an
explicit dependency change. Installing the Python package does not build
RTKLIB.

`contrib/pyubx2` is also pinned to an exact revision and is used only for
C++ parser generation. The logger and parser source are maintained directly
in this repository.

## License

Original project code and documentation are licensed under GNU GPL version 3
only (`GPL-3.0-only`); see [LICENSE](LICENSE). Third-party components retain
their own licenses, including [RTKLIB's upstream license](contrib/RTKLIB/license.txt).
