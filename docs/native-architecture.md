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
| GPS observation normalization without RTKLIB types | `libcppgnss` |
| Static GPS Float PPP adapter, filter and residual batches | `libneognss-obs`, linked to RTKLIB-EX |
| GPS STEC geometry, phase leveling and receiver DCB estimation | `libneognss-obs`, linked to RTKLIB-EX |
| Batch Python binding | `libneognss-obs` |
| Source selection, overlap byte I/O/proofs, publication and Parquet | Python |
| Numerical table reductions, rendering and parallel PNG export | Python/NumPy |

Python calls native processing through batch bindings. RTKLIB conversion,
RxTools and FFmpeg are external tools. `neoubxlogger` is the standalone recording
application. The raw-observation STEC and PPP paths link RTKLIB as a library;
they do not invoke conversion executables or write a RINEX observation intermediate.

## Batch and state semantics

Python reads bounded chunks and writes output products. Native code performs
framing, decoding and per-message state updates without calling Python for each
raw frame. pybind11 releases the GIL during native processing and converts final
record batches after reacquiring it. Currently, clock/subframe/grid paths use
native JSON trees converted to Python lists/dictionaries; PPP/STEC numerical
results use copied NumPy structured arrays. Some settings and summaries also
use JSON text conversion. These are current implementations, not the selected
CommonNEX interchange design below. Memory use depends on batch sizes.

Batch boundaries have no scientific meaning. File and GPST-day boundaries do
not reset clock or SBAS history. Explicit timeout, observed MON-SYS runtime
decrease, SBAS message/mask validity and explicit continuous-stream policy
control resets. Unknown time stays unknown. GPST remains the only project time
axis; native receiver fields remain in raw archives and protocol decoder APIs.

`ngo-dataset-qa` offers read-only `scan` (default) and explicit `restitch`.
Both share native UBX epoch interpretation, but only reconstruction computes
overlap fingerprints/indexes. SBAS extraction also uses that epoch assembler
without QA diagnostics; it requires neither reconstruction nor proof of a QA
run. Receiver-clock reads raw or reconstructed recordings directly too.

SBAS frame Parquet is the source-independent boundary: 250-bit message body,
GPST, canonical signal identity, CRC/acceptance, and explicit continuity/end
records. No raw envelopes, field dictionaries or source-offset mappings are
persisted in it. Grid reads only these files, re-decodes SBAS in native batches,
and links intervals to frame IDs. Daily partitioning never resets state.

## Selected CommonNEX interop design

The planned Ginan backend uses one context per worker process; see
[Ginan shim design and progress](ginan-shim.md). This is a selected direction,
not a change to the currently linked PPP/STEC implementation.

Status: agreed implementation direction, not yet wired into the bindings.
`contrib/arrow-nanoarrow` is available as a pinned submodule. CommonNEX remains
a logical specification independent of Arrow, while this project's native/Python
implementation will use Arrow-compatible columnar batches.

| Component | Responsibility |
| --- | --- |
| `libcppgnss` | Protocol decoding; no Arrow or Python dependency |
| `libneognss-obs` | Typed batch production/consumption and scientific state |
| Binding/interop layer | nanoarrow helpers, Arrow C Data Interface and Python PyCapsule exchange |
| Python/PyArrow | RecordBatch orchestration, Parquet encoding/decoding, partitioning and publication |

Keep pybind11 for high-level objects and processing calls. Use nanoarrow's C
Data Interface helpers rather than requiring the full Arrow C++ library.
Do not add a native Parquet writer or make JSON the bulk-data interchange.
Small settings, summaries and diagnostics may still use dictionaries/JSON.

### Bidirectional batches

Native output exports typed columns through Arrow C Data Interface capsules
for Python to consume as RecordBatches. Parquet replay passes Arrow-compatible
batches back into native processing without `to_pylist()` or per-row objects.
Use the Arrow PyCapsule array/batch protocol; a stream protocol can expose a
sequence when needed. This is in-process buffer exchange, not Arrow IPC byte
serialization or a network transport.

Use contiguous numeric columns and validity bitmaps for nullable fields.
Variable-length binary/string fields use offsets and data buffers; fixed-size
binary is appropriate for fixed-length family payloads. Preserve unsigned GPST
integers, units, rational scales, identities and quality semantics. Integer GPST
must not become Arrow UTC timestamps or pass through floating point.

Validate incoming schema, lengths, offsets, nullability and supported types
before native access. Account for sliced arrays and their offsets. Unsupported
layouts require an explicit error or documented conversion, not reinterpretation
of arbitrary memory. A scalar metadata object per batch is acceptable; a Python
object per scientific sample is not the intended path.

### Ownership, state and performance

- Exported buffers remain immutable and alive until consumers release them.
  Owners and release callbacks must prevent reuse, double release and dangling
  references, including when a Python batch outlives its native producer.
- Input owners remain alive throughout native processing. Async retention, if
  implemented, must retain ownership beyond the initiating call explicitly.
- Acquire Python objects/capsules while holding the GIL; release it for native
  work on owned buffers. Never access Python objects from a released-GIL loop.
- Bound batches by records/bytes, not a complete day or Era. Consumers control
  how many batches remain in flight; batch delivery does not reset scientific
  state or require a Parquet write before the next processing step.
- Aim to avoid redundant boundary copies, not promise end-to-end zero-copy.
  Decoding, conversion from existing row-oriented structs, and Parquet
  encoding/compression may allocate or copy. Measure throughput and peak memory
  on equivalent content before claiming a performance improvement.

This supports direct processing, Parquet persistence and replay with the same
logical inputs. See [CommonNEX pipeline](commonnex/overview.md#processing-pipeline)
and [ParquetNEX](commonnex/parquetnex.md). No existing product schema or binding
is changed merely by adopting this design.

References: [Arrow PyCapsule interface](https://arrow.apache.org/docs/format/CDataInterface/PyCapsuleInterface.html)
and [nanoarrow](https://arrow.apache.org/nanoarrow/latest/index.html).

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
parser/batch API feeds `ngo-sbas-frame-parquet -p sbf`, using native TOW/WNc and
receiver CRC status. UBX and SBF adapters share the same grid processor and
protocol-neutral Parquet identity fields (`constellation`, `prn`, `signal`).
The SBF path uses a configurable per-signal reception-gap policy; file and
GPST day boundaries do not reset state. Schema coverage does not establish
full-archive scientific validity.

SBF word layout follows the pinned RTKLIB-EX `src/rcv/septentrio.c`
GEORaw decoder; field definitions come from pinned pysbf2.

## Build dependencies

`contrib/pyubx2` and `contrib/pysbf2` supply build-time definitions and retain
their BSD-3-Clause notices. `contrib/json` supplies nlohmann/json under its MIT
license for native in-memory records. Most JSON file serialization remains in
Python. The generic protocol library does not depend on JSON or Python at runtime.
The analysis extension uses pybind11, OpenSSL Crypto and the pinned RTKLIB-EX
core. Its precise-product parsing and PPP calls are serialized within each
process because the core contains shared caches. No C++ plotting or
Parquet dependency is introduced.

`contrib/arrow-nanoarrow` retains its Apache-2.0 license. It is the selected
interop helper dependency, currently vendored as a submodule but not yet linked
by the build. Python retains Parquet I/O through PyArrow.
