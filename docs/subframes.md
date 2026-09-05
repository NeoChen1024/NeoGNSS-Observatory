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
`start_utc`/`end_utc`, not their filenames. Adjacent seconds remain in one
continuous group, including across midnight. Each group is fed through one
persistent native reader over a pipe: intermediate file boundaries do not
produce EOF or discard partial framing/parser state. Gaps start new groups;
overlaps are rejected. This does not yet add a stateful SBAS correction engine.

`group-NNNNN/` contains SBAS signal JSON Lines, the native summary, and
`sources.json`. A record's `offset` is in the concatenated group stream:
find the source range containing it and subtract `stream_begin` for its local
file offset. Source ranges also support a frame spanning a file boundary.
No exact SFRBX reception or transmission time is inferred from these ranges.

Each source is size/mtime checked and SHA256 verified while streaming. Group
outputs are hashed after writing and listed in `verified.json`. The run retains
implementation and worker snapshots, build configuration, Git/submodule
revisions, `run.json`, per-group diagnostic logs, and a flushed `groups.jsonl`
journal. `completed.json` is written only after every group succeeds.
Interrupted runs retain completed groups and partial output for inspection;
automatic resume is not implemented, and existing run directories are refused.

The output directory must not exist; its parent must exist. Input is read-only.
Use reconstructed UTC segments for Era A; exclude `unassigned/`. This tool
processes one recording per invocation, not an archive recursively.

Each `gnss-N_sv-N_sig-N_freq-N.jsonl` contains one RXM-SFRBX per line, with
the source byte offset (including UBX sync), original header fields and all
little-endian-decoded `words`. SBAS lines additionally contain canonical frame
hex, CRC results, message type, parser status, and typed content where supported.
`summary.json` records the input SHA256, byte/frame counts, stream identities,
and SBAS status/type counts; the same summary is printed to stdout. Record the
repository/submodule revisions and build options alongside production outputs.

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

## Experimental hourly VTEC maps

Install the package dependencies, then render from a completed batch extraction:

```sh
sbas-grid-render \
  --sbas-dir /path/to/era-a-sbas \
  --reconstruction-dir /path/to/era-a \
  --coastline contrib/natural-earth/ne_110m_coastline.zip \
  --output new-hourly-map-directory
```

`--start YYYY-MM-DDTHH` and exclusive `--end YYYY-MM-DDTHH` restrict an
experiment to UTC hours. `--vmin`, `--vmax`, and `--min-coverage` control the
fixed color scale and displayed coverage threshold. Defaults are 0–100 TECU
and 25%. `hourly.jsonl` retains values below the display threshold so later
rendering choices do not alter the aggregation result.

This experiment reconstructs the standard 2,192 IGP coordinates, associates
MT26 active-mask ordinals with MT18 masks having the same IODI, and keeps state
across continuous reconstructed files. A time gap starts fresh state. A new
IODI, changed same-IODI mask, expired/incomplete mask, MT0, `not_monitored`, or
`do_not_use` data is handled conservatively rather than filled with zero.

An accepted MT26 value is held until its next update or for at most 600 seconds;
complete masks age out after 1,200 seconds. Hourly means are weighted by the
number of valid seconds, split exactly at UTC-hour boundaries. `coverage` and
`valid_seconds` accompany every value. These timeout choices follow published
SBAS maximum intervals and are recorded in output provenance, but the result is
an exploratory visualization—not an aviation integrity implementation.

RXM-SFRBX has no timestamp. The command maps each extraction byte offset back
through reconstruction spans to the original payload-derived NAV/EOE epoch.
This is a reception-context UTC approximation, not an inferred SBAS transmit
time. No time is derived from an output filename.

VTEC is derived from the SBAS L1 vertical delay using the first-order relation
`VTEC = delay_m * 1575.42e6² / (40.3 * 1e16)`. It is not receiver-observed TEC.
The PNG overlay uses un-interpolated 5° point-centered cells, a fixed run-wide
extent and color scale, black 1:110m Natural Earth coastlines, and 10° graticules.
Missing or insufficient-coverage cells remain white. High-latitude cell shapes
are deliberately approximate; the JSON Lines grid points are authoritative.

Outputs include checksums for every PNG, input and source hashes, dependency
versions, and a snapshot of the aggregation source. The command requires a new
output directory and writes `completed.json` last. Interrupted output is not
automatically resumed.

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

Frame order comes only from `images.json`, not a filesystem glob. By default,
the command verifies every PNG checksum, checks the encoded codec, dimensions,
frame rate, frame count, and duration with `ffprobe`, and fully decodes the
finished video. It writes adjacent `.frames.jsonl` and `.json` files recording
the frame-to-hour mapping, checksums, exact command, actual tool versions, and
verification results. Use `--overwrite` to replace existing outputs only after
the new video passes verification.

## References and verification

- [u-blox integration manual](https://www.u-blox.com/sites/default/files/ZED-F9P_IntegrationManual_UBX-18010802.pdf): RXM-SFRBX navigation-word arrangement.
- [ESA Navipedia SBAS message format](https://gssc.esa.int/navipedia/index.php/The_EGNOS_SBAS_Message_Format_Explained): message contents and correction semantics.
- [ESA Navipedia ionospheric delay](https://gssc.esa.int/navipedia/index.php/Ionospheric_Delay): first-order delay/TEC relationship.
- [Natural Earth 1:110m physical vectors](https://www.naturalearthdata.com/downloads/110m-physical-vectors/): public-domain coastline source.
- [Pinned RTKLIB SBAS implementation](../contrib/RTKLIB/src/sbas.c): field-layout cross-reference; dependency revision is the repository gitlink.

Tests include independently byte-computed CRC fixtures, signed boundaries,
signal separation, invalid/unsupported frames, padding, and two small real
Era A MT18/26 vectors with recorded source offsets and SHA256. Tests require
neither the observation archive nor network access.
