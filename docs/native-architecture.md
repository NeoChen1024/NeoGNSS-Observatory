# Native protocol and processing boundaries

The dependency direction is Python analysis → `libneognss-obs` → `libcppgnss`.
The logger depends directly on `libcppgnss`.

| Responsibility | Owner |
| --- | --- |
| UBX/SBF framing, checksums, field definitions and decoding | `libcppgnss` |
| SFRBX/GEORawL1 extraction, SBAS L1 bits, MT18/26, IGP coordinate rules | `libcppgnss` |
| Full available SBF schema generation | `libcppgnss`, from pinned `contrib/pysbf2` |
| GPST epoch association, archive segmentation and quarantine policy | `libneognss-obs` |
| Receiver time association and restart evidence | `libneognss-obs` |
| Clock unwrap, adjustment inference, temperature association | Python/NumPy over CommonNEX |
| SBAS mask completeness, correction/mask ages, grid resets | `libneognss-obs` |
| GPS observation normalization without RTKLIB types | `libcppgnss` |
| Static GPS Float PPP adapter, filter and residual batches | `libneognss-obs`, linked to RTKLIB-EX |
| GPS STEC geometry, phase leveling and receiver DCB estimation | `libneognss-obs`, linked to RTKLIB-EX |
| Batch Python binding | `libneognss-obs` |
| Source selection, overlap byte I/O/proofs, publication and Parquet | Python |
| Numerical table reductions, rendering and parallel PNG export | Python/NumPy |

Python calls native processing through batch bindings. RTKLIB conversion,
RxTools and FFmpeg are external tools. `neoubxlogger` is the standalone recording
application. CommonNEX-input STEC and raw-input PPP link RTKLIB as a library;
they do not invoke conversion executables or write a RINEX observation intermediate.

## Batch and state semantics

Python reads bounded chunks and writes output products. Native code performs
framing, decoding and per-message state updates without calling Python for each
raw frame. pybind11 releases the GIL during native processing and converts final
record batches after reacquiring it. Currently, subframe/grid paths use
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
run. Receiver-clock consumes CommonNEX telemetry and Events only; its shared
vectorized unwrap also powers re-unwrapping of derived Parquet. It has no raw
clock scanner or protocol-selection fallback.

SBAS frame Parquet is the source-independent boundary: 250-bit message body,
GPST, canonical signal identity, CRC/acceptance, and explicit continuity/end
records. No raw envelopes, field dictionaries or source-offset mappings are
persisted in it. Grid reads only these files, re-decodes SBAS in native batches,
and links intervals to frame IDs. Daily partitioning never resets state.

## Selected CommonNEX interop design

The planned Ginan backend uses one context per worker process; see
[Ginan shim design and progress](ginan-shim.md). This is a selected direction,
not a change to the currently linked PPP/STEC implementation.

Status: native-to-Python output is wired into the CommonNEX importer.
`StecCnexReader` accepts replayed Observation Arrow batches directly, maps the
selected GPS pair and quality into the numerical engine, and retains a pending
measurement epoch across batches. Other analysis-binding migrations remain pending.
STEC checkpoints preserve native arc state between daily invocations; end of an
invocation is not end of the scientific stream. Python owns product selection,
daily sample persistence, affected-window DCB refitting and incremental plots.
`contrib/arrow-nanoarrow` is available as a pinned submodule. CommonNEX remains
a logical specification independent of Arrow, while this project's native/Python
implementation uses Arrow-compatible columnar batches in `CnexObservationReader`.
See [the importer pilot](commonnex/importer.md) for its restricted coverage.

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
binary is appropriate for fixed-length family payloads. Preserve canonical
GPST `decimal128(38,12)` seconds, units, rational scales, identities and quality
semantics. Absolute GPST must not become Arrow UTC timestamps or pass through
floating point.

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
adapter feeds `ngo-cnex-import run -p sbf` through native Arrow batches, using
receiver navigation TOW/WNc (not SIS timestamps) and separate receiver/independent
CRC checks. RawBits uses a 10-period receiver-time timeout and nullable
GPST, without a time-waiting backlog; EOE does not clear its anchor.
[Shared receiver time](commonnex/telemetry-time.md) retains uptime and restart
boundaries. Measurement time only advances the timeout high-water mark. UBX and SBF produce
the same CommonNEX SBAS L1 RawBits layout. Grid reads these records and Events;
its derived products retain RINEX `satellite_system`, `satellite_number` and
`signal` fields, displaying SBAS identities as Sxx throughout.
The grid uses a configurable reception-gap policy; file and
GPST day boundaries do not reset state. Schema coverage does not establish
full-archive scientific validity.

SBF word layout follows the pinned RTKLIB-EX `src/rcv/septentrio.c`
GEORaw decoder; field definitions come from pinned pysbf2.

## Selected native GPST representation

Design decision; the existing processing APIs and artifacts have not yet been
converted. CommonNEX canonical timestamps use `DECIMAL(38,12)` seconds since
1980-01-06 00:00:00 GPST. The observation pilot already uses Boost.Int128 ticks
with exact binary64-to-picosecond rounding and explicit Arrow word transfer.
The broader native implementation will use a small `GpstTime`
type backed by `boost::int128::int128` from `contrib/int128`. Its internal
unscaled integer counts picoseconds: `1000000000000` ticks represents one
second. A distinct `GpstDuration` represents signed time differences; timestamps
and durations must not be accidentally interchangeable.

Keep comparison, checked addition/subtraction, decimal input/output and
round-half-to-even input quantization behind this thin interface. Preserve
integer week/second contributions during conversion; never construct large
absolute float64 seconds as an intermediate. Convert relative intervals to
floating point only when a calculation needs it. Boost.Int128 provides integer
arithmetic, not automatic overflow checking, GPST semantics or decimal scaling.
Validate both signed-128 arithmetic limits and the narrower 38-digit decimal
range; absolute GPST remains nonnegative. Validate input syntax and range before
conversion rather than relying on wrapping integer arithmetic.

nanoarrow supplies `ArrowSchemaSetTypeDecimal`, `ArrowDecimal`,
`ArrowArrayAppendDecimal` and `ArrowArrayViewGetDecimalUnsafe` for exchange.
Set the schema to `NANOARROW_TYPE_DECIMAL128`, precision 38, scale 12. Transfer
the complete unscaled 128-bit value through an explicit byte/word adapter with
defined sign and endianness handling. Do not reinterpret a Boost object as an
Arrow buffer or use the int64-only `ArrowDecimalSetInt` for absolute epochs.
Preserve array offsets, nullability and buffer ownership on replay. nanoarrow
is not the arithmetic engine; no full Arrow C++ dependency is required.

Boost types and native tick storage are implementation details, not CommonNEX
requirements. Before integration, validate decimal round trips, negative
durations, rounding/carry, range checks and native/PyArrow exchange using small
representative inputs. This decision does not introduce a new executable backend.

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
interop helper dependency, linked by the native extension for CommonNEX import
and replay. Python retains Parquet I/O through PyArrow.

`contrib/int128` supplies Boost.Int128 under BSL-1.0. It is header-only and
requires no other Boost libraries. It is the selected native time-arithmetic
dependency, pinned as a submodule and used by CommonNEX import/replay. Preserve its
upstream license and keep its implementation details behind the time interface.
