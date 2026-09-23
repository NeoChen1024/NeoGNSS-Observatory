# Navigation subframes and SBAS

The processing boundary is the SBAS message, not its UBX/SBF transport.

## Daily Parquet pipeline

```sh
# Optional read-only QA; extraction does not require evidence of this run.
ngo-dataset-qa --input-dir /data/ubx
# Initialize once with actual station metadata, then supply raw files.
ngo-cnex-import init /data/cnex --setup /data/setup.json
ngo-cnex-import run -p sbf --station /data/cnex /data/first.sbf /data/second.sbf

ngo-sbas-grid-parquet --input-dir /data/cnex --output /data/sbas-grid

ngo-sbas-grid-plot --input-dir /data/sbas-grid --output /data/sbas-maps
```

Import, grid calculation and plotting are separate commands. Grid reads the
ParquetNEX station's latest RawBits and Events revisions and all their parts.
It has no protocol selection, raw-data path or reconstruction dependency.

The `SBAS input rows` progress bar counts all scanned RawBits and Events rows,
including records excluded from grid calculation; it is not a count of usable
SBAS frames. Output progress counts grid intervals, not elapsed GNSS time.

### Explicit wire protocol

Only `ngo-cnex-import run` uses `--protocol/-p ubx|sbf` (default UBX).
Complete checksum-valid foreign frames are skipped atomically
with throttled stderr warnings and skipped frame/byte counts. Invalid wire
frames follow the decoder's corruption/resynchronization policy; incomplete
tails remain unpublished and can be continued with the import sidecar.

Inputs are expanded files, supplied explicitly or discovered with importer
`-r/--recursive`; head probes determine their import order.
Observation and receiver navigation reversals abort import without overlap removal.
RawBits uses current receiver navigation context; source SIS timestamps are not
retained or used as fallback. Canonical bits remain unchanged.
Use reconstructed Era A
segments, excluding unassigned data, or nonoverlapping raw UBX/SBF recordings.
No reconstruction index, QA stamp or XZ decompression is
required. UBX navigation association uses fresh NAV-TIMEGPS; EOE is only a
navigation completion boundary and does not gate RawBits association,
independently of RAWX measurement time. SBF GEORawL1 uses synchronous receiver navigation time,
without claiming that an individual raw block completes a navigation epoch.
Unusable RawBits time is retained as null by import and skipped by grid,
never invented. See the
[importer guide](commonnex/importer.md) for continuation and coverage limits.

Continuous file/day boundaries preserve framing and signal state. Grid's
`--gap-timeout` (default 50 seconds) closes state after a navigation-context or
per-satellite SBAS reception gap. This is a downstream conservative coverage
policy, not receiver restart evidence or a CommonNEX format requirement.
At the selected input end, integration stops at the last available navigation
context (or final SBAS time if no context events exist), without extrapolation.
This is not a permanent end-of-stream marker in the input dataset.

### RawBits product

`YYYY/MM/DD/r00-raw-bits-part00.parquet` follows the
[CommonNEX RawBits fields](commonnex/raw-bits.md). SBAS L1 uses
`SBAS_L1_250_V1`, with DECIMAL(38,12) `nav_epoch_gpst`, normalized broadcaster
identity, `SBAS_L1` bitstream source and separately scoped independent/receiver
CRC checks. Failed checks remain records. The grid adapter projects these exact
timestamps to integer milliseconds for its existing aging engine; source
Parquet timestamps are unchanged. Navigation completion Events provide context
separately; observation completion never anchors SBAS reception time.

The SBAS body includes its own preamble, message type and CRC. UBX/SBF headers,
checksums, native field dictionaries, ninth UBX words, byte offsets and filenames
are not copied into the product. Raw archives preserve the transport. Invalid
and unsupported complete SBAS bodies remain available for inspection/re-decoding.

### Grid and map products

Grid verifies CRC evidence against the SBAS bodies. Receiver-rejected frames
do not update masks or corrections.
Independent stream state spans Parquet files and GPST midnight. Grid consumes
navigation completion Events and rejects unsupported navigation event kinds;
receiver-scope restart Events are currently filtered out, not applied as grid
boundaries. The importer already emits these Events. Until grid gains an
explicit mapping, short receiver restarts can incorrectly retain previous
mask/aging state; a navigation/reception gap is not a substitute for restart
handling.

- [ ] Consume receiver restart Events in grid, defining timed and untimed
  boundaries without carrying masks across an established restart.

Grid schema version 3 contains RINEX `satellite_system`/`satellite_number`
identity (`S`/`37`, displayed as `S37`), signal, `stream_id`,
`frame_id`, IGP band/mask position, coordinates, IODI, GIVEI, delay in meters,
equivalent VTEC in TECU, and `[start_gpst_ms,end_gpst_ms)`. The derived grid's
integer `stream_id` is a run-local continuity-segment counter, not a CommonNEX
station identity or a reference to a Stream metadata file. `frame_id` points
to a run-local occurrence counter for the originating MT26, not a Parquet
foreign key or byte offset. SBAS
bodies are not duplicated for every grid cell. Midnight splits intervals, not
state. No raw source references or transport identifiers are persisted.

Correction/mask ages remain 600/1200 seconds. Missing masks, expired state,
invalid and unmonitored values produce no interval, not zero TEC. Values from
different satellites are never averaged together. This is SBAS-broadcast equivalent
VTEC, not receiver-observed STEC. Empty days have no grid Parquet.

Grid publishes daily Parquet after successful processing; `--overwrite`
retains the previous output as a backup.
RawBits publication uses ParquetNEX revision/part rules. Grid does not require
the import-state sidecar, completion summaries or raw sources.

`ngo-sbas-grid-plot` uses grid Parquet and the bundled Natural Earth 10m coastline.
Use `--coastline PATH` to supply another coastline ZIP. It computes
valid-duration-weighted hourly means; `--min-coverage` defaults to 0.25.
Hourly grid records are stored in `hourly/GPST-YYYY-MM-DD.parquet`, using
Zstandard level 3 and explicit GPST, coordinate and VTEC units. Each file
contains that day's hourly means, valid durations and coverage fractions;
these are the reusable hourly products for rendering.
`--start/--end YYYY-MM-DDTHH` select GPST hours with an exclusive end.
Missing cells stay absent. See `--help` for rendering controls. Color limits
default to 0–200 TECU. The 5-degree cell
overlay and map coastlines are illustrative, not precision coverage polygons.

## Library API and routing

See [native architecture](native-architecture.md) for library ownership and
the [processing API](../libneognss-obs/README.md) for direct callers. Grid uses
CommonNEX records, not receiver transport fields.

## SBAS L1 content support

Scope is SBAS L1 C/A, not SBAS L5 or QZSS L1S. Canonical body layout and
receiver mappings are defined in [RawBits layouts](commonnex/raw-bits-layouts.md);
transport words and framing remain in the raw archive.

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
ngo-sbas-map-video \
  --images-manifest work/era-a-vtec-hourly/images.json \
  --output work/era-a-vtec-hourly/era-a-S37-hourly-vtec-5fps-hevc.mp4 \
  --title "Era A SBAS S37 hourly mean VTEC"
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
