# ParquetNEX persistence mapping

Status: v0 design draft; not an implemented format or API.

[Overview](overview.md)

ParquetNEX is optional persistence for CommonNEX, not a required processing
stage. Readers reconstruct the same logical records available on the direct
adapter path. See the [pipeline and modes](overview.md#processing-pipeline).

## Observatory implementation mapping

Use PyArrow RecordBatches backed by the native Arrow-compatible buffers for
writing, and pass batches read from Parquet back through the same native import
interface. Do not route bulk values through JSON, lists of dictionaries or
per-row Python objects. Nullable columns use Arrow validity bitmaps and become
Parquet nulls. This does not require Arrow IPC serialization between C++ and
Python, or a Parquet round trip for direct processing.

Arrow describes in-memory columnar data; Parquet describes encoded on-disk
storage. They are complementary, not interchangeable byte layouts. Parquet
encoding/decoding and compression still perform work even when the native/Python
boundary can share buffers. The [interop design](../native-architecture.md#selected-commonnex-interop-design)
defines the selected nanoarrow/PyCapsule integration; it is not yet implemented.

Use `pyarrow.parquet.ParquetWriter` for bounded writes, `ParquetFile.iter_batches()`
for replay, and `pyarrow.dataset` for multi-file selection. PyArrow invokes native
Parquet encoding/compression; Python orchestration does not imply per-row Python
encoding. Keep the native libraries independent of Parquet. nanoarrow supplies
the interchange helpers, not a Parquet encoder. Do not add a DataFrame conversion
between native batches and the writer. Batch size and row-group size are separate
tuning choices; neither requires buffering an entire GPST day.

## Storage initialization

A directory initialization operation imports `setup.json` and the optional
same-directory file named by `vendor_config`. See [Setup metadata](core.md#setup-metadata).
The configuration file's format is opaque to this specification. Validate that
the reference names a file in that directory, not an arbitrary external path.
Keep Setup metadata at Setup scope, outside daily revisions.

Daily input consists of observation recordings such as UBX, SBF or RINEX. It
does not carry or recopy Setup metadata/configuration. Daily Parquet records
reference the initialized Setup and Stream; readers resolve them from the
storage root. A detached daily directory alone is not a self-contained Setup.
Inspect declared constellation/signal compatibility before scanning observations;
unknown declarations do not justify rejecting an otherwise usable input.

Selected layout (family part names remain to be finalized):

```text
<root>/<setup_id>/
  setup.json
  receiver-config.txt       # Example vendor_config filename; format unrestricted
  <stream_id>/
    2025-08-15/
      r0001/
        observations.parquet
        observation-epochs.parquet
        navigation-epochs.parquet
        raw-nav.parquet
      r0002/
        ...                # Complete replacement daily record set, not a delta
```

Only applicable families are written. Large families may use multiple parts.
Stream declarations must be resolvable at initialization; their exact JSON
mapping remains to be finalized. Date directories denote GPST days.

## Serialization rules

ParquetNEX defines an interoperable mapping of CommonNEX logical records to
Parquet schemas, file metadata, and dataset layout. Unlike CommonNEX transport,
these mappings must eventually be specified rather than left writer-specific.

- Represent `gpst_ns` with Parquet's unsigned 64-bit integer logical annotation
  over its 64-bit integer physical storage. Do not use the standard Parquet
  `TIMESTAMP` annotation for GPST. Readers must preserve unsigned semantics.
- Preserve nullability, semantic identifiers/constraints, enums, units, and
  source-extension definitions. The schema version resolves standard semantic
  definitions; store additional field constraints where needed. Writers validate
  the logical rules rather than assuming Parquet physical types enforce them.
  Serialize logical null as Parquet null, not NaN or infinity. Convert any
  implementation-specific NaN missing-value representation at this boundary.
  Preserve declared integer widths and canonical rational scales. Retain a
  floating-point physical type only for an explicitly justified logical float
  field; do not expand scaled integers to floats on disk. Round trips must
  preserve integer counts, nullability, units, and scale exactly.
  Store schema versions, record family, GPST epoch/unit, and necessary schema
  interpretation in file metadata. Scientific interpretation does not require
  provenance sidecars, execution snapshots, or artifact hash inventories.
- Partition timed records by stream and GPST day. RawNav uses its referenced
  NavigationEpoch's GPST day; unresolved nav_epoch_id records have a separate
  unassociated scope. Each day may contain several
  complete part files per record family. Payload timestamps determine coverage;
  filenames alone are not evidence of observation time or continuity.
- Write bounded parts, close them, and publish a complete daily revision by
  directory rename. Published revisions are immutable. Do not append in place
  to a published Parquet file or change an already published revision.
- Use the [Core wide observation layout](core.md): one row per epoch/satellite/
  signal occurrence, C/L/D/S in separate nullable columns with independent
  quality fields. Do not serialize scalar observable rows as a second v0 layout.
- Setup/Stream/epoch references must resolve in the declared dataset scope.
  Setup metadata is imported at initialization, not replicated for daily input
  or output. Epoch tables and related families are selected from the same daily
  revision. Repeated identities must retain consistent interpretation.
- Compression, row-group sizing, sorting, and optional compaction are physical
  choices to measure. Do not promise a compression ratio relative to RINEX;
  compare equal retained content, including compressed RINEX baselines.

Concrete family schemas, metadata keys, part naming, and cross-day event/reference
mapping remain to be finalized. Use one v0 mapping rather than alternate layouts.

## Daily revisions

Normal ingestion adds new data. Retroactive repair is rare and explicitly
rebuilds affected stream/GPST-day scopes into their next numeric revision
(`r0001`, `r0002`, ...). Each revision is a complete daily snapshot of all its
applicable record families, not a delta to union with older revisions. Preserve
older revisions for manual recovery; no automatic cleanup or transaction log
is required. Revision is a storage concept, not a Setup/configuration revision.

Reconcile existing unique records with additions in bounded batches. Repeated
input is a scientific no-op; complementary input extends coverage and genuine
conflict alternatives remain explicit. Retaining old revisions does not remove
the need to avoid duplicate observations within the selected revision.

Write to a temporary sibling directory, close all files, then rename on the same
filesystem to the final revision name. Readers ignore unfinished directories and
select the highest published numeric revision per day, fixing that selection
when a run starts. Never recursively read all revision directories as one table,
or mix different revisions of a day's related families. Permit only one writer
per affected scope; no concurrent-writer coordination framework is required.

A multi-day repair adds a new revision to every affected day. Revision numbers
need not match across days, and unaffected days stay untouched. Inspect adjacent
days when epoch association/completion changes at midnight. There is no atomic
multi-day publication guarantee: initially, complete repair publication before
starting readers that require a consistent view across affected days. Keep
cross-day identities/references resolvable in that selected view. No global
dataset version is required.

Keep importer state across file/day boundaries. Restarted daily imports must
restore the necessary context or reread sufficient preceding input according to
protocol dependencies, not an assumed one-second overlap. Unresolved boundary
records remain explicit. This does not require a generic checkpoint system.

## References

- [Parquet logical types](https://parquet.apache.org/docs/file-format/types/logicaltypes/).
- [Parquet compression](https://parquet.apache.org/docs/file-format/data-pages/compression/).
