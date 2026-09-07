# Navigation subframes and SBAS

`libcppubx2` separates RXM-SFRBX container decoding, signal routing, and
constellation-specific content decoding. It does not need Python at runtime.

## Export a recording

After building the examples:

```sh
build/libcppubx2/cppubx2_subframes input.ubx new-output-directory
```

Append `--sbas-only` to suppress other constellations' signal output.

### Batch extraction from reconstructed Era A

```sh
python -m neognss_observatory.sbas_extract \
  --input-dir /path/to/era-a \
  --output new-sbas-output \
  --worker build/libcppubx2/cppubx2_subframes
```

The batch application validates the completed reconstruction's plan and artifact
metadata, excludes `unassigned/`, and groups segments using payload-derived
`start_gpst`/`end_gpst`, not their filenames. Segments within the reconstruction's
recorded gap timeout remain in one continuous group, including across midnight.
Each group is fed through one
persistent native reader over a pipe: intermediate file boundaries do not
produce EOF or discard partial framing/parser state. Gaps start new groups;
overlaps are rejected. This does not yet add a stateful SBAS correction engine.

`group-NNNNN/` contains SBAS signal JSON Lines, the native summary, and
`sources.json`. A record's `offset` is in the concatenated group stream:
find the source range containing it and subtract `stream_begin` for its local
file offset. Source ranges also support a frame spanning a file boundary.
No exact SFRBX reception or transmission time is inferred from these ranges.

Sources are size/mtime checked while streaming. The run retains per-group
diagnostics, source-span mappings, a `groups.jsonl` journal and a concise summary.
No worker/source snapshots or per-output hash inventories are produced.
Research directory outputs are published by rename. `--overwrite` retains the
previous output as a backup; failed temporary output remains for inspection.

Use reconstructed GPST segments for Era A; exclude `unassigned/`. This tool
processes one recording per invocation, not an archive recursively.

Each `gnss-N_sv-N_sig-N_freq-N.jsonl` contains one RXM-SFRBX per line, with
the source byte offset (including UBX sync), original header fields and all
little-endian-decoded `words`. SBAS lines additionally contain canonical frame
hex, CRC results, message type, parser status, and typed content where supported.
`summary.json` records byte/frame counts, stream identities,
and SBAS status/type counts; the same summary is printed to stdout. Record the
repository/submodule revisions and build options only for explicitly requested production runs.

`errors.jsonl` records malformed containers with their original UBX body, and
noise byte ranges. Unsupported SBAS message types stay in their signal stream.
Unknown SFRBX versions go to the error stream, not a guessed signal stream.
A complete summary means the input was processed, not that every frame decoded.
Output failures, truncation, and input I/O errors return nonzero; a partial
directory is not resumable and has no complete summary. The strict UBX reader
is not a corruption-salvage scanner: a corrupt length may consume later bytes.
Use stable input files; this example does not lock inputs against mutation.

## Library API and routing

```cpp
#include <cppubx2/sbas.hpp>

UBX::SubframeDemultiplexer router;
// frame is a checksum-validated UBX::ubx_frame.
auto result = router.dispatch(frame, [](const UBX::NavigationSubframe &s) {
    // Use s.signal as the key for application-owned per-signal state.
    if (s.signal.gnssId == 1) {
        auto decoded = UBX::SBAS::parse(s);
        // Check decoded.status before consuming typed content.
    }
});
```

`parse_subframe(frame)` is the stateless alternative. Only SFRBX version 2
is currently supported, with exact payload-length validation. The routing key
contains raw `gnssId`, `svId`, and `sigId`, plus `freqId` for GLONASS. The latter
is encoded as frequency slot + 7, not a signed channel number. Other systems
normalize the key's frequency to zero but retain `raw_freqId`. Receiver tracking
channel `chn` is preserved, not used as satellite identity.

GPS, SBAS, Galileo, BeiDou, QZSS, GLONASS, NavIC, and unknown numeric identifiers
remain separate. `prn()` provides a convenience mapping (including QZSS
`svId + 192`); GLONASS retains slot/frequency identity and returns no PRN.
Different PRNs and signals never share a routing key. Unknown GLONASS slot 255
cannot be resolved to a physical satellite by this API.

