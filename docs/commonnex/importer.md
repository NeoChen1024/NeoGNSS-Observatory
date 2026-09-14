# CommonNEX batch importer pilot

`ngo-cnex-import` is an implemented observation-first pilot, not full CommonNEX
acquisition support. Its `observation-pilot-1` Arrow/Parquet schema is experimental.
The broader format documents remain the target design. Do not use this pilot
as a lossless replacement for raw archives.

## Implemented path

- `libcppgnss` decodes RAWX version 1 and MeasEpoch revisions 0/1 without RTKLIB
  filtering, retaining supported GPS, Galileo, BeiDou, QZSS and SBAS signals.
  Unknown mappings are counted rather than assigned a guessed signal; GLONASS
  and NavIC are excluded. See `measurements.cpp` for the explicit current table.
- The native Observatory reader handles epoch completion and builds nanoarrow
  arrays, exporting ownership through Arrow C Data Interface capsules. Python
  receives batches, not per-frame callbacks or JSON observation dictionaries.
  Decode and array construction release the GIL.
- Native Boost.Int128 arithmetic produces `DECIMAL(38,12)` timestamps. RAWX's
  binary64 TOW is rounded directly from its exact binary rational to picoseconds,
  ties to even, before adding the integer week. SBF millisecond TOW is exact.
- Complete RAWX is immediately usable without NAV-EOE. SBF Measurements is
  buffered until matching EndOfMeas; a different epoch before closure is counted
  as incomplete and the previous pending group is omitted. File/chunk boundaries
  do not reset framing or the pending group.
- Python writes GPST-day `observations`, `raw-bits` and `events` catalogs with
  Zstandard level 3 compression. Events contain reported OBSERVATION/EPOCH and
  NAVIGATION/EPOCH completion, with RECORD_STRUCTURE or PROTOCOL_BOUNDARY basis. Missing other
  events never implies continuity or a known clock-correction state.
- `list` selects the latest revision independently for each day/catalog and
  returns all its parts. Eight Parquet writers may remain open at once; returning
  to an evicted partition creates another bounded part without rewriting it.

Each quality struct includes nullable `stddev_is_lower_bound`; MeasExtra supplies
variance-derived bounds, while RAWX bound semantics remain unknown.
Quality structs use string enums, nullable standard deviations and RINEX-only
fields left null. RAWX supplies code/phase validity, uncertainties, independent
half-cycle flags and millisecond lock duration. MeasEpoch supplies reconstructed
code/phase/Doppler, C/N0, half-cycle ambiguity and lock duration; source no-data
sentinels become null, while absent validity declarations remain UNKNOWN.
Clipped RAWX/SBF lock durations use lower bounds. No slip inference is performed.

MeasExtra revisions 0-3 are joined within the measurement epoch before
EndOfMeas: code/phase/Doppler uncertainties, high-resolution C/N0, longer lock
duration, modulo-256 continuity counter, Doppler variance factor and signed
code/phase preprocessing corrections are retained. Code/phase are not adjusted.
The join tolerates companion-block order and physical file boundaries, not
interleaved unrelated epochs. Missing MeasExtra keeps base observations usable;
ambiguous/unmatched extras are counted and not applied. Extra blocks arriving
first are included in the raw-tail replay cursor. Summary counters distinguish
excluded, unsupported, unmatched, ambiguous, pending and matched extras.

RawBits import covers the [implemented UBX/SBF adapters](raw-bits-importer.md),
including documentary mappings without current samples. Independent and
receiver checks remain separate. Failed-check bodies are retained; downstream
acceptance is not stored as a canonical property. RawBits-only input is supported.
UBX uses fresh NAV-TIMEGPS plus matching NAV-EOE, independently of RAWX time;
unresolved/conflicting groups are counted and omitted. SBF raw-navigation blocks
use their own TOW/WNc; no whole-epoch completion is inferred from a single block.
No transmission-time or observation-time equivalence is asserted.

Not yet implemented: undefined future RawBits representations, DecodedNav, telemetry,
cadence/reset/clock-correction events,
Meas3 decoding, RINEX/RTCM3 input, and automatic overlap reconciliation.
Observation values are not corrected using NAV-CLOCK or
SBF navigation solutions. The CLI reports its restricted catalog coverage.

## Initialize

Create one logical station directory with one receiver and one antenna. Start from the
[example Setup](../../config/commonnex-setup.example.json) and fill actual metadata;
the example is not a calibrated station description.

