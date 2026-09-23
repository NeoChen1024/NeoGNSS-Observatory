# libneognss-obs

`cnex_engine.hpp` exposes the Python-free `CnexEngine` and `CnexTimeProbe`.
The engine accepts byte chunks and returns four owned Arrow C Data Interface
batches. Python's `CnexObservationReader` delegates to it; binding code owns
capsule transfer, not scientific state. See [live streaming](../docs/commonnex/live.md)
for batch groups, the five-second delivery target and cross-process adapters.

The library owns receiver-to-CommonNEX normalization (`measurements.hpp` and
`raw_bits.hpp`), canonical navigation-body checks and service classification,
and satellite-content decoding (`sbas.hpp`, namespace `neognss_obs::SBAS`).
All receiver fields come from libcppgnss typed parsers. The GPS L1/L2 numerical
adapter in `observations.hpp` selects shared normalized measurements rather
than decoding receiver payloads again. Generated receiver navigation messages
remain in libcppgnss; satellite air-interface algorithms do not.

Observatory-specific C++20 processing on top of
[libcppgnss](../libcppgnss/README.md). The protocol library does not depend on
this component. CMake exposes `neognss_obs::neognss_obs`.

The library implements archive epoch indexing and GPST segmentation, receiver
time association and receiver telemetry normalization, SBAS
mask/aging state, multi-GNSS STEC/receiver DCB processing, and static GPS Float PPP
using the RTKLIB-EX core.
The pybind11 extension exposes bounded-batch processing;
Python owns file I/O, orchestration, Parquet and plots. See
[native architecture](../docs/native-architecture.md) for interop requirements.

## Build and use

Requirements: CMake 3.24+, a C++20 compiler and standard library supporting
`std::format`, Python 3.11+ development files, pybind11, OpenSSL Crypto development
files, a C compiler, and initialized `contrib/pyubx2`, `contrib/pysbf2`,
`contrib/json` and `contrib/RTKLIB`.
Install the repository Python package to build and install the extension:

```sh
git submodule update --init contrib/pyubx2 contrib/pysbf2 contrib/json contrib/RTKLIB
python -m pip install .
ngo-cnex-import init /data/cnex --setup /data/setup.json
ngo-cnex-import run -p ubx --station /data/cnex /data/first.ubx
ngo-receiver-clock --input-dir /data/cnex --output /data/clock
ngo-sbas-grid-parquet --input-dir /data/cnex --output /data/sbas-grid
```

The independent UBX/SBF `neognsslogger` application belongs to libcppgnss.
For a C++-only root build, use `-DNEOGNSS_BUILD_BINDINGS=OFF`.

Clang with an installed libc++/libc++abi can be verified in a separate build
directory, from the repository root:

```sh
cmake -S . -B build/clang-libcxx \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_CXX_FLAGS=-stdlib=libc++ -DCMAKE_BUILD_TYPE=Release
cmake --build build/clang-libcxx --parallel
ctest --test-dir build/clang-libcxx --output-on-failure
PYTHONPATH=build/clang-libcxx/libneognss-obs python -c 'import _native'
```

Use the Python environment selected at configuration time for the import check.
The final import is necessary: a successfully linked extension can still fail
while registering its C++ types. See [portability rules](../docs/native-architecture.md#c-toolchain-portability).

## Batch interface

The experimental extension is `neognss_observatory._native`:

| Entry point | Input and result |
| --- | --- |
| `DatasetScan(protocol="ubx", gap_timeout=50)` | Read-only streaming QA |
| `archive_index(buffer)` | Read-only UBX buffer to a packed `UBXIDX04` index |
| `SegmentPlanner(joins, timeout_ms)` | Source index buffers to GPST segments/quarantined spans |
| `GridProcessor(correction_age=600, mask_age=1200, gap_timeout=0)` | Protocol-neutral timed SBAS batches to valid IGP intervals |
| `ObservationReader(protocol="ubx")` | UBX RAWX / SBF MeasEpoch chunks to opaque GPS observation batches |
| `CnexObservationReader(protocol, setup_id, antenna, period_seconds, period_ps)` | UBX/SBF chunks to four CommonNEX Arrow batches: observations, events, RawBits and receiver-telemetry |
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

`StecCnexReader(setup_id, pairs)` accepts CommonNEX Observation Arrow
batches through `feed()` and returns the opaque `ObservationBatch` consumed by
`StecProcessor`. `flush()` emits the pending complete measurement epoch after
all its rows have arrived. The adapter validates selected signal/quality fields
and converts decimal GPST to integer nanoseconds without floating-point absolute
time. `StecProcessor` returns
structured NumPy sample/arc arrays. `products()` replaces only product data;
`restarts()` schedules timed receiver boundaries that close all active pair
arcs before the first epoch at or after each boundary.
`preview()` estimates open arcs without closing them, and `checkpoint()`/`restore()`
preserve scientific state, pending restart boundaries and their cursor across
daily invocations. `finish()` is reserved for
explicit scientific stream termination. `fit_receiver_dcb()` fits one
receiver-segment/signal-pair window from leveled residual batches, with coverage gates.
RTKLIB adapters share the same process-wide lock. See [STEC conventions and
limits](../docs/stec.md); GIM-constrained estimates are not independent calibration.

Receiver-clock consumes CommonNEX telemetry and Events, not raw receiver files.
One `GridProcessor` represents one
signal in one continuous group; `finish(end_gpst_ms)` stops at the last observed
epoch, without inventing an extra second.

`GridProcessor.process_frames()` accepts batches with `gpst_ms`, `frame_id`,
`frame` (32 bytes carrying 250 MSB-first bits with six zero padding bits),
`crc_valid`, and nullable `accepted`. It re-decodes SBAS content and verifies
CRC in C++ without any knowledge of the source transport. This dictionary
interface remains available for direct callers, alongside typed C++ frame and
interval batches; it is not used by the Parquet hot path. Grid frame IDs
are run-local occurrence counters, not raw byte offsets or Parquet foreign keys.

`GridCnexProcessor(setup_id, gap_timeout_ms)` is the station-level Arrow adapter
used by `ngo-sbas-grid-parquet`. `begin_day(day_gpst_ms)` opens a day;
`events(batch)` gathers its navigation completion context before `feed(batch)`
consumes RawBits. `end_day()` drains the day's pending frames without resetting
signal state, and `finish()` closes streams at the final available context.
Arrow types, Setup identity, canonical bodies, CRC evidence, day membership and
time ordering are checked in native code. Millisecond time projection retains
the existing truncation policy. Feed/drain methods return owned, read-only
structured NumPy interval arrays; no per-frame Python callbacks or JSON
conversion are involved. Native processing releases the GIL and rejects
concurrent use of the same processor. Diagnostics remain a small dictionary.

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
