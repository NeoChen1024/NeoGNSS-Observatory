# libcppgnss

C++20 UBX/SBF framing, generated message decoders, message names, and NAV semantic
helpers, maintained in NeoGNSS Observatory. The POSIX logger is an application
of this library, not its entry point. CommonNEX normalization and satellite
navigation-bit content decoding belong to `libneognss-obs`, not this library.

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
build/libcppgnss/neognsslogger -p ubx -n -q -f /path/to/input.ubx
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
as `<cppgnss/ubx_rxm_gen.hpp>`. SBF descriptors/decoders are under `cppgnss::SBF`.

- `read_ubx_frame()` in `ubx_reader.hpp` accepts a caller-owned byte reader,
  distinguishes EOF/truncation/timeout/error, and returns frame bytes without
  sync. Construct `ubx_frame` from those bytes to validate length and checksum.
- This byte-reader API discards partial state on callback failure,
  and a corrupt length can consume a following frame.
  It is not yet an archive-salvage or byte-offset-indexing API.
- The logger uses `StreamDecoder` instead: bounded input chunks, validated
  complete UBX/SBF wire frames and atomic skipping of foreign-protocol frames.
- Successful `ParseResult<T>` means structural decoding succeeded. NAV semantic validity
  is separate. UBX scaled wire fields remain raw values; do not assume they are
  already expressed in physical units or that timestamps are valid UTC.
- Generated UBX messages are independent decoded-data classes, not subclasses
  of `ubx_any_msg`. They retain decoded fields and `dump()`, without
  a duplicate raw payload or inherited frame identity. Keep the source frame
  when raw bytes or transport identity are needed; `ubx_any_msg` remains the
  separate generic raw-message container.
- Typed parsing returns errors as values and never invokes diagnostic callbacks.
  The standalone legacy `ubx_frame` checksum reader still supports its silent-by-default
  `set_parse_error_handler()`; it is not used by the validated-frame path.
- Message `dump()` methods return `std::string`; callers handle text I/O.
  Binary `write(FILE*)` helpers use caller-owned streams; the library never
  opens files, reconnects sockets, or rotates recordings.

The generated schema covers NAV, RXM, MON, TIM, ESF, HNR, LOG, SEC, CFG, and ACK.
Names also cover upstream messages without generated decoders. Debug dispatch
falls back to the original payload for unsupported messages or wire variants.
`ubx_subframe.hpp` provides stateless RXM-SFRBX word extraction with native
constellation, satellite, signal and frequency-slot identity. It does not decode
the satellite navigation content. See the [subframe guide](../docs/subframes.md)
for the Observatory RawBits and SBAS processing layer.

`stream.hpp` provides chunked UBX/SBF framing with checksum validation and
borrowed frame views. `feed()` keeps incomplete frames across calls; call
`finish()` only at a real end of stream. Reader statistics include invalid
frames and noise. No implicit transport or terminal output is performed.
If a frame callback throws, its original exception propagates and the decoder
becomes unusable: subsequent `feed()` and `finish()` calls throw `std::logic_error`.
Construct a new decoder to restart; callback side effects are not rolled back,
and pending bytes from the failed decoder must not be replayed automatically.

SBF codegen covers all 125 pinned blocks plus the retained legacy QZSRawL6
layout in 17 functional groups: 118 have payload definitions and eight explicitly
remain unsupported. Generated headers
such as `sbf_measurement_gen.hpp`, `sbf_pvt_gen.hpp` and `sbf_status_gen.hpp`
expose a concrete type per message. Both protocols use the same API:

```cpp
#include <cppgnss/parse.hpp>
#include <cppgnss/sbf_measurement_gen.hpp>
#include <cppgnss/ubx_rxm_gen.hpp>

auto result = cppgnss::parse<cppgnss::SBF::MeasEpoch>(frame);
if (result) {
    const auto& message = result.value();
    // message.MeasEpochChannelType1 contains the decoded observations.
} else {
    // result.error(): code, optional payload-relative byte offset, detail.
}
// UBX: cppgnss::parse<UBX::ubx_rxm_rawx>(frame)
// Type-agnostic text: cppgnss::dump(frame)
```

`ParseResult<T>` holds either owned decoded data or a `ParseError`, never a
partially decoded message. `value()` supports move extraction;
`consumed()` is available on success. Accessing the wrong result branch throws
`std::logic_error`. Parsing checks protocol, message ID, variant and payload
bounds, but relies on the caller's framing/checksum validation. `StreamDecoder`
provides that validation; fabricating a `FrameView` does not validate its CRC.
No raw-payload copy or second checksum pass is required. Allocation failures
remain exceptions, not malformed-input errors. `decode_payload` is generated
implementation machinery; callers use `parse<T>` for identity checking.

`UbxMessageId` and `SbfMessageId` are generated from the pinned name registries,
including names without typed decoders. Message types declare `protocol`,
`message_id` and `message_name`. IDs do not include firmware revision or payload
variant; unknown numeric IDs remain representable in `FrameView`. SBF revision
stays in the source frame and is not a claim of layout support for every version.

