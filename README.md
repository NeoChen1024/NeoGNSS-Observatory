# NeoGNSS Observatory

This project builds a reproducible offline ionospheric-observation and static
PPP pipeline from the existing GNSS raw archive. The repository stores code,
configuration, manifests, and compact result summaries. Raw archives remain
read-only preservation masters, and large derived data stays out of Git.

Era A-C processing reads expanded input files directly, without an XZ
decompression stage.
Both the expanded dataset and preservation masters remain read-only.

See [docs/offline-processing-plan.md](docs/offline-processing-plan.md) for the
verified data inventory and proposed implementation sequence.

All observation processing uses [one GPST time policy](docs/time-policy.md),
including day/hour boundaries and GPST-prefixed derived filenames.

The broader scientific validation plan remains staged rather than treating
full-archive processing as proof of scientific validity:

1. Generate one GPST day of canonical RINEX from Era C's 1 Hz SBF data.
2. Cross-check it against the receiver-generated 30 s RINEX.
3. Produce availability, gap, cycle-slip, and signal-pair QC.
4. Produce carrier-phase relative dTEC, ROT/ROTI, and IPP time series.
5. Expand to all 147 Era C days only after the golden-day gate passes, then
   ingest Eras A and B.

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
downloads and verifies external GNSS products. `ubx-restitch` reconstructs Era A/B
GPST segments. `sbas-frame-parquet -p ubx|sbf` extracts reconstructed UBX or
expanded SBF into source-independent daily SBAS frame Parquet.
`sbas-grid-parquet` reads only these frames to calculate daily GPST
grid validity intervals; `sbas-grid-plot` reads those
daily files to produce experimental hourly VTEC maps without reopening raw recordings.
The earlier combined raw-to-map command is removed. `sbas-map-video` encodes
maps as a manifest-ordered HEVC/MP4 preview.
`sbf-rinex` wraps an installed Septentrio RxTools converter for native-rate SBF
exports; `rinex-observation-audit` inventories the resulting observations.
See the [RxTools conversion experiment](docs/era-c-rxtools-experiment.md) for
preservation options, validation results and current limits.

```sh
git submodule update --init contrib/pyubx2 contrib/pysbf2 contrib/json
python -m pip install .
```

Python dependencies use minimum versions to allow upgrades. Building the package
also requires CMake 3.24+, a C++20 compiler/standard library with `std::format`,
and OpenSSL Crypto development files. Installation builds the native extension;
processing commands no longer accept `--worker` or `--indexer` executable paths.

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
and SBF protocol library and the original logger as `examples/ubxlogger.cpp`.
CMake exposes `cppgnss::cppgnss`; codegen covers UBX and all available pinned
`contrib/pysbf2` block definitions. The generic library has no Python runtime
dependency. SBAS L1 decoding accepts receiver-independent air-frame bits.

[`libneognss-obs/`](libneognss-obs/README.md) contains Observatory-specific
archive segmentation, clock reconstruction and SBAS grid state. Its pybind11
extension passes batches directly to Python, releasing the GIL during native
processing. Python owns orchestration, file/Parquet I/O and plotting. The old
indexer, clock-scan, subframe-export and inspection executables are removed;
`neoubxlogger` remains a standalone application.

See the [architecture guide](docs/native-architecture.md) for API boundaries
and SBF schema limitations.

The [UBX reconstruction tool](docs/ubx-restitch.md) inventories expanded archives,
proves cross-file overlaps, and writes GPST segments without modifying inputs.
Its millisecond indexes preserve subsecond navigation epochs. It splits at
GPST midnight or a NAV-to-NAV interval exceeding `--gap-timeout` (default
50 seconds), supporting both high-rate and 30-second low-rate input without
fabricating missing observations. Era B's nested inputs can be selected with
`--recursive`. New indexes and plans are versioned separately from the older
second-based format.

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
in this repository, not through a submodule of the historical logger project.

## License

Original project code and documentation are licensed under GNU GPL version 3
only (`GPL-3.0-only`); see [LICENSE](LICENSE). Third-party components retain
their own licenses, including [RTKLIB's upstream license](contrib/RTKLIB/license.txt).
