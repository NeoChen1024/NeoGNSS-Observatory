# NeoGNSS Observatory

This pre-Alpha project provides offline GNSS research tools for receiver
telemetry, SBAS grids, phase-leveled GPS STEC and GPS Float PPP. The repository stores
code and configuration. Raw archives remain
read-only preservation masters, and large derived data stays out of Git.

Era A-C processing reads expanded input files directly, without an XZ
decompression stage.
Both the expanded dataset and preservation masters remain read-only.

See the [documentation index](docs/README.md) for current tools and the
[processing overview](docs/processing-overview.md) for implemented paths and
scientific limits. Absolute STEC estimates are GIM-constrained, not independently
calibrated; multi-GNSS PPP remains an extension goal.
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

All Python commands use the `ngo-` prefix, including `ngo-cnex-import`. With the installation's
executable directory on `PATH`, type `ngo-` and use shell command completion
to list them. Each command provides `--help`.

[`ngo-mosaic-push`](docs/mosaic-push.md) runs as a daemon or one-shot sync on a
Linux receiver host such as a Raspberry Pi. It retrieves closed mosaic SBF files
over FTP, verifies local xz compression, and publishes archives with per-file
SHA-512 checksums of the original SBF to independently enabled FTPS targets.
A single JSON configuration controls resumable transfers, retry backoff, local
file/directory permissions and an optional storage cap that protects pending
uploads while evicting older completed archives. Start with the
[configuration example](config/mosaic-push.example.json), or the
[local-only example](config/mosaic-push-local.example.json) without FTPS forwarding.

The Python package requires Python 3.11 or newer. Runtime dependencies are
declared in `requirements.txt`. The `ngo-cddis-download` command inventories,
downloads and verifies external GNSS products. `ngo-dataset-qa` performs optional
read-only QA by default; `--profile restitch` reconstructs overlapping UBX
archives such as Era A. Nonoverlapping Era B/C inputs need no reconstruction.
`ngo-cnex-import run -p ubx|sbf` imports observations and multi-GNSS RawBits
into an initialized single-station ParquetNEX directory. `init` accepts a
vendor-config companion and optional ANTEX catalogs for the station's antenna.
`ngo-sbas-grid-parquet` reads its RawBits and Events to calculate daily GPST
grid validity intervals; `ngo-sbas-grid-plot` reads those
daily files to produce experimental hourly VTEC maps without reopening raw recordings.
`ngo-sbas-map-video` encodes
maps as a manifest-ordered HEVC/MP4 preview.
`ngo-sbf-rinex` wraps an installed Septentrio RxTools converter for native-rate SBF
exports; `ngo-rinex-observation-audit` inventories the resulting observations.
See the [RINEX conversion guide](docs/rinex-conversion.md) for options and limitations.

`ngo-rx-msgratio recording.ubx` or `ngo-rx-msgratio -p sbf recording.sbf`
scans a complete expanded recording and lists every checksum-valid message
ID/name, encountered SBF revisions, count, bytes and
percentage of the entire file, sorted by descending size. Sizes include headers
and padding; SBF revisions are combined per block ID, and UBX messages are grouped
by class/message ID (hexadecimal). Unknown IDs are included without
payload decoding. Foreign-protocol frames and unclassified/damaged bytes are reported
separately. This read-only tool helps identify disk-logging storage costs and
does not require usable timestamps or a navigation fix.

Use `--format json` or `--format csv` for machine-readable stdout; progress and
warnings stay on stderr. For example:

```sh
ngo-rx-msgratio -p sbf --format json recording.sbf > message-sizes.json
ngo-rx-msgratio --format csv recording.ubx > message-sizes.csv
```

JSON includes message rows and file-level accounting/diagnostics. IDs and byte
counts are integers; UBX IDs are `(class << 8) | message_id`. `percent_of_file`
is numeric on a 0-100 scale, using `source_bytes` as denominator (zero for an
empty file). CSV contains `message`, `foreign` and `unclassified` record types;
their byte counts sum to the file size without subtotal double-counting. SBF
revisions are semicolon-separated in CSV and integer arrays in JSON. CSV's
unclassified row carries invalid-candidate and pending-tail diagnostics.

```sh
git submodule update --init contrib/pyubx2 contrib/pysbf2 contrib/json
git submodule update --init contrib/RTKLIB
git submodule update --init contrib/arrow-nanoarrow contrib/int128
python -m pip install .
```