The router retains counts, not a growing history. Callers own buffering, files,
and parser state. Add future constellation parsers beside `sbas.hpp`; do not
reinterpret another GNSS merely because its frame resembles SBAS.

RXM-SFRBX supplies no reception timestamp. Source offsets are provenance, not
time. Any later association with NAV timestamps must explicitly preserve its
inference and time scale; filenames alone are insufficient.

## SBAS L1 content support

Scope is `gnssId=1, sigId=0` (L1 C/A), not SBAS L5 or QZSS L1S. Decode the
first eight U4 words MSB-first into 250 over-air bits. Six trailing padding bits
are retained separately and excluded from CRC-24Q. Validate the preamble and
24-bit CRC before exposing typed content. The Era A sample also has nine-word
reports: the ninth word is retained as `Message::trailing_word` and in exported
`words`, but its meaning is deliberately unspecified. Other lengths are rejected.

| Message type | Typed content |
| --- | --- |
| 0 | Test-mode marker; optional corrections are not applied |
| 1 | PRN mask and IODP |
| 2–5 | Fast corrections, UDREI, IODF, IODP |
| 6 | Integrity indicators |
| 7 | Fast-correction degradation indices |
| 9 | GEO navigation raw signed fields and time-of-day field |
| 18 | Ionospheric band mask and IODI |
| 26 | Ionospheric delays, GIVEI, band/block and IODI |
| 63 | Null-message marker |

Other types, including 10, 12, 17, 24, 25, 27, 28 and 62, return
`unsupported_message` with the original frame retained. Invalid frames never
yield typed content. This is content extraction, not a complete SBAS correction
engine or safety/integrity assessment.

Raw numeric fields remain available. Only explicitly suffixed `_m` helpers and
JSON fields are scaled; MT9 position, velocity, acceleration and clock fields
remain raw wire integers. MT26 delay 511 is `do_not_use` with no numeric delay;
GIVEI 15 is `not_monitored`, even if a numeric delay is present. Numeric values
alone do not establish that a correction is safe to apply.

MT18 mask positions are one-based static mask bits. MT26
`active_mask_ordinal` is instead an ordinal into the **active** MT18 mask,
starting at `15 * block + 1`. The stateless C++ parser validates field
extraction, CRC, and band/block bounds, not every reserved bit or cross-message
semantic constraint. The Python hourly-map experiment described below adds
conservative matching and aging independently per SBAS signal.

## Daily Parquet pipeline

Use two independent commands for new grid processing runs:

```sh
sbas-grid-parquet \
  --input-dir /path/to/reconstructed-ubx \
  --worker build/libcppubx2/cppubx2_subframes \
  --output new-sbas-grid-directory

sbas-grid-plot \
  --input-dir new-sbas-grid-directory \
  --coastline contrib/natural-earth/ne_10m_coastline.zip \
  --output new-sbas-map-directory \
  --workers 4
```

The first command reads raw UBX bytes from a completed GPST reconstruction,
not RINEX. It reuses the native SFRBX decoder and retains its signal JSONL,
raw words, CRC results, diagnostics and source-offset mappings in
`frames/`. It then reads these inputs and computes MT18/26 grid validity
intervals, keeping each signal's state across continuous source files and GPST
midnight. Separate continuous groups start fresh state. The reconstruction's
group-boundary policy and SBAS mask/correction aging remain distinct policies.

Each `daily/GPST-YYYY-MM-DD.parquet` contains one GPST day, across all received
SBAS signals. A row represents an accepted IGP value over the half-open interval
`[start_gpst_ms, end_gpst_ms)`. Midnight splits the interval, not the parser
state. Integer times are milliseconds since 1980-01-06 00:00:00 GPST; they are
not Unix timestamps. Time remains an approximate NAV/EOE reception context,
not a measured SFRBX reception time or SBAS transmission time.