Repeated groups are nested `std::vector<SubBlock>`, conditional groups are
`std::optional<SubBlock>`, and bit fields remain nested under their source field.
For example, `result.value().MeasEpochChannelType1[i].MeasEpochChannelType2[j]`
addresses a secondary observation, and `result.value().CommonFlags.CodeSmoothing`
addresses a flag. Invalid C++ identifier characters become underscores, with a
`field_` prefix when needed; dumps retain original source field names. Binary
fields and wide integers own their bytes. No field-name map is constructed.

`SBF::inspect(id, revision, payload)` provides layout status, consumed length
and optional native TOW/WNc for generic QA. That time is not necessarily a
receiver-navigation anchor. Each message owns a `std::string dump() const`
method that formats its stored fields, preserving nested groups and allowing
message-specific formatting. `cppgnss::dump(frame)` dispatches to these methods
for either protocol, includes raw bytes and errors for failures, and preserves
unconsumed trailing bytes. Output is one line per message, with repeated groups
expanded as nested lists rather than counts. It is human-readable text, not a
stable serialization format. `schemas()` remains descriptor
introspection, not the runtime parsing engine. The former `Block.fields` /
dynamic `decode()` API is removed. The schema does not describe every firmware
revision, and structural decoding is not semantic validation.

Measurements and RawBits adapters use these same generated typed decoders; they
do not maintain a second receiver-payload byte-offset parser. Their remaining
work is signal identity, units, missing-value interpretation, observation
reconstruction and navigation-body normalization/integrity checks. RawBits
navigation checks are distinct from the receiver-frame checksum.

Local codegen supplements preserve protocol details needed by these adapters:

- RAWX exposes its version byte separately from the remaining reserved bytes.
- MeasEpoch accepts revisions 0/1; CumClkJumps is reserved in revision 0 and
  is interpreted as clock-adjustment evidence only for revision 1.
- MeasExtra accepts revisions 0–3. Optional `revision1`, `revision2` and
  `revision3` groups contain fields introduced at those revisions; absent
  groups are not populated with invented zero values. `N` retains its uint8
  wire value, while `group.size()` is the actual count reconstructed using
  sub-block length, payload size and the permitted trailing alignment padding.
- Navigation-page layouts used by RawBits accept revision 0 and require exact
  payload consumption. Legacy QZSRawL6 (4069) uses the retained L6 header/body
  layout, including the unspecified Source=0 case.

The pinned upstream submodules are not edited for these supplements.

Generated navigation-page messages expose native receiver fields and body words.
Canonical repacking, independent navigation-body checks, service classification,
and SBAS correction contents are handled in `libneognss-obs`.

## UBX/SBF logger

`examples/gnsslogger.cpp` builds `neognsslogger`.

| Flag | Behavior |
| --- | --- |
| `-p ubx\|sbf`, `--protocol` | Input protocol; default UBX |
| `-f FILE` | File input; stdin is the default |
| `-t HOST:PORT` | TCP input with reconnection |
| `-n` | Disable recording |
| `-d` | Dump every accepted frame's decoded fields to stderr |
| `-q` | Suppress live status; retain periodic statistics |
| `--expected-period-ms N` | Expected navigation cadence; default 1000 ms, use 100 for 10 Hz or 33 for approximately 30 Hz |
| `--epoch-interval-ms N` | Alias for `--expected-period-ms` |
| `--expected-measurement-period-ms N` | SBF EndOfMeas cadence; defaults to the navigation period, independently configurable |
| `--epoch-tolerance-percent N` | Symmetric interval tolerance; default ±20%, configurable from 0 to 99% |
| `--expect-nav-clock` | UBX only: warn when an EOE interval contains no valid-length NAV-CLOCK |
| `--disk-buffer-mib N` | Bounded background disk buffer; default 64 MiB, minimum 1 MiB |
| `OUTPUT_DIR` | Optional first positional argument; recording root, default `./` |

For UBX, the periodic `FIX` percentage counts usable 2D, 3D, GNSS+dead-reckoning and
TIME-only solutions (`fixType` 2–5), always requiring `gnssFixOK`. A receiver
operating in timing mode can therefore report `FIX 100%` without a 2D/3D
position solution. No-fix, dead-reckoning-only, reserved types and solutions
without `gnssFixOK` are not counted as successes. The denominator remains
the semantically valid NAV-PVT messages received during the statistics period.

SBF reports PVT Mode, Error and satellite count from PVTCartesian/PVTGeodetic.
Its FIX numerator requires Error=0 and Mode Type 1–8 or 10, including fixed
location (3); no-solution and reserved types are excluded. Each received PVT
block contributes to the denominator, including both variants if both are enabled.

`-d` prints the strings returned by UBX/SBF message dumps. SBF displays block name,
revision, schema status and decoded fields (including bit fields and
indexed repeated fields). Binary and wide-integer values use hex. Unknown blocks,
private schemas and schema decoding failures include the raw payload as hex;
decoded trailing bytes are also displayed. Schema coverage is limited by the
pinned definitions; this is not a claim that every revision is fully decoded.
CRC-valid blocks remain recordable regardless of schema decoding support.
`-n -d` requires neither recording time anchors nor epoch-completion messages.

