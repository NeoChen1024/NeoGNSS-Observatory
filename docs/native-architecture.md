# Native protocol and processing boundaries

The dependency direction is Python analysis → `libneognss-obs` → `libcppgnss`.
The logger depends directly on `libcppgnss`.

| Responsibility | Owner |
| --- | --- |
| UBX/SBF framing, checksums, field definitions and decoding | `libcppgnss` |
| SFRBX/GEORawL1 extraction, SBAS L1 bits, MT18/26, IGP coordinate rules | `libcppgnss` |
| Full available SBF schema generation | `libcppgnss`, from pinned `contrib/pysbf2` |
| GPST epoch association, archive segmentation and quarantine policy | `libneognss-obs` |
| Clock adjustments, runtime-decrease restarts, temperature association | `libneognss-obs` |
| SBAS mask completeness, correction/mask ages, grid resets | `libneognss-obs` |
| Batch Python binding | `libneognss-obs` |
| Source selection, overlap byte I/O/proofs, publication and Parquet | Python |
| Numerical table reductions, rendering and parallel PNG export | Python/NumPy |

This split does not require moving efficient array operations or external-tool
orchestration into C++. RTKLIB conversion, RxTools and FFmpeg remain external
tools; they are not the removed internal UBX worker protocol. The separate
experimental RTKLIB RINEX-geometry executable has not yet been migrated into
this binding; it is not a protocol-library application.

The Python pipeline now calls native processing directly rather than launching
archive-index, clock-scan or subframe-export worker executables. Those CLI shells
and the unused inspection example are removed. The actual UBX logger and its
existing CLI behavior remain.

## Batch and state semantics

Python reads bounded chunks and writes output products. Native code performs
framing, decoding and per-message state updates without calling Python for each
raw frame. pybind11 releases the GIL during native processing and converts final
record batches after reacquiring it. The initial representation uses owned
records/byte buffers, not C++ Arrow, a promised zero-copy ABI or Python classes
for every wire message. Memory use depends on caller-selected batch sizes.

Batch boundaries have no scientific meaning. File and GPST-day boundaries do
not reset clock or SBAS history. Explicit timeout, observed MON-SYS runtime
decrease, SBAS message/mask validity and reconstruction continuous-group policy
control resets. Unknown time stays unknown. GPST remains the only project time
axis; native receiver fields remain in raw archives and protocol decoder APIs.

SBAS frame Parquet is the source-independent boundary: 250-bit message body,
GPST, canonical signal identity, CRC/acceptance, and explicit continuity/end
records. No raw envelopes, field dictionaries or source-offset mappings are
persisted in it. Grid reads only these files, re-decodes SBAS in native batches,
and links intervals to frame IDs. Daily partitioning never resets state.

## SBF schema coverage and limitations

The generated descriptor tree covers every block in the pinned `SBF_BLOCKS`
dictionary: currently 125 named blocks, including 117 defined payloads and eight
empty definitions. Generation supports nested repetition, field-controlled
counts, conditional groups, LSB-first bitfields, sub-block length padding,
remaining-payload byte fields, floating point and wide integers. Generated
files stay in the build directory and are never edited manually.

Empty/proprietary definitions return `unsupported_schema`; unknown IDs return
`unknown_block`. Bounds/structure errors return `invalid_payload`, with an error
and original payload retained. Every block retains its revision and undecoded
tail. A decoded status means the pinned schema could be read, **not** that every
receiver/firmware revision, no-data sentinel or physical interpretation has been
validated. The upstream dictionary is not a complete revision history. Native
wire values are not silently converted to UTC or replacement no-data values.

SBF `GEORawL1` is adapted from its eight little-endian words to the same 250-bit
MSB-first SBAS L1 representation used by UBX. `CRCPassed`, raw `SVID`/`SigIdx`,
channel fields and TOW/WNc remain available; independent SBAS CRC verification
is also retained. SBAS satellite identity is normalized separately from the
raw receiver identifiers. `GEORawL5` is never passed to the L1 parser. The SBF
parser/batch API feeds `sbas-frame-parquet -p sbf`, using native TOW/WNc and
receiver CRC status. UBX and SBF adapters share the same grid processor and
protocol-neutral Parquet identity fields (`constellation`, `prn`, `signal`).
The SBF path uses a configurable per-signal reception-gap policy; file and
GPST day boundaries do not reset state. Full Era C validation remains separate
from small-sample pipeline validation.

The word layout is cross-checked against the pinned RTKLIB-EX
`src/rcv/septentrio.c` GEORaw decoder; field definitions come from pinned pysbf2.
No upstream sources were modified.

## Build dependencies

`contrib/pyubx2` and `contrib/pysbf2` supply build-time definitions and retain
their BSD-3-Clause notices. `contrib/json` supplies nlohmann/json under its MIT
license for native in-memory records. Most JSON file serialization remains in
Python. The generic protocol library does not depend on JSON or Python at runtime.
The analysis extension uses pybind11 and OpenSSL Crypto; no C++ plotting or
Parquet dependency is introduced.
