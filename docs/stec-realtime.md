# Realtime relative STEC

`ngo-stec-realtime` is a CommonNEX streaming consumer example. It emits one JSONL
record per completed observational epoch, with GPS/QZSS/Galileo/BeiDou signal-pair relative
STEC and ionospheric pierce-point (IPP) coordinates. No RINEX conversion, CDDIS
products, DCB estimation or precise-orbit download is needed.

## Run

Supply actual station marker XYZ and marker-to-ARP NEU in Setup; example
coordinates are not a station survey. The setup is read without modification.

```sh
ngo-stec-realtime -p sbf --host RECEIVER_IPV6 --port 2006 \
  --setup /data/station/setup.json --duration 120 > realtime.jsonl

ngo-stec-realtime -p ubx --input /data/recording.ubx.xz \
  --setup /data/station/setup.json --output replay.jsonl
```

Use exactly one of `--host` and `--input`. UBX/SBF, including XZ file replay,
enter the same CommonNEX engine. TCP delivery defaults to five-second batches;
JSONL still has one line per epoch, not one line per batch. Input antenna is 0.
`--output` exclusively creates a new file; otherwise JSONL goes to stdout.
Progress, discontinuity and incomplete-tail notices go to stderr. A TCP error
is not silently reconnected. `--duration` applies only to TCP.

Pipe stdout into [`ngo-stec-realtime-view`](stec-realtime-view.md) for a PySide6
map and time-series display of the latest hour.

## Output interpretation

Each line includes:

- `setup_id`, `segment`, and `gpst`: GPST seconds since 1980-01-06 as a decimal
  string. Native numerical processing rounds CommonNEX picoseconds to integer
  nanoseconds, ties to even, like the existing STEC reader.
- `orbit="broadcast"`, `phase="receiver_exported"` and
  `ipp_shell_radius_m=6821000`: explicit geometry/phase conventions.
- `samples`: satellite identity, exact `signals` pair, `arc_id`,
  `relative_stec_tecu`, `ipp_latitude_deg`, `ipp_longitude_deg`, `elevation_deg`
  and `azimuth_deg`. Coordinates are geocentric latitude and east-positive
  longitude on the declared spherical shell, not satellite subpoints.

There can be multiple samples for a satellite (for example L1/L2 and L1/L5).
They are independent pairs, not an averaged or calibrated satellite TEC value.
Relative STEC is the change in geometry-free carrier phase from the first usable
observation of its continuous arc, divided by the pair's meters-per-TECU factor.
It can be negative. Code/phase bias calibration, antenna phase correction and
phase wind-up correction are not applied. Interpret it as an arc-relative
receiver-phase ionospheric observable, not absolute STEC/VTEC.

Without usable ephemerides, that satellite produces no samples. `samples=[]`
is valid during cold start or when no supported visible pair is available.
The processor still tracks phase continuity while geometry is missing; the
first visible value need not be zero. It does not backfill earlier samples.
The elevation mask defaults to 10 degrees. A missing satellite alone does not
reset its arc; lock, half-cycle, GF-jump, signal-switch, restart and gap rules
come from the shared STEC engine. Raw measurement-clock telemetry is not yet
joined by this example; it consumes Observation tracking and restart Events.
Untimed restart Events cannot be safely placed and fail explicitly.

## Navigation support and time

The backend accepts these canonical RawBits families, not receiver envelopes:

| System | Family | Required ephemeris units |
| --- | --- | --- |
| GPS/QZSS | GPS_LNAV / QZS_LNAV | Subframes 1-3 |
| Galileo | GAL_INAV | Nominal even/odd pairs for word types 1-5 |
| Galileo | GAL_FNAV | Pages 1-4 |
| BeiDou | BDS_D1 | Subframes 1-3 |
| BeiDou GEO | BDS_D2 | Subframe 1 pages 1 and 3-10; page 2 is not required |