The output root and monthly subdirectories are created when the first complete
epoch is recorded. Relative and absolute paths are supported. `-n` does not
create the output directory. More than one positional argument is an error.

A single background I/O thread owns directory creation, file opening, ordered
writes and day rotation. Completed recording intervals enter a bounded RAM queue;
`--disk-buffer-mib` counts allocated payload capacity both queued and actively
being written. Queue metadata, stdio buffering and the separate incomplete-epoch
buffer are additional memory. The receive thread does not wait for free queue
space: exhaustion stops recording with an explicit error rather than silently
dropping data or blocking reception. This absorbs temporary disk stalls, not
sustained insufficient throughput. Debug/status output still uses stderr directly.

At normal EOF, accepted writes are drained and close errors are checked before
success is reported. On a parsing or buffer-exhaustion error, already accepted
writes are drained before exiting unsuccessfully; the rejected interval is not
written. Disk errors stop the writer and are reported to the receive loop or at
shutdown. Draining can wait on a stalled filesystem. This is not durable storage:
abrupt termination or power loss can lose queued/buffered bytes.

`-f`/`-t` and `-d`/`-q` remain mutually exclusive. When recording UBX, frames are buffered until
NAV-EOE. Every EOE requires a fresh valid NAV-TIMEGPS with the same iTOW;
missing, invalid, conflicting or non-increasing time causes a nonzero exit.
The entire epoch, including EOE, is written to its GPST day. NAV-PVT calendar
fields never select the output date. Outputs are named
`YYYY-MM/GPST-%Y-%m-%d--%H-%M-%S-mmm.ubx`, using the first recorded epoch
with exactly three millisecond digits, including `000`.
Nominal iTOW defines the epoch boundary; fTOW remains unchanged in the raw
message and is not used to move a nominal epoch across a day boundary.

SBF recording buffers frames through EndOfPVT and uses its valid GPST WNc/TOW
to flush/rotate. It requires EndOfPVT on the selected output stream, not an
extra TIMEGPS-equivalent message. EndOfMeas is a separate measurement diagnostic
boundary, never the recording boundary. SBF output uses the same naming rule
with `.sbf`. RawBits/SIS timestamps never control rotation or monotonicity.
Frames following a completion marker belong to the next recording interval;
the logger does not reorder asynchronous blocks or claim all enclosed blocks
share the marker's timestamp. Input wire bytes are preserved without repacking.

EOF with an incomplete recording interval fails without writing that interval.
The pending epoch buffer is limited to 64 MiB; exceeding it fails explicitly.
Truncated file input and output flush/close failures return nonzero. TCP uses
bounded 64 KiB reads and a five-second receive timeout. A timeout/disconnect
explicitly reports and discards the unfinished frame and recording interval,
then resynchronizes; completed intervals are retained. Reconnection retries
are two seconds apart. Valid foreign-protocol frames are warned and skipped;
invalid transport frames and discarded noise are reported. Existing output
files are never overwritten. This application is not a lossless offline archive
normalizer. Recording files are buffered and remain open within a GPST day;
default signal termination does not guarantee the last buffered bytes are flushed.

### Overnight continuity diagnostics

```sh
neognsslogger -p ubx -q -t RECEIVER_HOST:PORT --expect-nav-clock /data/receiver-test \
  2> receiver-test.log
```

Enable NAV-TIMEGPS and NAV-EOE on the receiver's selected output stream;
enable NAV-CLOCK when using `--expect-nav-clock`. The logger does not configure
the receiver. Add `-n` for diagnostics without recording. Keep the diagnostic
log outside a not-yet-created output root, since the shell opens it first.

For Septentrio, enable EndOfPVT; enable Measurements/EndOfMeas for observation
continuity diagnostics and PVTGeodetic or PVTCartesian for live status:

```sh
neognsslogger -p sbf -q -t RECEIVER_HOST:PORT /data/sbf
neognsslogger -p sbf -n -d -f /path/to/recording.sbf
```

Navigation and measurement diagnostics have independent histories, labeled by
`axis`. If PVT is 1 Hz and measurements are 10 Hz, set
`--expected-period-ms 1000 --expected-measurement-period-ms 100`.
MeasEpoch changes without EndOfMeas and mismatched EndOfMeas are warnings.

`[logger qc] epoch_gap` reports previous/current full GPST labels, epoch interval,
expected interval, and the monotonic host completion-marker arrival interval. Missing epoch
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

Cadence and message-presence warnings do not drop frames, insert epochs,
change rotation, or stop recording. Transport failures discard pending bytes
as described above. Missing/mismatched TIMEGPS, missing recording completion
and non-increasing GPST remain fatal
according to the recording policy; diagnostics do not silently recover them.
These recording requirements do not apply to `-n` inspection. The host arrival
interval measures application reception, not UART transmission
or network latency in isolation. A gap cannot by itself identify the receiver,
bridge, network, or gpsd as the cause. Compare equivalent receiver configurations
and retain raw UBX/SBF alongside the log for investigation.

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
