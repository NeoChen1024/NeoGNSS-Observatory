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
- Native processing exchanges owned columnar batches with Python; see the
  [interop contract](../native-architecture.md#selected-commonnex-interop-design).
- Timestamp normalization produces `DECIMAL(38,12)` timestamps. RAWX's
  binary64 TOW is rounded directly from its exact binary rational to picoseconds,
  ties to even, before adding the integer week. SBF millisecond TOW is exact.
- Complete RAWX is immediately usable without NAV-EOE. SBF Measurements is
  buffered until matching EndOfMeas; a different epoch before closure is counted
  as incomplete and the previous pending group is omitted. File/chunk boundaries
  do not reset framing or the pending group.
- Python writes `observations`, `raw-bits`, `events` and `receiver-telemetry` catalogs with
  Zstandard level 3 compression. Events contain reported OBSERVATION/EPOCH and
  NAVIGATION/EPOCH completion, with RECORD_STRUCTURE or PROTOCOL_BOUNDARY basis. Missing other
  events never implies continuity or absence of internal receiver clock adjustments.
- `list` selects the latest revision independently for each day/catalog and
  returns all its parts. A day/catalog can contain multiple parts without
  rewriting earlier parts.

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
UBX uses NAV-TIMEGPS independently of EOE and RAWX time. SBF uses synchronous
receiver navigation TOW/WNc, not SIS headers. Both use a 10-period anchor timeout
with nullable GPST and uptime association as specified in the
[association policy](telemetry-time.md). No time-waiting backlog is retained.
No whole-epoch completion is inferred from a single raw block.
No transmission-time or observation-time equivalence is asserted.

Status import retains float32 Celsius, receiver uptime and fine-time state.
Clock and pulse estimates use the [auxiliary field mappings](auxiliary.md).
Receiver restart Events retain uptime-decrease or fresh-pair offset-jump
inference; nullable GPST never suppresses these records. The native batch order
is observations, events, raw-bits, receiver-telemetry. `_archive_day` is Python
routing only and is removed before persistence. Telemetry is assembled one PVT
epoch late; see [Auxiliary](auxiliary.md) for fields, ordered report lists and
explicit end-of-stream behavior. Pending telemetry is included in checkpoints.
Use `--finalize-telemetry` only at a true stream end, not each daily continuation.

SBF Type1/Type2 smoothing state remains in Observation receiver_corrections;
UBX uses null for unavailable smoothing state.

Not yet implemented: undefined future RawBits representations, DecodedNav,
cadence events,
Meas3 decoding, RINEX/RTCM3 input, and automatic overlap reconciliation.

STEC maps timed receiver-restart Events to independent phase arcs and receiver
bias segments; see [STEC restart handling](../stec.md#receiver-restart-events).
Untimed restart evidence and unsupported stream Events remain errors; do not
remove these events to bypass validation. SBAS grid skips null-time RawBits.
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
character is retained; daily filenames commonly use `0`.
The same names with an additional `.xz` suffix are also selected. When an
expanded file and its XZ copy coexist in a scanned directory, the expanded
file wins; supplying both explicitly is an error. RINEX `.25o`/`.25p` and unrelated
files are not selected. Matching is case-sensitive for the `.ubx` suffix.
Filenames select candidates, not observation dates or order.
Explicit file arguments need not match these discovery patterns, so a renamed
`sample.sbf` and `sample.sbf.xz` remain usable.

Directory arguments require `--recursive`. Traversal is deterministic and does
not follow nested directory symlinks; an explicitly supplied directory symlink
can identify the root. Unreadable directories and empty overall discovery are
errors. Overlapping roots or file aliases resolving to the same input path are
errors, not an observation deduplication policy.

Inputs may be expanded or XZ-compressed files, including a mixture. XZ is
decoded in memory without writing an expanded temporary file. The `xz`
executable is required to read expanded sizes from the container indexes;
Python's LZMA decoder reads and checks the payload, including concatenated
XZ streams. Progress, head-probe budgets and continuation offsets refer to
expanded bytes. Resume also checks the stored file size; seeking to a saved
XZ offset may require decompressing from the beginning, so continuation can
be slower than for an expanded file. Corrupt/truncated archives fail rather
than being treated as a clean end of input. As with other input failures,
already published daily parts are retained.

The importer orders files from bounded head
samples, not filenames. Use one continuous recording path per invocation; do
not mix overlapping logger copies.
The importer does not acquire FTP data or require a QA stamp.
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

Import preserves input order and scientific state regardless of worker
scheduling. `--decode-workers` controls decoding concurrency; see `--help` for
limits and defaults. It may change on resume without changing scientific state.
Memory use includes decoded records, not just the `--chunk-mib` input buffer.

The progress bar measures parsed input, not completed publication. Import waits
for pending writes before reporting success. Parsing or writing errors prevent
affected work from publishing; resume from the last published continuation
state, not from partially consumed work.

For each new input, inspect at most `min(size, max(ceil(size / 100), 1 MiB))`
bytes from its beginning. No tail read or full indexing pass is performed.
An independent native framing probe validates complete frames and records the
first usable observation time (RAWX or MeasEpoch). The probe stops early when
that anchor is available. If none is present in the window, use the first valid
UBX NAV-TIMEGPS or synchronous SBF navigation anchor instead. SIS timestamps
are never probe anchors. No observation
completion event is required merely to read a valid measurement timestamp.
Do not snap fractional GPST or use a binary64 sort key; probe times use exact
integer seconds/picoseconds, with the same native conversion as import.

Stable-sort by that timestamp, preserving caller order for ties. Print the
resulting order and anchor kind on stderr. If a file has no usable anchor in
the window, warn and preserve supplied input order for the entire import.
Do not guess from filenames or expand the probe into a full scan. The caller
must supply continuous, nonoverlapping input order when it cannot be inferred.

Formal import starts each sorted file at byte zero and carries framing/epoch
state across files. Probe state is discarded. Native checks compare observation
epochs separately from receiver navigation time. UBX uses TIMEGPS; SBF uses
PVTCartesian, PVTGeodetic, ReceiverTime and EndOfPVT. RawBits receives a nonexpired
navigation context or null, never its source SIS timestamp.
Invalid anchors disable the previous context; untimed RawBits is still emitted. The canonical body
is unchanged. Observation time does not substitute for navigation context.
Their shared high-water mark is used only to age navigation anchors; never
interpret cross-axis differences as observation reversals. A strict
decrease in observation or receiver navigation time stops import and reports the
axis, previous/current GPST and source file/byte offset. Equal timestamps are accepted without a
duplicate check. Head samples do not claim overlap detection or global validity.
Inspect or re-stitch overlapping/disordered inputs rather than expecting the
importer to trim or merge them. Failed runs retain already published daily parts;
unpublished staging remains available for inspection and is not selected by readers.

Last-seen times for these sequences are retained in the continuation cursor. Previously
parsed navigation replay is skipped using the existing cursor, so it is not
mistaken for new backwards time. Saved raw-tail segments always precede the
sorted new input files and are never independently probed or reordered.

## Tail continuation and reconstruction

The station uses `YYYY/MM/DD/` GPST directories. The latest receiver/observation
context day receives `import-state.json`, a narrow continuation cursor. It records
published parts, previously read input paths/sizes/positions, and raw ranges for
tail replay. No SIS timestamp affects the directory or cursor location.
The importer rejects the old flat-date directory layout and incompatible
continuation cursors. Initialize a new station for these experimental products;
it does not rename or relabel previously generated SIS-timed data.
It also stores the last anchor, receiver uptime, fresh paired offset, archive
day, timeout high-water mark, independent UBX completion context, required
period and replay skip offset. Replay does not duplicate RawBits or telemetry.
State version 3 additionally requires receiver-time policy 3, which preserves
the full pending navigation completion timestamp; older association
cursors and period changes are rejected. Rebuild old experimental imports.
It is not a science catalog or general recovery manifest.
Raw context files must remain available and unchanged; the basic size check does
not detect every possible same-size alteration. No pending tail means no raw
context replay is needed. Downstream processing ignores this sidecar.

```sh
ngo-cnex-import run -p sbf --station data/my-setup \
  --resume-from data/my-setup/2025/08/15/import-state.json next.25_
```

By default, select the latest unconsumed daily state automatically. Explicit
`--resume-from` overrides selection; `--rebuild` disables automatic continuation.
Validate the selected state, Setup/protocol/antenna, published parts, and saved
input sizes. Invalid state fails, without trying an older state or fresh import.
Reusing the same recursive input directory skips previously read paths before
head probing, replays only the necessary tail, and imports new files. Partially
read files continue from the saved cursor. Print the state and skipped/new counts;
no new input or unread remainder is a successful `up-to-date` result. Input files
must be immutable; growing/replaced recordings are not silently accepted.

Tail completion produces the next `partNN` under the existing revision;
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

At each parsing-batch barrier where both time axes and any pending measurement
have passed an open day, close and publish its parts immediately. To keep the
checkpoint exactly aligned with emitted rows, the barrier also publishes any
completed rows already emitted for the next day as its first small part; later
batches append another part. No incomplete measurement group is published.
End of input publishes remaining completed records and saves pending raw context.
Publication does not reset scientific state.

Files are staged, closed and renamed without overwriting existing parts. Update
state after publishing parts. Completed publications survive a later parsing or
writing failure and can be resumed automatically. Do not read while writing. Failed/interrupted
publication requires manual resolution before readers run; no atomic multi-file
transaction or full crash recovery is promised. Old revisions are retained.

## Remaining work

- [x] Initializer, filename parsing/allocation and latest-revision selection.
- [x] Head-only ordering and independent observation/receiver-navigation checks.
- [x] Receiver-context RawBits time, nested GPST dates and incremental daily publication.
- [x] Automatic latest-state continuation and previously read input skipping.
- [x] RAWX/MeasEpoch decoding and native decimal Arrow observation batches.
- [x] Measurement completion events and daily Parquet writing.
- [x] Local raw-tail continuation with immutable parts and explicit rebuilding.
- [x] MeasExtra uncertainty, C/N0, lock/continuity and preprocessing corrections.
- [x] RAWX clock flags, SBF cumulative clock counters and independent smoothing state.
- [x] Expiring navigation anchors, nullable RawBits time and receiver-time continuation.
- [x] ReceiverStatus, ReceiverClock and PulseTiming Arrow/Parquet catalogs.
- [x] Uptime and fresh GPST/uptime restart evidence in Events.
- [x] Convert the receiver-clock analysis CLI to consume these CommonNEX catalogs.
- [x] UBX navigation completion independent of RawBits output.
- [ ] Cadence Events.
- [x] Gate borrowed UBX navigation/uptime pairs on new-uptime freshness;
  directly timestamped SBF status does not require a navigation anchor.
- [x] Apply SBF MeasEpoch E6BUsed to Type2 as well as Type1 observations.
- [x] Reviewed RawBits layouts and independent navigation-time association;
  distinguish sample-verified and documentary adapters in the coverage table.
- [ ] Additional input protocols and any explicitly requested reconciliation.
