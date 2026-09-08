# Navigation subframes and SBAS

The processing boundary is the SBAS message, not its UBX/SBF transport.

## Daily Parquet pipeline

```sh
# Optional read-only QA; extraction does not require evidence of this run.
ngo-dataset-qa --input-dir /data/ubx
# Choose expanded raw UBX or SBF only at extraction.
sbas-frame-parquet -p ubx --input-dir /data/ubx --output /data/sbas-frames
# Alternatively:
sbas-frame-parquet -p sbf --input-dir /data/raw-sbf --output /data/sbas-frames

sbas-grid-parquet --input-dir /data/sbas-frames --output /data/sbas-grid

sbas-grid-plot --input-dir /data/sbas-grid --output /data/sbas-maps \
  --coastline contrib/natural-earth/ne_10m_coastline.zip
```

Extraction, grid calculation and plotting are separate commands. Grid accepts
only frame Parquet and has no protocol selection, raw-data path, reconstruction
dependency, or source-offset lookup. Input schemas and stream end records
carry the information required for independent downstream processing.

### Explicit wire protocol

Only `sbas-frame-parquet` uses `--protocol/-p ubx|sbf` (default UBX,
case-insensitive). Complete checksum-valid foreign frames are skipped atomically
with throttled stderr warnings and skipped frame/byte counts. Invalid wire
frames follow the decoder's corruption/resynchronization policy; corrupt lengths
can still result in a truncated-tail error.

UBX input can be raw nonoverlapping recordings or reconstructed files;
`unassigned/` is excluded. No completion manifest or reconstruction index is
opened. A shared native epoch assembler resolves SFRBX GPST from NAV-TIMEGPS
and supported RAWX anchors while streaming, buffering messages until their
epoch is resolved. It is also used by QA/reconstruction, but extraction does
not collect or rerun full QA diagnostics. Files are traversed recursively in
path order; filenames never supply time. SBF input
recursively selects expanded `.sbf`, `.YY_`, and `.ubx` candidates in path
order; arrange files in stream order and do not mix overlapping recordings.
No XZ decompression occurs. SBF uses GEORawL1 TOW/WNc directly. Missing time
and reversed timestamps fail rather than receiving invented timestamps.

Continuous file/day boundaries preserve framing and signal state. UBX starts
a new stream after a navigation-epoch gap greater than `--gap-timeout`. SBF starts a new per-signal stream after a
gap greater than `--gap-timeout` (default 50 seconds), considering every SBAS
message type. This is a conservative signal-coverage rule, not proof of reboot.
At a gap or EOF, stream closure stops at the last observed time; UBX may close
at its last navigation epoch. No extra second or receiver cadence is invented.

### Frame product

`daily/GPST-YYYY-MM-DD.parquet` uses frame schema version 1:

| Field | Meaning |
| --- | --- |
| `gpst_ms` | Integer milliseconds since 1980-01-06 00:00:00 GPST |
| `constellation / prn / signal` | Canonical signal identity; currently SBAS L1CA |
| `time_basis` | `navigation_epoch_context` or `receiver_message_time`; neither asserts SBAS transmit time |
| `stream_id` | Product-local continuous signal stream; preserved across daily partitions |
| `kind` | `frame` or `end` |
| `frame_id` | Product-local frame identity, null on end records |
| `frame` | Fixed 32 bytes: 250 SBAS bits, MSB-first, final six padding bits zero |
| `crc_valid` | Independent SBAS CRC result; null on end records |
| `accepted` | Receiver acceptance when supplied, otherwise null; false blocks grid updates |

End records carry the observed closure time with null payload/validity fields.
They preserve integration boundaries without raw files or extra source journals.
Each stream's records remain ordered within a day; days are consumed in order.
Only complete stream ranges are accepted by grid; missing end records fail.

The SBAS body includes its own preamble, message type and CRC. UBX/SBF headers,
checksums, native field dictionaries, ninth UBX words, byte offsets and filenames
are not copied into the product. Raw archives preserve the transport. Invalid
and unsupported complete SBAS bodies remain available for inspection/re-decoding.

### Grid and map products

Grid re-decodes SBAS bodies in bounded C++ batches and verifies CRC metadata
against their bytes. Receiver-rejected frames do not update masks or corrections.
Independent stream state spans Parquet files and GPST midnight. Explicit end
records close it; the grid has no protocol-specific continuity branches.

Grid schema version 3 contains canonical signal identity, `stream_id`,
`frame_id`, IGP band/mask position, coordinates, IODI, GIVEI, delay in meters,
equivalent VTEC in TECU, and `[start_gpst_ms,end_gpst_ms)`. `frame_id` points
to the originating MT26 in the input frame product, not a byte offset. SBAS
bodies are not duplicated for every grid cell. Midnight splits intervals, not
state. No raw source references or transport identifiers are persisted.

Correction/mask ages remain 600/1200 seconds. Missing masks, expired state,
invalid and unmonitored values produce no interval, not zero TEC. Values from
different PRNs are never averaged together. This is SBAS-broadcast equivalent
VTEC, not receiver-observed STEC. Empty days have no grid Parquet.

Both products use bounded buffers and temporary shards, compacted into daily
Zstandard level-3 Parquet without keeping all day writers open. Directory
publication is atomic; `--overwrite` retains the previous output as a backup.
Concise completion summaries are informational; grid reads frame schemas and
end records directly without requiring completion JSON or raw sources.

`sbas-grid-plot` uses only grid Parquet and a coastline asset. It computes
valid-duration-weighted hourly means; `--min-coverage` defaults to 0.25.
`--start/--end YYYY-MM-DDTHH` select GPST hours with an exclusive end.
Missing cells stay absent. Rendering/PNG compression use independent worker
processes (`--workers`), with compression level 3 by default
(`--png-compression`). Color limits default to 0–100 TECU. The 5-degree cell
overlay and map coastlines are illustrative, not precision coverage polygons.

## Library API and routing

```cpp
#include <cppgnss/ubx_subframe.hpp>

UBX::SubframeDemultiplexer router;
// frame is a checksum-validated UBX::ubx_frame.
auto result = router.dispatch(frame, [](const UBX::NavigationSubframe &s) {
    // Use s.signal as the key for application-owned per-signal state.
    if (s.signal.gnssId == 1) {
        auto decoded = UBX::parse_sbas(s);
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

RXM-SFRBX supplies no reception timestamp. Source offsets are not time.
The input adapter resolves navigation-epoch context before writing frame
Parquet; filenames alone are insufficient.

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
semantic constraint. The Observatory native grid processor adds conservative
matching and aging independently per SBAS signal.

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

## References

- [u-blox integration manual](https://www.u-blox.com/sites/default/files/ZED-F9P_IntegrationManual_UBX-18010802.pdf): RXM-SFRBX navigation-word arrangement.
- [ESA Navipedia SBAS message format](https://gssc.esa.int/navipedia/index.php/The_EGNOS_SBAS_Message_Format_Explained): message contents and correction semantics.
- [ESA Navipedia ionospheric delay](https://gssc.esa.int/navipedia/index.php/Ionospheric_Delay): first-order delay/TEC relationship.
- [Natural Earth 1:10m coastline](https://www.naturalearthdata.com/downloads/10m-physical-vectors/10m-coastline/): public-domain coastline source.
- [Pinned RTKLIB SBAS implementation](../contrib/RTKLIB/src/sbas.c): field-layout cross-reference; dependency revision is the repository gitlink.
