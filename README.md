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

The first deliverable is deliberately smaller than a complete two-year
reprocessing run:

1. Generate one GPST day of canonical RINEX from Era C's 1 Hz SBF data.
2. Cross-check it against the receiver-generated 30 s RINEX.
3. Produce availability, gap, cycle-slip, and signal-pair QC.
4. Produce carrier-phase relative dTEC, ROT/ROTI, and IPP time series.
5. Expand to all 147 Era C days only after the golden-day gate passes, then
   ingest Eras A and B.

All references to RTKLIB mean the RTKLIB-EX `main` branch from
[`rtklibexplorer/RTKLIB`](https://github.com/rtklibexplorer/RTKLIB), not the
upstream `tomojitakasu/RTKLIB` repository.

## Python tools

The Python package requires Python 3.11 or newer. Runtime dependencies are
declared in `requirements.txt`. The `cddis-download` command inventories,
downloads and verifies external GNSS products. `ubx-restitch` reconstructs Era A
GPST segments, `sbas-grid-render` produces experimental hourly SBAS VTEC maps,
and `sbas-map-video` encodes those maps as a manifest-ordered HEVC/MP4 preview.
`sbf-rinex` wraps an installed Septentrio RxTools converter for native-rate SBF
exports; `rinex-observation-audit` inventories the resulting observations.
See the [RxTools conversion experiment](docs/era-c-rxtools-experiment.md) for
preservation options, validation results and current limits.

```sh
python -m pip install .
```

Python dependencies use minimum versions to allow upgrades;
processing runs record their actual installed versions in provenance.

See [the downloader guide](docs/cddis-downloader.md) and
[example configuration](config/products.example.toml) for Earthdata setup,
product selection, optional checksum snapshots and resumable downloads.
See the [subframe and SBAS guide](docs/subframes.md) for extraction and hourly
map semantics.
The [IPP track experiment](docs/tec-ipp-maps.md) adds relative dSTEC trajectories
over pale SBAS backgrounds, with parallel hourly PNG export.

## C++ UBX library

[`libcppubx2/`](libcppubx2/README.md) contains the maintained C++20 UBX
library and the original logger as `examples/ubxlogger.cpp`. CMake exposes
`cppubx2::cppubx2`; parsers are generated at build time using the pinned
`contrib/pyubx2` schema. The compiled library has no Python runtime dependency.
See the library guide for build, testing, API, and logger compatibility details.

The [UBX reconstruction tool](docs/ubx-restitch.md) inventories expanded archives,
proves cross-file overlaps, and writes UTC segments without modifying inputs.

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
