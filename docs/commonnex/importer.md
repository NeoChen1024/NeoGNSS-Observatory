# CommonNEX batch importer pilot

`neo-cnex-import` is an implemented observation-first pilot, not full CommonNEX
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

Create one Setup directory with a Stream beneath it. Start from the
[example Setup](../../config/commonnex-setup.example.json) and fill actual metadata;
the example is not a calibrated station description.

```sh
neo-cnex-import init data/my-setup \
  --setup config/my-setup.json --stream-id main --antenna-name main
```

The initializer imports `setup.json`, its optional same-directory vendor config,
and creates `main/stream.json`. `--source-antenna` selects the native antenna
index (default 0; RAWX currently requires 0). Scientific antenna identity remains
the named Setup reference. Initialization validates essential references and
period representation; it is not a complete Setup JSON Schema validator.
Existing output directories are not overwritten. Additional Stream initialization
inside an existing Setup is not yet exposed by this pilot.

## Import local recordings

```sh
neo-cnex-import run -p sbf --stream data/my-setup/main first.25_ second.25_
neo-cnex-import list data/my-setup/main
```

Inputs are explicit expanded files in caller-supplied recording order. Use one
continuous recording path per invocation; do not mix overlapping logger copies.
The importer does not acquire FTP data, decompress XZ, or require a QA stamp.
Choose `-p ubx` for RAWX. Frame-validated foreign-protocol messages are skipped
atomically with a throttled warning and counters.

Ordinary import refuses an already existing day/catalog rather than guessing
whether data is a duplicate, a missing interval, or a replacement. Summary JSON
is on stdout; processing progress and warnings are on stderr.

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
neo-cnex-import run -p sbf --stream data/my-setup/main \
  --resume-from data/my-setup/main/2025-08-15/import-state.json next.25_
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
neo-cnex-import run -p sbf --stream data/my-setup/main --rebuild complete-day.25_
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
- [x] RAWX/MeasEpoch decoding and native decimal Arrow observation batches.
- [x] Measurement completion events and daily Parquet writing.
- [x] Local raw-tail continuation with immutable parts and explicit rebuilding.
- [x] MeasExtra uncertainty, C/N0, lock/continuity and preprocessing corrections.
- [ ] Cadence, clock and navigation epoch Events, with context across invocations.
- [ ] Verified RawBits families and their navigation-time association.
- [ ] Additional input protocols and any explicitly requested reconciliation.