Python dependencies use minimum versions to allow upgrades. Building the package
also requires CMake 3.24+, a C++20 compiler/standard library with `std::format`,
and OpenSSL Crypto development files. Installation builds the native extension used by UBX/SBF processing. The
optional RINEX converter is built separately.

### CommonNEX import pilot

`ngo-cnex-import init|run|list` imports UBX RAWX or SBF MeasEpoch into daily
Observation, multi-GNSS RawBits and independent measurement/navigation completion
Events Parquet, using native Arrow batches. Undefined future RawBits representations and the
remaining Events/quality mappings are not implemented
by this pilot. See [the importer guide](docs/commonnex/importer.md) for scope,
initialization, tail continuation and explicit reconstruction.

See [the downloader guide](docs/cddis-downloader.md) and
[example configuration](config/products.example.toml) for Earthdata setup,
product selection, optional checksum snapshots and resumable downloads.
See the [subframe and SBAS guide](docs/subframes.md) for extraction and hourly
map semantics.

### GPS STEC and receiver DCB

`ngo-stec --input-dir /data/commonnex-station` reads CommonNEX GPS L1/L2
observations, applies exact-signal
satellite code biases, levels phase to code and estimates receiver DCB against
local CODE IONEX products. It writes daily samples plus arc and receiver-bias
Parquet tables. Daily incremental runs retain cross-day arcs and refit affected
DCB windows; `--rebuild` handles source revisions or changed settings.
Insufficient calibration windows retain unavailable absolute
values, never an assumed zero bias. Missing STEC products produce warnings and
unavailable dependent fields without aborting other time periods.
`ngo-stec-plot` renders hourly absolute-STEC
IPP trajectories incrementally and in parallel, using the bundled Natural Earth
10m coastline (overridable with `--coastline`) and optional pale
SBAS VTEC backgrounds. See [the STEC guide](docs/stec.md) and
[example configuration](config/stec.example.toml).

### Offline PPP Float

`ngo-ppp` processes GPS L1/L2 observations from UBX/SBF directly through the
RTKLIB-EX core using locally downloaded precise products and IGS20/NGS20 antenna
catalogs. `ngo-ppp-plot` renders the daily Parquet outputs in parallel.
See [the PPP guide](docs/ppp.md) for configuration and the initial GPS-only,
static, forward Float scope; this is not a PPP-AR or CSRS-equivalent solver.

### Receiver clock analysis

The [receiver clock pipeline](docs/receiver-clock.md) separates extraction,
clock reconstruction, and plotting:

- `ngo-receiver-clock` reads CommonNEX receiver-clock, measurement-clock,
  status, pulse and restart catalogs, then produces derived clock Parquet.
  Receiver protocols and time association are handled only by the importer;
  unavailable time stays unknown and restart uncertainty is not bridged.
- `ngo-receiver-clock-reunwrap` recalculates clock arcs and accumulated bias
  corrections from existing Parquet, including UBX and SBF adjustment evidence. Short gaps retain
  the accumulated correction; the default timeout is 50 seconds. Unresolved
  jumps remain explicit uncertainty boundaries, not inferred reboots.
- `ngo-receiver-clock-plot` produces parallel hourly detail PNGs and daily overviews
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
and SBF protocol library and the maintained logger as `examples/gnsslogger.cpp`.
CMake exposes `cppgnss::cppgnss`; codegen covers UBX and all available pinned
`contrib/pysbf2` block definitions. The generic library has no Python runtime
dependency. SBAS L1 decoding accepts receiver-independent air-frame bits.

[`libneognss-obs/`](libneognss-obs/README.md) contains Observatory-specific
archive segmentation, clock reconstruction and SBAS grid state. Its pybind11
extension passes batches directly to Python, releasing the GIL during native
processing. Python owns orchestration, file/Parquet I/O and plotting.
`neognsslogger` is a standalone application.

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

The maintained `neognsslogger` supports UBX/SBF (`-p ubx|sbf`), file/stdin input
and TCP with reconnection.
Both logger and reconstruction outputs use
`GPST-%Y-%m-%d--%H-%M-%S-mmm.ubx`, with exactly three millisecond digits;
SBF logger recordings use `.sbf` instead.

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
- SBF records through EndOfPVT using its GPST WNc/TOW; EndOfMeas provides
  independent measurement cadence diagnostics. `-d` displays decoded SBF fields
  and raw hex for unsupported blocks without dropping CRC-valid recordings.

Diagnostic warnings do not stop recording. Recording requires valid monotonic
TIMEGPS/EOE (UBX) or EndOfPVT (SBF). See the [logger guide](libcppgnss/README.md#overnight-continuity-diagnostics)
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