```sh
ngo-cnex-import init data/my-setup \
  --setup config/my-setup.json \
  --vendor-config /path/to/receiver-config.txt \
  --antenna-catalog /path/to/igs20.atx.gz \
  --antenna-catalog /path/to/ngs20.atx
```

The initializer imports `setup.json` directly at the station root, its optional
vendor config and selected `antenna.atx`. Both catalog and config options are
optional. To supply a config from another location, add
`--vendor-config /path/to/receiver-config.txt`. The file is copied unchanged
beside the output `setup.json`; its basename replaces `vendor_config` in the
output metadata, without modifying the input Setup or config. Without this
option, the initializer uses the filename already declared in the input Setup.
Config filenames must not collide with `setup.json` or reserved `antenna.atx`.
For a declared Septentrio receiver of any model, `init` also derives unknown tracking
entries from the vendor config's `setSignalTracking` command. Explicit tracking
is retained, with warnings on differences. Unsupported config syntax stays
opaque rather than yielding guessed or partially complete tracking. See the
[signal examples and mapping](setup-json.md#tracking-declaration-and-signal-names).
`setup_id` is a free-form string independent of OUTPUT and `marker.name`;
only companion filenames are constrained as path components.
Marker name/number/type are preserved explicitly, never
inferred from the Setup ID.
Initialization validates strings, nominal period, tracking, marker XYZ, ARP
N/E/U, orientation and feed-line length. See [ANTEX selection](setup-json.md#antex-selection-during-initialization)
for exact type/radome matching, optional matching-serial priority, catalog order
and validity handling. The example intentionally has unknown radome: confirm
the correct catalog code before requesting calibration selection.
Existing output directories are never overwritten. Files are prepared in a
temporary directory and published by rename; failed initialization removes
only its own unpublished temporary directory. No receiver is configured.

## Import local recordings

```sh
ngo-cnex-import run -p sbf --station data/my-setup first.25_ second.25_
ngo-cnex-import list data/my-setup
```

Use `-r/--recursive` to supply directories (files and directories may be mixed):

```sh
ngo-cnex-import run -p sbf -r --station data/my-setup /data/sbf-archive
ngo-cnex-import run -p ubx -r --station data/another-setup /data/ubx-archive
```

Recursive discovery selects `*.ubx` for UBX. SBF basenames must match
`[A-Za-z0-9_]{4}[0-9]{3}[A-Za-z0-9]\.[0-9]{2}_`: four marker characters,
three DOY digits, one session character, a literal dot, two year digits and
a final underscore. Examples: `bx4a1600.25_`, `bee_2560.26_`. The session
character is retained; daily filenames commonly use `0`. RINEX `.25o`/`.25p`,
XZ copies and unrelated files are not selected. Matching is case-sensitive for
the `.ubx` suffix. Filenames select candidates, not observation dates or order.
Explicit file arguments need not match these discovery patterns, so a renamed
`sample.sbf` remains usable; explicit XZ inputs are rejected.

Directory arguments require `--recursive`. Traversal is deterministic and does
not follow nested directory symlinks; an explicitly supplied directory symlink
can identify the root. Unreadable directories and empty overall discovery are
errors. Overlapping roots or file aliases resolving to the same input path are
errors, not an observation deduplication policy.

Inputs must be expanded files. The importer orders files from bounded head
samples, not filenames. Use one continuous recording path per invocation; do
not mix overlapping logger copies.
The importer does not acquire FTP data, decompress XZ, or require a QA stamp.
Choose `-p ubx` for RAWX. Frame-validated foreign-protocol messages are skipped
atomically with a throttled warning and counters.
`run --source-antenna` selects a native observation input (default 0; RAWX
requires 0). Different physical antennas use different logical stations. The
choice is saved in continuation state and must match when resuming. Observations,
RawBits and Events carry `setup_id`, not a separate Stream/antenna reference.

Ordinary import refuses an already existing day/catalog rather than guessing
whether data is a duplicate, a missing interval, or a replacement. Summary JSON
is on stdout; processing progress and warnings are on stderr.

### Head-only ordering and time reversal

For each new input, inspect at most `min(size, max(ceil(size / 100), 1 MiB))`
bytes from its beginning. No tail read or full indexing pass is performed.
An independent native framing probe validates complete frames and records the
first usable observation time (RAWX or MeasEpoch). The probe stops early when
that anchor is available. If none is present in the window, use the first valid
UBX NAV-TIMEGPS or supported timed SBF RawBits anchor instead. No observation
completion event is required merely to read a valid measurement timestamp.
Do not snap fractional GPST or use a binary64 sort key; probe times use exact
integer seconds/picoseconds, with the same native conversion as import.

Stable-sort by that timestamp, preserving caller order for ties. Print the
resulting order and anchor kind on stderr. If a file has no usable anchor in
the window, fail before staging any output; never guess from its filename or
silently expand the probe into a full scan. A tiny fragment-only file may need
to be reassembled before importing.

Formal import starts each sorted file at byte zero and carries framing/epoch
state across files. Probe state is discarded. Native checks compare observation
epochs separately from UBX TIMEGPS. SBF RawBits times are checked separately
per satellite/signal/message family. SBF SIS timestamps describe transmission
time and need not follow output order. RawBits decreases produce throttled
warnings and a summary count, preserving timestamps and canonical payloads
unchanged. They are not evidence that restitch is required. RawBits tail
continuation likewise does not require times to exceed previously published
maxima; readers must not assume chronological row order.
Never compare these independent time sequences against each other. A strict
decrease in observation time or UBX TIMEGPS still stops import and reports the
axis, previous/current GPST and source file/byte offset. Equal timestamps are accepted without a
duplicate check. Head samples do not claim overlap detection or global validity.
Inspect or re-stitch overlapping/disordered inputs rather than expecting the
importer to trim or merge them. Failed runs do not publish their staged catalogs;
unpublished staging remains available for inspection.

Last-seen times for these sequences are retained in the continuation cursor. Previously
parsed navigation replay is skipped using the existing cursor, so it is not
mistaken for new backwards time. Saved raw-tail segments always precede the
sorted new input files and are never independently probed or reordered.

## Tail continuation and reconstruction

The latest affected GPST day receives `import-state.json`, a narrow continuation
cursor for that invocation. It records the published part names, terminal input
position, and raw file/offset ranges needed to replay the unpublished tail.
It also stores bounded pending UBX navigation context and a replay skip
offset, so observation-tail replay does not duplicate published RawBits.
It is not a science catalog or general recovery manifest.
Raw context files must remain available and unchanged; the basic size check does
not detect every possible same-size alteration. No pending tail means no raw
context replay is needed. Downstream processing ignores this sidecar.

```sh
ngo-cnex-import run -p sbf --station data/my-setup \
  --resume-from data/my-setup/2025-08-15/import-state.json next.25_
```

The supplied inputs are new recording files; do not repeat the saved raw-context
files. Tail completion produces the next `partNN` under the existing revision;
new days start at `r00`/`part00`. Context replay starts at the withheld frame/group,
not at the already published prefix. Insertion before existing catalog coverage
is rejected, not silently treated as an append. No general deduplication is offered.
An invocation with no complete supported epochs fails without publishing parts;
provide that raw input again together with its continuation.

For a real correction or missing interval inside published data, explicitly
provide the complete replacement input:

```sh
ngo-cnex-import run -p sbf --station data/my-setup --rebuild complete-day.25_
```

This creates the next revision for affected implemented catalogs. It does not
merge old rows automatically, nor rebuild unsupported catalogs. A rebuilt empty
catalog uses an empty part when necessary to supersede an older nonempty catalog.
Keep all related future catalogs consistent when expanding the importer.

Files are staged, closed and renamed without overwriting existing parts. Update
state after publishing parts. Do not read while writing. Failed/interrupted
publication requires manual resolution before readers run; no atomic multi-file
transaction or full crash recovery is promised. Old revisions are retained.

## Remaining work

- [x] Initializer, filename parsing/allocation and latest-revision selection.
- [x] Head-only ordering, strict observation/TIMEGPS checks and nonfatal RawBits reversal warnings.
- [x] RAWX/MeasEpoch decoding and native decimal Arrow observation batches.
- [x] Measurement completion events and daily Parquet writing.
- [x] Local raw-tail continuation with immutable parts and explicit rebuilding.
- [x] MeasExtra uncertainty, C/N0, lock/continuity and preprocessing corrections.
- [ ] Cadence, clock and navigation epoch Events, with context across invocations.
- [x] Reviewed RawBits layouts and independent navigation-time association;
  distinguish sample-verified and documentary adapters in the coverage table.
- [ ] Additional input protocols and any explicitly requested reconciliation.