At least one recorded check must pass and none may fail. Required units must
be available within a 120-second context window, with source decoder checks on
subframe/page identity, IOD and, for BeiDou, SOW and TOE/TOC consistency. Galileo
satellite identity is checked against the decoded message. I/NAV alert pages
remain unsupported. Families have separate assembly/cache histories; I/NAV and
F/NAV never supply each other's missing pieces.

The latest contributing navigation-context time is the complete ephemeris's
availability time. Untimed RawBits do not update the cache. Finite/truncated week
numbers are resolved against receiver GPST, not the host date. Galileo uses
RTKLIB's nominal GST/GPST alignment for orbital geometry; no GGTO precision-clock
correction is claimed. BeiDou model times use BDT with the 14-second GPST offset.
TOE/TOC week carry is resolved around decoded transmission time. Broadcast GEO
propagation uses the BeiDou GEO transform rather than a MEO orbit formula.

Queries select only ephemerides whose availability is strictly earlier than
the observation time. Same-time pages may arrive after an observation within
the navigation window, so a newly completed ephemeris does not retroactively
enable that epoch depending on delivery chunk size. Selection is additionally
limited to the broadcast fit half-window capped at two hours. The latest applicable
health report in each available family is respected; an unhealthy update does
not fall back to an older healthy record or another family's healthy orbit.
This is conservative satellite-level filtering, not a claim that every signal's
health was reported by every family. Satellite positions include transmission-time iteration and
Earth rotation for the line of sight used by IPP calculation.

This is causality at navigation-context resolution, not exact wire-arrival
ordering. At higher measurement rates or with stale navigation context, it is
still not an exact wire-arrival timestamp guarantee. Each satellite/family retains
at most eight ephemeris entries, so feed bounded chronological batches instead
of an entire archive of RawBits before processing observations. RawBits and
Observation catalogs are independent; they need not contain matching timestamps.
Modern CNAV/CNAV-2/B-CNAV orbit decoding remains unimplemented. A supported
legacy orbit can provide geometry for the same satellite's modern observation
pairs; it does not provide their code-bias calibration. Existing RawBits
preservation for unsupported navigation families is unchanged.

## Reusable Python API

```python
from neognss_observatory.stec_realtime import RealtimeStec

processor = RealtimeStec(setup)
result = processor.process(
    raw_bits=raw_bits_batch,
    observations=observation_batch,
    events=events_batch,
)
samples = result.samples             # PyArrow RecordBatch
epochs = result.epochs_ns            # includes epochs with no emitted samples
```

Each input accepts a RecordBatch, Table or sequence of RecordBatches. Supply
observation-completion Events with their observations so the last epoch can be
released without waiting for the next epoch. The API does not mutate inputs;
other consumers can reuse the same CommonNEX batches. It is single-owner state.
`processor.feed(group)` accepts an existing `CnexBatchGroup`, handles preceding
records before a transport-discontinuity notice, clears state afterward and
increments the output segment. Arc IDs are local to a processor segment.

The underlying `_native.BroadcastNavigation(setup_id)` accepts RawBits Arrow
batches through `feed()`. It can be passed as `navigation=` to multiple
`RealtimeStec` consumers. Its `ecef(times_ns, systems, numbers)` batch query
returns an N-by-3 NumPy array of satellite ECEF meters at the requested GPST,
with NaN rows when unavailable. This direct query has no receiver-specific
light-time/Earth-rotation transformation. RTKLIB types remain private.
Shared navigation contexts must be fed once by their owner, and discontinuity
handling must be coordinated across consumers; they are not an IPC service.
`decoded_by_family` reports inserted ephemeris updates by navigation family;
repeated unchanged ephemerides are not counted again.

Only the final example CLI converts numerical batches into JSONL. Numerical
work and navigation decoding remain in native code with the GIL released.
Broadcast ephemerides are processing state, not a new CommonNEX catalog.