Rows preserve signal identity, band, mask bit, coordinates, IODI, GIVEI,
vertical delay in meters, equivalent VTEC in TECU, and the originating MT26
group-stream offset. That offset references `frames/group-NNNNN/`; it is not
a local file offset. Source mappings and the retained MT18 messages allow
replaying the mask association. No values from different PRNs are averaged
together. This is SBAS-broadcast equivalent VTEC, not receiver-observed STEC.

The experimental state policy retains the existing 600-second correction and
1,200-second complete-mask limits. Invalid/unmonitored values, missing masks,
and expired state yield no valid interval, never zero TEC. Their original
messages remain in `frames/`. Days without any valid interval have no Parquet
file. At group EOF, integration stops at the final observed epoch: this pipeline
does not add one second or extrapolate a receiver cadence. Consequently its
last-hour coverage can differ from the earlier combined experiment.

The producer uses bounded row buffers and temporary shards, then compacts to
one Zstandard level-3 Parquet file per day. It removes only its own temporary
shards after successful compaction. State processing is sequential within a
signal; a day boundary is not a safe independent parser restart point.

The second command needs only the completed grid product and coastline, not
UBX, reconstruction indexes, or `frames/`. `--start YYYY-MM-DDTHH` and exclusive
`--end YYYY-MM-DDTHH` select GPST hours; only intersecting daily files are read
and aggregated. The GPST day is read from Parquet metadata, not inferred solely
from the filename. Either a product directory or its `daily/` directory is accepted. It processes one day at a time, sums TECU-seconds and valid
duration across intervals, and divides to obtain time-weighted hourly means.
`hourly/GPST-YYYY-MM-DD.jsonl` retains the mean, integral, valid seconds and
coverage, including cells below the display threshold. Future longer-window
averages must combine integrals and durations, not average hourly means equally.

`--vmin`, `--vmax` and `--min-coverage` default to 0 TECU, 100 TECU and 25%.
PNG workers render independent signal-hours in parallel, including compression;
`--png-compression` defaults to 3. Extent and color scale are fixed across the
selected run. Images use the existing `png/` layout and `images.json` manifest,
compatible with the video helper. Changing display settings does not rerun
UBX parsing or SBAS state reconstruction.

If extraction completed but grid processing failed, reuse that extraction without
rescanning UBX or requiring a native worker:

```sh
sbas-grid-parquet \
  --input-dir /path/to/reconstructed-ubx \
  --sbas-dir previous-sbas-grid-directory/frames \
  --output new-sbas-grid-directory
```

This restarts grid calculation, not raw extraction. The caller supplies the
matching reconstruction for its byte-to-time mapping. Keep the referenced
extraction for raw-message inspection; the new output does not copy it or demand
hash-chain sidecars. Epoch-index mappings use an LRU cache of 16 open indexes.
Cache eviction does not reset SBAS state.

Both commands publish directory outputs by rename; `--overwrite` keeps the old
directory as a backup. Grid Parquet embeds GPST day, units, time basis and aging
parameters. Plotting needs only daily Parquet and coastline, not completion
manifests or extraction files. Concise summaries are informational. Interrupted
calculation is not automatically resumed.

Daily Parquet rows need not be globally time-sorted across signals; intervals
are emitted chronologically within each signal/IGP. Consumers must not assume
global order. The product is an exploratory visualization input, not a complete
aviation correction/integrity engine.

## Earlier combined hourly VTEC experiment

The original `sbas-grid-render` command remains available for existing workflows.
New runs should prefer the independent Parquet producer and plotter above.

Install the package dependencies, then render from a completed batch extraction:

```sh
sbas-grid-render \
  --sbas-dir /path/to/era-a-sbas \
  --reconstruction-dir /path/to/era-a \
  --coastline contrib/natural-earth/ne_10m_coastline.zip \
  --output new-hourly-map-directory
```

`--start YYYY-MM-DDTHH` and exclusive `--end YYYY-MM-DDTHH` restrict an
experiment to GPST hours. `--vmin`, `--vmax`, and `--min-coverage` control the
fixed color scale and displayed coverage threshold. Defaults are 0–100 TECU
and 25%. `hourly.jsonl` retains values below the display threshold so later
rendering choices do not alter the aggregation result.

