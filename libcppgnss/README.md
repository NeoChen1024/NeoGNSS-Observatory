# libcppgnss

C++20 UBX/SBF framing, generated message decoders, message names, and NAV semantic
helpers, maintained in NeoGNSS Observatory. The POSIX logger is an application
of this library, not its entry point. Navigation-subframe routing and SBAS L1
decoding live in separate library modules, independent of transport and logging.

## Build and test

Requirements: a C++20 compiler and standard library with `std::format`,
CMake 3.24+, Python 3.11+, the packages listed in
[`requirements-codegen.txt`](requirements-codegen.txt), and the pinned
pyubx2 and pysbf2 schemas.

From the repository root:

```sh
git submodule update --init contrib/pyubx2 contrib/pysbf2 contrib/json
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
build/libcppgnss/neoubxlogger -n -q -f /path/to/input.ubx
```

CMake generates parsers in the build directory; it never installs dependencies
or fetches schemas implicitly. Python is not required by the compiled library
or logger at runtime.

Observatory-specific processing lives in [libneognss-obs](../libneognss-obs/README.md),
not in this protocol library. The root build includes its Python extension and
requires pybind11 and OpenSSL Crypto development files; a direct library-only
build does not require either dependency.

The library can also be configured directly with `cmake -S libcppgnss -B build/ubx`.
Use `-DCPPGNSS_BUILD_EXAMPLES=OFF` for a library-only build and
`-DBUILD_TESTING=OFF` to omit tests. Static builds are the default;
`-DBUILD_SHARED_LIBS=ON` builds a shared library. The logger and current
reader checks target POSIX systems. Cross-compilation of tests is not
supported. No install/export package or stable ABI is promised in this version.

## Using the library

`StreamDecoder` selects UBX or SBF explicitly. It validates and skips complete
frames of the other protocol atomically, exposing skipped frame/byte counters
without printing diagnostics. Callers decide how to report these counters.
Embedded synchronization bytes inside a valid foreign frame are not decoded.

Within a CMake parent project:

```cmake
add_subdirectory(path/to/NeoGNSS-Observatory/libcppgnss cppgnss-build)
target_link_libraries(my_tool PRIVATE cppgnss::cppgnss)
```

Public headers use `#include <cppgnss/ubx.hpp>` and the existing `UBX`
namespace. Individual generated decoders are available through headers such
as `<cppgnss/ubx_rxm_gen.hpp>`. SBAS content is under `cppgnss::SBAS`,
and SBF descriptors/decoders under `cppgnss::SBF`.

- `read_ubx_frame()` in `ubx_reader.hpp` accepts a caller-owned byte reader,
  distinguishes EOF/truncation/timeout/error, and returns frame bytes without
  sync. Construct `ubx_frame` from those bytes to validate length and checksum.
- The initial reader preserves the logger's framing policy: callback failure
  discards partial state, and a corrupt length can consume a following frame.
  It is not yet an archive-salvage or byte-offset-indexing API.
- Generated `valid` means structural decoding succeeded. NAV semantic validity
  is separate. Scaled wire fields remain raw values; do not assume they are
  already expressed in physical units or that timestamps are valid UTC.
- Diagnostics are silent by default. `set_parse_error_handler()` installs a
  per-thread callback and returns the previous handler. Message views are
  borrowed only for the duration of the callback.
- Explicit `dump(FILE*)` and `write(FILE*)` helpers use caller-owned streams;
  the library never opens files, reconnects sockets, or rotates recordings.

The generated schema covers NAV, RXM, MON, TIM, ESF, HNR, LOG, SEC, CFG, and ACK.
Names also cover upstream messages without generated decoders. Debug dispatch
falls back to the original payload for unsupported messages or wire variants.
`ubx_subframe.hpp` provides RXM-SFRBX routing by constellation, satellite,
signal, and GLONASS frequency slot. `sbas.hpp` decodes SBAS L1 frames with
CRC-24Q validation. Other constellations retain their raw navigation words.
See the [subframe guide](../docs/subframes.md) for supported SBAS message types,
API examples, limitations, and the Python batch exporter.

`stream.hpp` provides chunked UBX/SBF framing with checksum validation and
borrowed frame views. `feed()` keeps incomplete frames across calls; call
`finish()` only at a real end of stream. Reader statistics include invalid
frames and noise. No implicit transport or terminal output is performed.

`sbf.hpp` exposes all 125 pinned pysbf2 block descriptors: 117 have payload
definitions; eight explicitly remain unsupported. Decoding returns typed field
values, original payload, revision, status and unconsumed tail. Repeated fields
use upstream-style `_01` suffixes; wide integers and raw byte fields retain
precision. Unknown blocks remain raw. The schema does not describe every
firmware revision, and structural decoding is not semantic validation.

`cppgnss::SBAS::parse_l1()` accepts 32 MSB-first bytes holding 250 air bits.
`UBX::parse_sbas()` adapts SFRBX; `cppgnss::SBF::extract_sbas_l1()` adapts
GEORawL1, preserving receiver CRC status and native SBF fields. GEORawL5 is
never silently passed through the L1 decoder.

## Logger compatibility

`examples/ubxlogger.cpp` builds the existing `neoubxlogger` executable:

