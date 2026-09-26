# Realtime broadcast-message decoding

`ngo-broadcast-decode-realtime` consumes CommonNEX RawBits through a native
GPS/QZSS LNAV and SBAS L1 decoder. Receiver input is normalized by the shared CommonNEX
engine, not parsed again by the scientific consumer. No precise products,
orbit calculation or DCB calibration is required.

```sh
ngo-broadcast-decode-realtime -p sbf --host RECEIVER_IPV6 --port 2006 \
  --setup /data/station/setup.json > broadcast.jsonl

ngo-broadcast-decode-realtime -p ubx --input recording.ubx.xz \
  --setup /data/station/setup.json --snapshot-interval 60 \
  --output broadcast.jsonl
```

Choose exactly one of `--host` and `--input`. Plain and XZ replay are supported.
TCP batches default to five-second delivery; `--duration` limits TCP acquisition
only. `--output` exclusively creates a file, otherwise JSONL goes to stdout.
Diagnostics go to stderr. There is no automatic reconnect. Snapshot interval
defaults to 3,600 seconds and must be a positive integer.

## Implemented output

Each JSONL line has `type=message` and `kind`, or `type=snapshot`. Decimal time
values are strings, binary payloads are hexadecimal strings, and missing values
are JSON null. `output_sequence` orders emitted records within a decoder run;
it is not a durable identity or a CommonNEX reference.

| Message kind | Contents |
| --- | --- |
| `lnav_sf1`, `lnav_sf2`, `lnav_sf3` | Independently received clock/control or ephemeris fields |
| `ephemeris` | Fresh SF1-3 assembly, matching IODC/IODE, reference times and source |
| `almanac_entry` | One GPS/QZSS subject slot's orbit, clock and health |
| `almanac_epoch` | WNa/toa and ordered health slots |
| `configuration_health` | GPS per-slot configuration and health for slots 25-32 |
| `almanac_set`, `almanac_set_entries` | GPS source-local full collection summary and meaningful entries |
| `ionosphere_utc` | Klobuchar and UTC/leap-second parameters, with region/reference identity |
| `special_message` | Full 22-byte payload, escaped display and ICD character-set check |
| `nmct` | Availability indicator, 180-bit payload and unencrypted signed ERD codes |
| `qznma_payload` | Extracted 182-bit data region; no authentication verification |
| `sbas_*` | SBAS L1 fields, corrections, masks, GEO parameters, service regions and covariance factors; [complete field contract](broadcast-sbas.md) |

QZSS has no full-almanac completion output. SV ID zero is test mode, not an empty
member. GPS dummy pages count toward reception completeness but create no
satellite entry. Unsupported pages/families and failed integrity checks are
counted in diagnostics, not emitted as fabricated decoded parameters.

GPS almanac collection follows the section-20 nominal HOW/page schedule: the
32 subject slots plus SF5/SV51 and SF4/SV63. A collection lasts at most 2,250
seconds. Epoch/content conflicts reset it. Ephemeris assembly lasts at most 90
seconds and cannot use alert-marked fragments. Both restart from fresh pieces
after completion; duplicates do not extend the deadline. Complete unhealthy
parameters can be reported: health is separate from decoding/assembly success.

Angles/rates use radians and radians/second, lengths use meters, `sqrt_a` uses
sqrt(m). Clock offsets and time coordinates use decimal seconds with 12 places;
clock polynomial rates are float64. Klobuchar coefficients retain their standard
second/semicircle powers. `health_raw` and configuration codes require the
system-specific definitions in the linked ICDs. NMCT ERD code -32 is unavailable;
other unencrypted codes have a 0.3 m scale. No ERD correction is applied.

`subject_sv_id` is the almanac slot identity, not the broadcaster. QZSS slot to
signal PRN mapping can depend on L1C/A versus L1C/B. Normalization currently
covers QPNT-006 QZO slots 2-5 and GEO/QGEO slots 7-9. Other QZSS reference orbits
are not guessed: `orbit_reference_known=false` and normalized eccentricity/
inclination are null. Historical/expanded-PRN variants need further coverage.

## Snapshots

SBAS currently provides MessageOutput only. Its messages contribute to lifetime
statistics but are not inserted as usable correction candidates into snapshots.
Mask/issue association, correction aging and MT0 use policies belong downstream.

Snapshots describe state known in input order, not exact RF-time completeness.
When a non-null RawBits navigation context reaches/crosses `k * interval_s`
GPST seconds, emit each crossed snapshot before processing that new context's
record. The first grid target is strictly after the initial known context.
Batch boundaries do not trigger snapshots. End-of-input does not manufacture a
future snapshot, and no reliable RawBits time progress means no snapshot.

A snapshot JSON line contains `snapshot_gpst`, lifetime statistics and typed
`catalogs`. Arrow users receive these as separate batches with the same snapshot
coordinate. Almanac candidates alone are aggregated across broadcasters within
a system when reference epoch and parameters match. Per-candidate `sources`
retain contributing signals and receipt-context ranges. Conflicts remain
separate; unresolved epochs are not merged across sources. Collection summaries
currently report UNKNOWN expected membership, not a guessed complete constellation.

Ephemeris candidates are filtered by supported GPS fit/IODC rules and QZSS orbit/
clock fit intervals; `ORBIT_CLOCK_FIT` does not mean every signal is healthy.
Other retained parameter categories are explicitly `applicability=UNKNOWN` until
their detailed validity contracts are implemented. Seven-day cache retention is
only a resource bound, not a claim of validity. Special messages, NMCT and QZNMA
payloads are MessageOutput only. No interpolation, source ranking or bias
correction is performed.

No historical snapshot query exists. A discontinuity clears incomplete assemblies
and restarts scheduling; completed candidates remain subject to their temporal
filters. Time reversal without a declared discontinuity is an error. Untimed
restart Events are rejected rather than assigned a fabricated position.

## Python API and bounds

```python
from neognss_observatory.broadcast_realtime import BroadcastMessageDecoder

decoder = BroadcastMessageDecoder(setup_id, snapshot_interval_s=3600)
outputs = decoder.feed(common_nex_batch_group)
# outputs maps each kind to a list of typed pyarrow.RecordBatch objects.
# For raw-bits-only callers:
outputs = decoder.process(raw_bits_batch)
```

`process` accepts a RecordBatch, Table or iterable of batches. Such callers own
discontinuity delivery via `decoder.native.discontinuity()`; `feed` handles
restart Events and group notices. Inputs and outputs remain independently owned.
Native decoding/assembly releases the GIL; low-rate snapshot aggregation and
JSONL presentation are Python consumers of typed results.

Use bounded batches. The decoder allows 8,192 retained candidates, approximately
1,024 source assembly states and 100,000 emitted native rows per feed. Exceeding
a bound fails explicitly rather than silently dropping valid output. A failed
decoder must be reconstructed. Full health interpretation, other navigation
families, persistent recovery and replacement of the older orbit adapter remain
in the [decoder roadmap](broadcast-message-decoder.md).
