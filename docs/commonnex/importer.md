# CommonNEX batch importer

`ngo-cnex-import` reads local UBX/SBF recordings into the current experimental
CommonNEX schemas. Raw archives remain the preservation masters. Readers use
current fields directly; no legacy-schema migration or compatibility layer is
provided. Existing products are never rewritten merely because schemas change.

## Implemented path

The importer produces `observations`, `raw-bits`, `events` and
`receiver-telemetry`, independently according to available input. See
[receiver mappings](receiver-mappings.md) for source revisions, normalization,
MeasExtra association, RawBits packing and validation limits; see
[ParquetNEX](parquetnex.md) for persisted schemas and naming.

Completion and receiver-restart Events are implemented; cadence Events, Meas3
and overlap reconciliation are not. RINEX/RTCM3 input is intentionally excluded,
not a pending adapter. No decoded-navigation catalog is planned.

Telemetry follows the common [receiver-telemetry contract](receiver-telemetry.md),
including temperature, uptime, CPU load and ordered clock/pulse reports, rather
than vendor status structs. It is assembled one PVT epoch late. File/day/chunk
boundaries retain pending state; use `--finalize-telemetry` only at a true stream
end. Unknown RawBits/telemetry time follows [receiver time](receiver-time.md).

STEC consumes Observation and receiver-restart Events; receiver-clock consumes
telemetry and Events; SBAS grid selects SBAS L1 RawBits. Each processor applies
its own missing-time/validity policy. See their tool documentation rather than
treating successful import as proof that all processors can use every row.

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
The sidecar also preserves receiver-time/completion and pending telemetry state.
It is private operational state, not a science catalog or a portable generic
decoder checkpoint. Resume validates its supported state format and import
configuration; incompatible state fails instead of silently starting over.
Downstream readers never depend on it.
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

See [remaining work](TODO.md) for explicitly deferred capabilities.