`--workers` (default up to four) renders independent hours in separate
processes, including PNG compression. `--png-compression` selects level 0-9
(default 3). These options do not change the hourly aggregation or map extent.

This experiment reconstructs the standard 2,192 IGP coordinates, associates
MT26 active-mask ordinals with MT18 masks having the same IODI, and keeps state
across continuous reconstructed files. A time gap starts fresh state. A new
IODI, changed same-IODI mask, expired/incomplete mask, MT0, `not_monitored`, or
`do_not_use` data is handled conservatively rather than filled with zero.

An accepted MT26 value is held until its next update or for at most 600 seconds;
complete masks age out after 1,200 seconds. Hourly means are weighted by the
number of valid seconds, split exactly at GPST-hour boundaries. `coverage` and
`valid_seconds` accompany every value. These timeout choices follow published
SBAS maximum intervals and are recorded in output provenance, but the result is
an exploratory visualization—not an aviation integrity implementation.

RXM-SFRBX has no timestamp. The command maps each extraction byte offset back
through reconstruction spans to the original payload-derived NAV/EOE epoch.
This is a reception-context GPST approximation, not an inferred SBAS transmit
time. No time is derived from an output filename.

VTEC is derived from the SBAS L1 vertical delay using the first-order relation
`VTEC = delay_m * 1575.42e6² / (40.3 * 1e16)`. It is not receiver-observed TEC.
The PNG overlay uses un-interpolated 5° point-centered cells, a fixed run-wide
extent and color scale, black 1:10m Natural Earth coastlines, and 10° graticules.
Missing or insufficient-coverage cells remain white. High-latitude cell shapes
are deliberately approximate; the JSON Lines grid points are authoritative.

Outputs retain hourly values, image ordering and calculation settings, without
source snapshots or hash inventories. Directory publication and `--overwrite`
follow the same research-output policy.

### HEVC/MP4 preview

Encode the PNG manifest in its recorded order with a Vulkan Video HEVC encoder:

```sh
./scripts/encode-sbas-map-video \
  --images-manifest work/era-a-vtec-hourly/images.json \
  --output work/era-a-vtec-hourly/era-a-prn137-hourly-vtec-5fps-hevc.mp4 \
  --title "Era A SBAS PRN 137 hourly mean VTEC"
```

The defaults are 5 fps, Vulkan physical device 0, CQP 24, an `hvc1` MP4 stream,
and `faststart` metadata placement. Native dimensions are preserved when both
dimensions are even; an odd dimension is padded by one pixel because the NV12
hardware path requires even dimensions. The command never rescales.

Frame order comes only from `images.json`, not a filesystem glob. The command
checks PNG headers/dimensions and the encoded codec, dimensions, frame rate,
frame count and duration with `ffprobe`. Full decoding is opt-in with
`--verify-output`. An adjacent `.frames.jsonl` retains the frame-to-hour mapping;
there is no environment/hash provenance bundle. Use `--overwrite` to replace an
existing video after the new video passes the requested checks.

## References and verification

- [u-blox integration manual](https://www.u-blox.com/sites/default/files/ZED-F9P_IntegrationManual_UBX-18010802.pdf): RXM-SFRBX navigation-word arrangement.
- [ESA Navipedia SBAS message format](https://gssc.esa.int/navipedia/index.php/The_EGNOS_SBAS_Message_Format_Explained): message contents and correction semantics.
- [ESA Navipedia ionospheric delay](https://gssc.esa.int/navipedia/index.php/Ionospheric_Delay): first-order delay/TEC relationship.
- [Natural Earth 1:10m coastline](https://www.naturalearthdata.com/downloads/10m-physical-vectors/10m-coastline/): public-domain coastline source.
- [Pinned RTKLIB SBAS implementation](../contrib/RTKLIB/src/sbas.c): field-layout cross-reference; dependency revision is the repository gitlink.

Existing core C++ checks cover field extraction, CRC, signal separation and
invalid/unsupported frames. Script output contracts are intentionally not frozen
by integration tests during pre-Alpha development.
