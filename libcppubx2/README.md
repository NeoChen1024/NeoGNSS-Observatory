# libcppubx2

C++20 UBX framing, generated message decoders, message names, and NAV semantic
helpers, maintained in NeoGNSS Observatory. The POSIX logger is an application
of this library, not its entry point. Future navigation-subframe decoding and
offline tools belong in separate library modules and applications respectively.

## Build and test

Requirements: a C++20 compiler and standard library with `std::format`,
CMake 3.24+, Python 3.11+, the packages listed in
[`requirements-codegen.txt`](requirements-codegen.txt), and the pinned
pyubx2 schema.

From the repository root:

```sh
git submodule update --init contrib/pyubx2
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
build/libcppubx2/neoubxlogger -n -q -f /path/to/input.ubx
```

CMake generates parsers in the build directory; it never installs dependencies
or fetches schemas implicitly. Python is not required by the compiled library
or logger at runtime.

The archive-index application also requires OpenSSL Crypto development files.
Its library scanner API is in `<cppubx2/ubx_archive.hpp>`; the
[reconstruction guide](../docs/ubx-restitch.md) describes the public CLI,
UTC grouping policy, and preservation guarantees.

The library can also be configured directly with `cmake -S libcppubx2 -B build/ubx`.
Use `-DCPPUBX2_BUILD_EXAMPLES=OFF` for a library-only build and
`-DBUILD_TESTING=OFF` to omit tests. Static builds are the default;
`-DBUILD_SHARED_LIBS=ON` builds a shared library. The logger and current
integration tests target POSIX systems. Cross-compilation of tests is not
supported. No install/export package or stable ABI is promised in this version.

## Using the library

Within a CMake parent project:

```cmake
add_subdirectory(path/to/NeoGNSS-Observatory/libcppubx2 cppubx2-build)
target_link_libraries(my_tool PRIVATE cppubx2::cppubx2)
```

Public headers use `#include <cppubx2/ubx.hpp>` and the existing `UBX`
namespace. Individual generated decoders are available through headers such
as `<cppubx2/ubx_rxm_gen.hpp>`. See
[examples/inspect_frame.cpp](examples/inspect_frame.cpp) for a minimal
transport-independent decoder.

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
SFRBX container decoding is available; constellation navigation-subframe
interpretation is not yet implemented.

## Logger compatibility

`examples/ubxlogger.cpp` builds the existing `neoubxlogger` executable:

| Flag | Behavior |
| --- | --- |
| `-f FILE` | File input; stdin is the default |
| `-t HOST:PORT` | TCP input with reconnection |
| `-n` | Disable recording |
| `-d` | Dump every frame to stderr |
| `-q` | Suppress live status; retain periodic statistics |

`-f`/`-t` and `-d`/`-q` remain mutually exclusive. A valid NAV-PVT opens
or rotates recording by full UTC date; preceding frames are not recorded.
NAV-EOE is diagnostic only. Invalid PVT does not change the recording date.
Truncated file input and output flush/close failures return nonzero. TCP uses
the existing byte-at-a-time read and five-second receive timeout, discarding
partial frames on timeout before resynchronizing. This application is not a
lossless offline archive normalizer.

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
