# Native protocol and processing boundaries

The dependency direction is Python analysis → `libneognss-obs` → `libcppgnss`.
The standalone `neognsslogger` depends directly on `libcppgnss`.

| Component | Responsibility |
| --- | --- |
| `libcppgnss` | UBX/SBF/RTCM3/NMEA mixed framing, integrity checks, native message fields and typed protocol APIs |
| `libneognss-obs` | CommonNEX normalization, receiver-time association, canonical RawBits, satellite-content decoding and scientific processing |
| Bindings | High-level processing APIs and owned batch exchange |
| Python | File orchestration, product selection, Parquet storage, analysis and visualization |

Receiver-protocol decoding ends at receiver message fields. Satellite
air-interface interpretation, including SBAS content and grid policy, belongs
to Observatory, not the protocol library. Generated receiver decoded-navigation
messages are still protocol fields. Shared decoding mechanisms should not be
reimplemented by each analysis tool.

Historical import and live acquisition share normalization and scientific
state semantics. See [supported workflows](processing-overview.md),
[importer coverage](commonnex/importer.md) and [streaming API](commonnex/live.md).
Internal scheduling, data structures and optimization details belong in code,
not in this architecture guide.

## Selected CommonNEX interop design

CommonNEX is a logical model independent of Arrow. This project's native/Python
boundary uses Arrow-compatible columnar batches through nanoarrow and the Arrow
C Data/PyCapsule interface. Python/PyArrow owns Parquet I/O. Keep pybind11 for
high-level processing calls; do not make JSON or per-frame Python callbacks the
bulk scientific-data interface. Small settings and diagnostics may use JSON.

In-process buffer exchange is distinct from serialized Arrow IPC transport;
see [streaming infrastructure](commonnex/live.md) for transport contracts.

### Ownership and state

- Exported buffers remain immutable and alive until consumers release them.
  Retain input owners for the entire duration of native access.
- Release the GIL during native work; do not access Python objects without it.
- Validate schema, array offsets, lengths, nullability and supported types
  before native access. Unsupported layouts are errors, not reinterpretations.
- Bound in-flight data. Batch and file boundaries have no scientific meaning
  and must not implicitly reset processing state.
- Preserve canonical units, identities, missing values and quality semantics.
  Avoid redundant copies, but do not promise end-to-end zero-copy processing.

The component [API guide](../libneognss-obs/README.md) owns call lifecycle and
concurrency requirements. Tool documents own scientific reset and continuation
policies; the format specification does not prescribe their execution model.

### Native time

CommonNEX timestamps use `DECIMAL(38,12)` GPST seconds. Native exact-time
arithmetic uses signed 128-bit picosecond ticks backed by Boost.Int128; storage
and arithmetic types remain implementation choices, not format requirements.
Do not construct large absolute timestamps through binary64 seconds. Keep
timestamp and duration semantics distinct, with explicit checked conversion at
numerical-engine boundaries. Follow the [time policy](time-policy.md) and
[CommonNEX Core](commonnex/core.md) for rounding, units and missing-time rules.

## C++ toolchain portability

Support GCC/libstdc++ and Clang/libc++. Types registered with pybind11 must have
distinguishable qualified identities across translation units; same-named
anonymous-namespace helpers can collide under libc++ registration. Do not
assume that `size_t`, `long` and fixed-width integer types are interchangeable.

Verification must include importing the extension and exercising its batch
interface, not only compiling and linking. See the
[component build guide](../libneognss-obs/README.md#build-and-use).

## Dependencies and limits

Pinned pyubx2/pysbf2 definitions support protocol code generation. Generated
files belong in the build directory; schema availability does not establish
support for every firmware revision or scientific interpretation. The
[protocol API guide](../libcppgnss/README.md) owns parser coverage and errors.

nanoarrow, Boost.Int128, pybind11 and nlohmann/json support native processing
and interop. Preserve their upstream licenses and notices. The protocol library
has no Python or Arrow dependency; native analysis has no plotting or Parquet
dependency.

The current PPP/STEC numerical backend links RTKLIB-EX. Its calls are
process-wide serialized; independent parallel solutions require separate
processes. The [Ginan shim](ginan-shim.md) is a planned backend, not an assertion
about the currently supported solver. See [PPP](ppp.md), [STEC](stec.md) and
[SBAS](subframes.md) for scientific capabilities and limits.

References: [Arrow PyCapsule interface](https://arrow.apache.org/docs/format/CDataInterface/PyCapsuleInterface.html)
and [nanoarrow](https://arrow.apache.org/nanoarrow/latest/index.html).