| Flag | Behavior |
| --- | --- |
| `-f FILE` | File input; stdin is the default |
| `-t HOST:PORT` | TCP input with reconnection |
| `-n` | Disable recording |
| `-d` | Dump every frame to stderr |
| `-q` | Suppress live status; retain periodic statistics |
| `--expected-period-ms N` | Expected navigation cadence; default 1000 ms, use 100 for 10 Hz or 33 for approximately 30 Hz |
| `--epoch-interval-ms N` | Alias for `--expected-period-ms` |
| `--epoch-tolerance-percent N` | Symmetric interval tolerance; default ±20%, configurable from 0 to 99% |
| `--expect-nav-clock` | Warn when an EOE interval contains no valid-length NAV-CLOCK |
| `OUTPUT_DIR` | Optional first positional argument; recording root, default `./` |

The periodic `FIX` percentage counts usable 2D, 3D, GNSS+dead-reckoning and
TIME-only solutions (`fixType` 2–5), always requiring `gnssFixOK`. A receiver
operating in timing mode can therefore report `FIX 100%` without a 2D/3D
position solution. No-fix, dead-reckoning-only, reserved types and solutions
without `gnssFixOK` are not counted as successes. The denominator remains
the semantically valid NAV-PVT messages received during the statistics period.

The output root and monthly subdirectories are created when the first complete
epoch is recorded. Relative and absolute paths are supported. `-n` does not
create the output directory. More than one positional argument is an error.

`-f`/`-t` and `-d`/`-q` remain mutually exclusive. Frames are buffered until
NAV-EOE. Every EOE requires a fresh valid NAV-TIMEGPS with the same iTOW;
missing, invalid, conflicting or non-increasing time causes a nonzero exit.
The entire epoch, including EOE, is written to its GPST day. NAV-PVT calendar
fields never select the output date. Outputs are named
`YYYY-MM/GPST-%Y-%m-%d--%H-%M-%S-mmm.ubx`, using the first recorded epoch
with exactly three millisecond digits, including `000`.
Nominal iTOW defines the epoch boundary; fTOW remains unchanged in the raw
message and is not used to move a nominal epoch across a day boundary.
EOF with an incomplete recording epoch fails without publishing that epoch.
The pending epoch buffer is limited to 64 MiB; exceeding it fails explicitly.
Truncated file input and output flush/close failures return nonzero. TCP uses
the existing byte-at-a-time read and five-second receive timeout, discarding
partial frames on timeout before resynchronizing. This application is not a
lossless offline archive normalizer.

### Overnight continuity diagnostics

```sh
neoubxlogger -q -t RECEIVER_HOST:PORT --expect-nav-clock /data/receiver-test \
  2> receiver-test.log
```

Enable NAV-TIMEGPS and NAV-EOE on the receiver's selected output stream;
enable NAV-CLOCK when using `--expect-nav-clock`. The logger does not configure
the receiver. Add `-n` for diagnostics without recording. Keep the diagnostic
log outside a not-yet-created output root, since the shell opens it first.

`[logger qc] epoch_gap` reports previous/current full GPST labels, epoch interval,
expected interval, and the monotonic host EOE arrival interval. Missing epoch
counts are estimates under the configured fixed cadence, reported only when
the interval matches a multiple of that cadence within tolerance. Off-cadence
intervals are reported separately; cadence changes are not automatically learned.
The accepted interval range is inclusive: 800–1200 ms at the default 1000 ms
period, or 26.4–39.6 ms at a 33 ms period. Comparisons retain these fractional
bounds even though iTOW itself has integer-millisecond resolution. For estimated
missing counts, the residual from the nearest multiple must be within ±20%
of one expected period (or the configured percentage), not of the whole outage.
This check is independent of re-stitch's 50-second segmentation timeout: at
1 Hz a missing second must remain visible in the diagnostic log.

NAV-CLOCK absence (opt-in), duplicate messages and a last CLOCK iTOW differing
from EOE are warnings. TCP timeouts/reconnection attempts and checksum failures
carry a monotonic elapsed time and last accepted GPST epoch. Existing discarded
byte and parsing diagnostics remain enabled. Totals are printed with periodic
statistics and on normal C++ scope exit, including ordinary fatal errors.
Default SIGINT/SIGTERM handling is unchanged: retain periodic totals and
individual warnings; a final summary is not guaranteed on signal termination.

Warnings do not drop frames, insert epochs, change rotation, or stop recording.
Missing/mismatched TIMEGPS, missing EOE and non-increasing GPST remain fatal
according to the recording policy; diagnostics do not silently recover them.
The host arrival interval measures application reception, not UART transmission
or network latency in isolation. A gap cannot by itself identify the receiver,
bridge, network, or gpsd as the cause. Compare equivalent receiver configurations
and retain raw UBX alongside the log for investigation.

## Source provenance and licensing

Imported handwritten source, generator, and regression tests originated in
`rpi-gnss-server`, commit `c08579d67a9fecebcd55e5e95cde9ff9d09378bb`,
under `neoubxlogger/` and `scripts/generate_ubx_parsers.py`. That repository
is historical; all further parser and logger maintenance happens here.
Imported BSD-3-Clause notices and [LICENSE](LICENSE) are retained. New
project-owned additions use the repository's GPL-3.0-only license.

The initial schema revision is pyubx2
`a56fbc16aa68e3e8a90af5abce32d0be7dc3016b`, matching the historical build.
The authoritative dependency revision is the parent repository's submodule
gitlink. Preserve pyubx2's own license and attribution, including when
distributing generated artifacts. Record repository/submodule commits,
compiler, build options, and actual code-generation dependency versions in
production provenance. pyubx2 is not needed at C++ runtime.
