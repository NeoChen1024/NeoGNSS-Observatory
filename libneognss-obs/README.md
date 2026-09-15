# libneognss-obs

Observatory-specific C++20 processing on top of
[libcppgnss](../libcppgnss/README.md). The protocol library does not depend on
this component. CMake exposes `neognss_obs::neognss_obs`.

The library implements archive epoch indexing and GPST segmentation, receiver
clock association/unwrap and MON-SYS restart/temperature tracking, and SBAS
mask/aging state, GPS STEC/receiver DCB processing, and static GPS Float PPP
using the RTKLIB-EX core.
The pybind11 extension exposes bounded-batch processing;
Python owns file I/O, orchestration, Parquet and plots. JSON containers here
are in-memory records, not a subprocess or JSON-text transport.

## Build and use

Requirements: CMake 3.24+, a C++20 compiler and standard library supporting
`std::format`, Python 3.11+ development files, pybind11, OpenSSL Crypto development
files, a C compiler, and initialized `contrib/pyubx2`, `contrib/pysbf2`,
`contrib/json` and `contrib/RTKLIB`.
Install the repository Python package to build and install the extension:

```sh
git submodule update --init contrib/pyubx2 contrib/pysbf2 contrib/json contrib/RTKLIB
python -m pip install .
ngo-receiver-clock --input-dir /data/reconstructed --output /data/clock
ngo-cnex-import init /data/cnex --setup /data/setup.json
ngo-cnex-import run -p ubx --station /data/cnex /data/first.ubx
ngo-sbas-grid-parquet --input-dir /data/cnex --output /data/sbas-grid
```

No `--worker` or `--indexer` executable paths are used. The old internal
executables are removed; the independent `neoubxlogger` application remains.
For a C++-only root build, use `-DNEOGNSS_BUILD_BINDINGS=OFF`.

## Batch interface

The experimental extension is `neognss_observatory._native`:

| Entry point | Input and result |
| --- | --- |
| `DatasetScan(protocol="ubx", qa=True, gap_timeout=50)` | Read-only streaming QA; `qa=False` supplies timed UBX SBAS batches without diagnostics |
| `archive_index(buffer)` | Read-only UBX buffer to a packed `UBXIDX04` index |
| `SegmentPlanner(joins, timeout_ms)` | Source index buffers to GPST segments/quarantined spans |
| `ClockProcessor(..., protocol="ubx")` | UBX/SBF chunks to clock, PPS, adjustment and temperature/uptime batches |
| `SubframeProcessor(sbas_only=True)` | UBX chunks to decoded navigation-frame records |
| `GridProcessor(correction_age=600, mask_age=1200, gap_timeout=0)` | Protocol-neutral timed SBAS batches to valid IGP intervals |
| `SbfParser(block_ids=[])` | SBF chunks to typed block records, optionally filtered by block ID, with GEORawL1 SBAS extraction |
| `ObservationReader(protocol="ubx")` | UBX RAWX / SBF MeasEpoch chunks to opaque GPS observation batches |
| `CnexObservationReader(protocol, setup_id, antenna=0)` | UBX RAWX / SBF MeasEpoch chunks to CommonNEX pilot observation, RawBits and completion-event Arrow batches |
| `CnexTimeProbe(protocol)` | Independent head-sample framing and first valid observation/navigation GPST anchors; no Arrow science output |
| `PppFloat(settings)` | Observation batches and local precise products to Float solution/residual arrays |

Raw-processing CLIs provide `--protocol/-p ubx|sbf` (default `ubx`). The stream
decoders skip complete checksum-valid frames of the unselected protocol and
expose `skipped_protocol_frames` and `skipped_protocol_bytes`; Python reports
throttled warnings. Selecting SBF does not imply every analysis supports SBF:
see [supported extraction paths](../docs/subframes.md#explicit-wire-protocol).

`feed()` accepts a contiguous **read-only** byte buffer, such as `bytes` or a
read-only mmap. A caller must not modify backing storage during the call.
The CommonNEX pilot currently accepts `bytes` specifically; consume each returned
batch once with `pyarrow.record_batch()`. It does not use the GPS-only
`ObservationReader` or RTKLIB normalization. See [its guide](../docs/commonnex/importer.md)
for deferred-tail handling and incomplete catalog coverage.
Native work releases the GIL. Results own their memory; there are no borrowed
per-frame objects escaping into Python and no per-frame Python callbacks.
Concurrent calls on the same processor are rejected; independent processors
may run on independent threads. Batch sizes should be bounded by the caller.
RTKLIB calls are process-wide serialized because the core contains shared caches;
use separate processes for independent parallel PPP runs. `PppFloat.products()`
loads a new product window without resetting filter state, and `process()`
returns structured NumPy arrays rather than per-observation Python objects.
See [PPP settings, models and limits](../docs/ppp.md).

`StecCnexReader(setup_id, signal1, signal2)` accepts CommonNEX Observation Arrow
batches through `feed()` and returns the opaque `ObservationBatch` consumed by
`StecProcessor`. `flush()` emits the pending complete measurement epoch after
all its rows have arrived. The adapter validates selected signal/quality fields
and converts decimal GPST to integer nanoseconds without floating-point absolute
time. `StecProcessor` returns
structured NumPy sample/arc arrays. `products()` replaces only product data;
`preview()` estimates open arcs without closing them, and `checkpoint()`/`restore()`
preserve scientific state across daily invocations. `finish()` is reserved for
explicit scientific stream termination. `fit_receiver_dcb()` fits one
receiver/signal-pair window from leveled residual batches, with coverage gates.
RTKLIB adapters share the same process-wide lock. See [STEC conventions and
limits](../docs/stec.md); GIM-constrained estimates are not independent calibration.

For clocks, pass each file's name to `feed()` and retain one processor across
files. Frame-start source attribution is preserved even for split frames.
`end_file()` is an explicit framing reset for known complete streams, not
required at ordinary file boundaries. Call `finish()` once at the end of the receiver
stream. A `SubframeProcessor` spans a complete continuous group and is not
finished at ordinary file/day boundaries. One `GridProcessor` represents one
signal in one continuous group; `finish(end_gpst_ms)` stops at the last observed
epoch, without inventing an extra second.

`GridProcessor.process_frames()` accepts batches with `gpst_ms`, `frame_id`,
`frame` (32 bytes carrying 250 MSB-first bits with six zero padding bits),
`crc_valid`, and nullable `accepted`. It re-decodes SBAS content and verifies
CRC in C++ without any knowledge of the source transport. The Python adapter
constructs these batches from ParquetNEX RawBits and navigation Events; this
internal batch interface is not an additional storage format. Grid frame IDs
are run-local occurrence counters, not raw byte offsets or Parquet foreign keys.

The decoded-content interface `GridProcessor.process()` accepts `gpst_ms`, `offset` and `sbas`
keys. These times are continuous milliseconds since 1980-01-06 GPST. For UBX,
the shared streaming epoch assembler establishes reception context. These times
must not be confused with a measured SBAS transmission timestamp.

SBF adapters use GEORawL1 TOW/WNc and reject receiver-failed CRC records for
grid updates. A positive `gap_timeout` clears state after a per-signal message
gap, closing intervals at the last observed time; zero disables this extra
policy when explicit frame-stream end records already define continuity. Feed all SBAS
message types for gap detection, not just mask/correction messages. Python
attaches RINEX `satellite_system`, `satellite_number`, and `signal` identities to
intervals, keeping SBAS Sxx numbering through plotting and selection.

This is pre-Alpha research code, not a stable ABI or safety-critical SBAS
implementation. See [architecture and limitations](../docs/native-architecture.md).
