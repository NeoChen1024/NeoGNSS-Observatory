# ParquetNEX persistence mapping

Status: v0 design draft. An [observation-first pilot](importer.md) implements a
subset; the complete format and all catalog schemas are not implemented.

[Overview](overview.md)

ParquetNEX is optional persistence for CommonNEX, not a required processing
stage. Readers reconstruct the same logical records available on the direct
adapter path. See the [pipeline and modes](overview.md#processing-pipeline).

## Observatory implementation mapping

RawBits and receiver telemetry permit null GPST. Untimed rows remain in arrival
order in the last known GPST day, or `1980/01/06/` before any date is known;
that placement does not assert an actual timestamp. No retrospective move or
time backfill is required. See [receiver-time association](telemetry-time.md).
Native batches carry an internal `_archive_day` routing column so a later
anchor in the same batch cannot misdate earlier untimed rows. Python removes
this implementation column before Parquet storage. Append via new parts, not
by modifying a closed Parquet file.

Use PyArrow RecordBatches backed by the native Arrow-compatible buffers for
writing, and pass replay batches to the relevant CommonNEX consumer interface,
not back to the raw-byte importer. Do not route bulk values through JSON, lists of dictionaries or
per-row Python objects. Nullable columns use Arrow validity bitmaps and become
Parquet nulls. This does not require Arrow IPC serialization between C++ and
Python, or a Parquet round trip for direct processing.

Arrow describes in-memory columnar data; Parquet describes encoded on-disk
storage. They are complementary, not interchangeable byte layouts. Parquet
encoding/decoding and compression still perform work even when the native/Python
boundary can share buffers. The [interop design](../native-architecture.md#selected-commonnex-interop-design)
defines the selected nanoarrow/PyCapsule integration; native output is implemented
in the observation importer. STEC also consumes replayed Observation Arrow
batches natively; other analysis consumers are migrated separately.

Use `pyarrow.parquet.ParquetWriter` for bounded writes, `ParquetFile.iter_batches()`
for replay, and `pyarrow.dataset` for multi-file selection. PyArrow invokes native
Parquet encoding/compression; Python orchestration does not imply per-row Python
encoding. Keep the native libraries independent of Parquet. nanoarrow supplies
the interchange helpers, not a Parquet encoder. Do not add a DataFrame conversion
between native batches and the writer. Batch size and row-group size are separate
tuning choices; neither requires buffering an entire GPST day.

The unified `receiver-telemetry` catalog follows the same daily revision/part
names and Zstandard level 3. Its ordered measurement/pulse lists and status
fields are defined in [Auxiliary](auxiliary.md). Pending navigation windows
survive file boundaries and ordinary daily-import checkpoints.

## Storage initialization

A directory initialization operation imports `setup.json` and the optional
same-directory file named by `vendor_config`. See [Setup JSON](setup-json.md).
The vendor configuration file's format is opaque to this specification. Validate that
the reference names a file in that directory, not an arbitrary external path.
Keep Setup metadata at Setup scope, outside daily revisions.

Daily input consists of observation recordings such as UBX, SBF or RINEX. It
does not carry or recopy Setup metadata/configuration. Daily Parquet records
reference the initialized Setup; readers resolve it from the
storage root. A detached daily directory alone is not a self-contained Setup.
Inspect declared constellation/signal compatibility before scanning observations;
unknown declarations do not justify rejecting an otherwise usable input.

Selected layout:

The Setup directory name is chosen by the caller; `<setup-directory>` is not
constructed from free-form `setup_id`. Its metadata carries the logical ID.

```text
<root>/<setup-directory>/
  setup.json
  receiver-config.txt       # Example vendor_config filename; format unrestricted
  antenna.atx                # Optional selected receiver calibration
  2025/08/15/
    r00-observations-part00.parquet
    r00-observations-part01.parquet  # Completed deferred tail
    r00-raw-bits-part00.parquet
    r00-events-part00.parquet       # Completion, continuity and clock context
    r01-observations-part00.parquet # Complete replacement of this day's observations
```

Only applicable catalogs are written. A catalog is the storage name for a
record family, such as `observations`, `raw-bits`, `events`, or `decoded-nav`.
The [Events family](events.md) uses daily part files when
present, not a global file. Interval events are assigned to the next available
epoch's GPST day and carry previous timestamps directly. Event absence is
interpreted with import capability/coverage, not as unconditional continuity.
RawBits-only revisions omit observation files entirely; they retain nullable
navigation times directly on RawBits and applicable Events. Do not create empty
observation tables as a conformance prerequisite. Observation requires valid
measurement time; RawBits/telemetry with null GPST remains readable in arrival order.
One directory represents one logical station with one receiver/antenna. There
is no additional Stream subdirectory or Stream metadata file. Records and Parquet metadata
carry the parent `setup_id`; native source-antenna selection is an import option
retained in the continuation cursor, not a second logical identity.
Date directories denote GPST days. Companion files are initialized once, not
replicated per day. See [ANTEX selection](setup-json.md#antex-selection-during-initialization).

The importer publishes completed days during a multi-day run, not only at its
end. Daily `import-state.json` files track continuation; the latest unconsumed
state is selected by default. A publication barrier can also close the next
day's first partial part to align the cursor with all emitted rows. See
[publication and continuation](importer.md#tail-continuation-and-reconstruction).

## Serialization rules

### Decoded navigation collections

[DecodedNav](decoded-nav.md) is stored separately in the `decoded-nav` catalog,
using `r00-decoded-nav-part00.parquet` and the naming rules below.
This is a file within an applicable partition/revision, not a perpetually
appended global file. It uses the common header and typed EPH/STO/EOP/ION
payload branches; no JSON parameter blobs are introduced.

Receiver-derived DecodedNav may live in its station's daily revision when the
model's partition day matches that directory. Standalone navigation collections
(including merged RINEX NAV) live outside receiver Setup hierarchies and
do not require synthetic receiver metadata. Both use the same record schema.
The standalone collection directory naming and collection metadata encoding
remain to be finalized. Collection identity is file/collection metadata;
acquisition context carries explicit times rather than mandatory RawBits row references.

GPS LNAV and Galileo I/NAV/F/NAV EPH use the day of native `toc` nominally
aligned to GPST. Native reference times and their additional GPST coordinates
follow DecodedNav's `NavigationTime` definition. Applying or updating a fine
broadcast time-offset model does not change the partition day, even if the
corrected `gpst` crosses midnight. Acquisition on a different day does
not change that partition: a receiver-derived record belongs in the model's
reference-day output, potentially requiring a new revision of that day rather
than insertion into the acquisition-day directory. Other navigation models
must define their partition reference field explicitly. No duplicate
`partition_gpst` value is stored. These dates are storage partitions, not
validity windows; consumers select relevant neighboring partitions as needed.
Close and publish files by rename as for the other families.

The [additional model definitions](navigation-models.md) select TOC for the
other Keplerian EPH models, shared reference time for SBAS EPH, and model
reference time for STO/EOP. All partition dates use nominal GPST alignment.
ION uses known transmission time, or reliable acquisition epoch when transmission
time is absent; these remain distinct semantics on replay. ION lacking both is
skipped with diagnostics/counts. Do not create an untimed collection-level
DecodedNav file or use an arbitrary date for it.

DecodedNav is logically a tagged union, physically represented by mutually
exclusive nullable typed structs. Validate the active branch against the
record-kind/system/message-type/subtype discriminator. Shared field shapes do
not imply interchangeable models or algorithms.

### Shared encodings and observation/raw-navigation rules

ParquetNEX defines an interoperable mapping of CommonNEX logical records to
Parquet schemas, file metadata, and dataset layout. Unlike CommonNEX transport,
these mappings must eventually be specified rather than left writer-specific.

- Represent `GpstTimestamp`, `TimeDelta` and `Duration` as `DECIMAL(38,12)` over a 16-byte
  `FIXED_LEN_BYTE_ARRAY`, with signed big-endian two's-complement unscaled
  integers as required by Parquet. Values are seconds; only timestamps use the GPST origin.
  use Arrow `decimal128(38,12)` in the selected interop implementation.
  Do not use Parquet `TIMESTAMP`, convert absolute times through float64, or
  infer UTC semantics from the storage type. Preserve precision, scale, GPST
  origin, semantic type and units on replay. Enforce nonnegative timestamps
  and durations, and strictly positive periods separately;
  signed timestamp differences may be negative. Parquet and Arrow buffer
  layouts are distinct; the Parquet writer handles their conversion.
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
- Partition Observation by its `gpst` and RawBits by its `nav_epoch_gpst`, within
  the station and GPST day. Neither requires an epoch-table join. Each day may contain several
  complete part files per record family. Payload timestamps determine coverage;
  filenames alone are not evidence of observation time or continuity.
- Close each part before publishing it by rename. Published files are immutable;
  deferred tail completion adds a new part to the same revision. Do not append
  bytes to a closed Parquet file. See the daily revision and publication rules below.
- Use the [Core wide observation layout](core.md): one row per epoch/satellite/
  signal occurrence, C/L/D/S in separate nullable columns with independent
  quality fields. Do not serialize scalar observable rows as a second v0 layout.
- Present Setup metadata references must resolve in the declared dataset scope.
  Standalone DecodedNav does not require those references. There are no required
  epoch tables, row IDs, or per-row event foreign keys. Optional row counters
  are file-local and may be reassigned in a new revision.
  Setup metadata is imported at initialization, not replicated for daily input
  or output. Select revisions independently per day/catalog after the importer
  has finished publishing all related outputs; numeric revisions need not match.
  Continuity-sensitive readers load applicable Events, including still-effective
  states from earlier days; reading only yesterday is not a guaranteed bound.
  Epoch-specific evidence is not carried forward. Missing state is UNKNOWN.
- The recommended compression method for CommonNEX persisted as ParquetNEX is
  **Zstandard (ZSTD), level 3**. Writers should select the level explicitly
  rather than relying on a library's codec default (with PyArrow:
  `compression="zstd", compression_level=3`). This is a recommended storage
  default, not a mandatory conformance requirement or an Arrow in-memory
  compression rule. Other Parquet-supported codecs or uncompressed storage
  do not change CommonNEX record semantics.
- Compression tuning, row-group sizing, sorting, and optional compaction remain
  physical choices to measure. Do not promise a compression ratio relative to RINEX;
  compare equal retained content, including compressed RINEX baselines.

Concrete family schemas, metadata keys, and event context selection
mapping remain to be finalized. Use one v0 mapping rather than alternate layouts.

## Daily revisions

The fixed relative filename pattern within a station or navigation collection is:

```text
<GPST YYYY>/<MM>/<DD>/r<revision:02d>-<catalog>-part<part:02d>.parquet
```

Revision and part are two-digit decimal numbers, starting at `00`. Each is
scoped to its day/catalog (and parent station or collection); part numbering
restarts at `00` for a new revision. No revision directory or repeated date in
the filename is used. Exhausting `99` requires an explicit naming-policy
extension, not wrapping or silently emitting a different-width filename.

- Initial import writes `r00-<catalog>-part00.parquet` for applicable catalogs.
- A deferred incomplete tail is not published. When subsequent local raw input
  completes it, write the next part under the same revision, assigned by the
  record's GPST partition rule. Existing parts remain unchanged. Create parts
  only for catalogs receiving additional records. Parts do not imply discontinuity.
- Changes to published rows, intermediate insertions, or explicit reconstruction
  produce the next revision. It completely replaces that day's catalog,
  including all previously retained parts; it is not a patch over an older version.
- Readers select the highest numeric revision separately per day/catalog and
  read all its parts in numeric order. Never union old and new revisions.
  Preserve old files for manual recovery; no automatic cleanup is required.

Keep native state across files/days within a batch. A later invocation may
reread sufficient preceding local raw input to reconstruct the withheld tail;
prior Parquet cannot restore information never published. Context replay must
not emit already published records again. This targeted boundary replay is not
general deduplication or idempotent reimport. If completion is still impossible,
report and omit the incomplete tail without inventing timestamps or joining
unrelated epochs. Acquisition/FTP and waiting for receiver files are outside
the importer; it processes the local files supplied by the caller.

The first implementation does not support reading while writing. Permit one
writer per affected scope. Stage files outside the reader-visible filename
pattern, close them, and publish by same-filesystem rename without overwriting
existing files. Start readers only after all related catalog/day writes finish
successfully. An interrupted publication must be resolved before reading;
highest revision alone is not a transaction-completion marker. No multi-file
transaction framework or concurrent-reader guarantee is introduced.

A multi-day repair revises only affected day/catalog scopes. Update affected
Events as well, keeping scientific interpretation consistent before readers
start; revision numbers need not match between catalogs or days. Renumbering
file-local counters alone never forces downstream revisions. A complete empty
replacement uses a schema-bearing empty part00 so readers do not fall back to
an older nonempty revision; normally absent catalogs require no empty files.

Basic tooling must parse/validate filenames, select latest revisions and their
parts, allocate the next revision/part, and reject accidental overwrite. It does
not provide automatic overlap merging or a generic version-management system.
Immutable existing parts let synchronization transfer only new tail parts or
new days; a rebuilt revision may require transferring a complete new catalog.

## Progress

- [x] Select PyArrow I/O and native Arrow-compatible batches without C++ Parquet.
- [x] Recommend explicit ZSTD level 3 for ParquetNEX storage, without requiring
  that codec or level for format conformance.
- [x] Select direct decimal timestamps, no epoch tables or required row references.
- [x] Fix GPST date directories and two-digit revision/part filenames.
- [x] Separate deferred tail parts from complete replacement catalog revisions;
  do not introduce ID-driven cascading rebuilds or reading while writing.
- [x] Select daily Events with context lookup beyond the requested day when needed.
- [ ] Finalize full family schemas, enum encoding, metadata keys
  and equal-time conflict applicability with Events.
- [x] Implement pilot initialization, bounded writing and revision/part selection.
- [x] Implement measurement-tail raw replay using a daily import-state sidecar.
- [x] Implement a native Arrow Observation replay consumer for GPS STEC.
- [ ] Implement remaining catalogs and migrate other native replay consumers.
- [ ] Validate remaining catalogs and source-specific RINEX offset replay.

## References

- [Parquet logical types](https://parquet.apache.org/docs/file-format/types/logicaltypes/).
- [Parquet compression](https://parquet.apache.org/docs/file-format/data-pages/compression/).
