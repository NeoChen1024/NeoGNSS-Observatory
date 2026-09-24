# ParquetNEX persistence mapping

This is the current experimental persistence contract for the four supported
catalogs. Deferred capabilities are tracked in [TODO](TODO.md).

[Overview](overview.md)

ParquetNEX is optional persistence for CommonNEX, not a required processing
stage. Readers reconstruct the same logical records available on the direct
adapter path. See the [pipeline and modes](overview.md#processing-pipeline).

## Observatory implementation mapping

RawBits and receiver telemetry permit null GPST. Untimed rows remain in arrival
order in the last known GPST day, or `1980/01/06/` before any date is known;
that placement does not assert an actual timestamp. No retrospective move or
time backfill is required. See [receiver-time association](receiver-time.md).
Native batches carry an internal `_archive_day` routing column so a later
anchor in the same batch cannot misdate earlier untimed rows. Python removes
this implementation column before Parquet storage. Append via new parts, not
by modifying a closed Parquet file.

Python/PyArrow owns Parquet I/O; see [native architecture](../native-architecture.md)
for Arrow buffer ownership and interchange. Persistence is not required for
direct processing, and Parquet bytes are not Arrow in-memory buffers.

The unified `receiver-telemetry` catalog follows the same daily revision/part
names and Zstandard level 3. Its ordered measurement/pulse lists and status
fields are defined in [receiver telemetry](receiver-telemetry.md). Pending navigation windows
survive file boundaries and ordinary daily-import checkpoints.

## Storage initialization

A directory initialization operation imports `setup.json` and the optional
same-directory file named by `vendor_config`. See [Setup JSON](setup-json.md).
The vendor configuration file's format is opaque to this specification. Validate that
the reference names a file in that directory, not an arbitrary external path.
Keep Setup metadata at Setup scope, outside daily revisions.

Daily input consists of observation recordings such as UBX or SBF. It
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
  antenna.atx                # Optional selected antenna calibration
  2025/08/15/
    r00-observations-part00.parquet
    r00-observations-part01.parquet  # Completed deferred tail
    r00-raw-bits-part00.parquet
    r00-events-part00.parquet       # Completion, continuity and clock context
    r01-observations-part00.parquet # Complete replacement of this day's observations
```

Only applicable catalogs are written. A catalog is the storage name for a
record family, such as `observations`, `raw-bits`, `events`, or `receiver-telemetry`.
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

### Shared encodings and observation/raw-navigation rules

ParquetNEX defines an interoperable mapping of CommonNEX logical records to
Parquet schemas, file metadata, and dataset layout. The current mappings below
are shared by writers and readers, not selected independently per file.

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
- Preserve nullability, enums, units and the field definitions in the owning
  catalog document. Current metadata keys are listed below; no generic
  per-field constraint-expression metadata is required. Writers validate
  the logical rules rather than assuming Parquet physical types enforce them.
  Serialize logical null as Parquet null, not NaN or infinity. Convert any
  implementation-specific NaN missing-value representation at this boundary.
  Preserve declared integer widths and canonical rational scales. Retain a
  floating-point physical type only for an explicitly justified logical float
  field; do not expand scaled integers to floats on disk. Round trips must
  preserve integer counts, nullability, units, and scale exactly.
  Store the current schema label, catalog and GPST epoch/unit metadata.
  Scientific interpretation does not require
  provenance sidecars, execution snapshots, or artifact hash inventories.
- Partition Observation by its `gpst` and RawBits by its `nav_epoch_gpst`, within
  the station and GPST day. Neither requires an epoch-table join. Each day may contain several
  complete part files per record family. Payload timestamps determine coverage;
  filenames alone are not evidence of observation time or continuity.
- Close each part before publishing it by rename. Published files are immutable;
  deferred tail completion adds a new part to the same revision. Do not append
  bytes to a closed Parquet file. See the daily revision and publication rules below.
- Use the [wide observation layout](observations.md): one row per epoch/satellite/
  signal occurrence, C/L/D/S in separate nullable columns with independent
  quality fields. Do not serialize scalar observable rows as a second v0 layout.
- Present Setup metadata references must resolve in the declared dataset scope.
  There are no required
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

## Current catalog encoding and metadata

Record fields follow [Observation](observations.md), [RawBits](raw-bits.md),
[Events](events.md) and [receiver telemetry](receiver-telemetry.md). Enums use
strings, bodies use binary, and nested records/lists use Parquet structs/lists.
The internal `_archive_day` column is not persisted.

| File metadata key | Current value |
| --- | --- |
| `commonnex.schema` | `observation-pilot-1` (experimental label shared by all four catalogs) |
| `commonnex.catalog` | Catalog filename component |
| `time.scale` | `GPST` |
| `time.origin` | `1980-01-06T00:00:00` |
| `time.unit` | `s` |
| `setup_id` | Parent station identity |

The existing experimental schema label is not a compatibility promise or an
old-schema dispatch mechanism. Readers use the current field definitions.
Do not reinterpret older products solely because they carry the same label.

## Daily revisions

The fixed relative filename pattern within a station is:

```text
<GPST YYYY>/<MM>/<DD>/r<revision:02d>-<catalog>-part<part:02d>.parquet
```

Revision and part are two-digit decimal numbers, starting at `00`. Each is
scoped to its day/catalog (and parent station); part numbering
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
