# Realtime relative STEC

`ngo-stec-realtime` is a CommonNEX streaming consumer example. It emits one JSONL
record per completed observational epoch, with GPS/QZSS signal-pair relative
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

## Output interpretation

Each line includes:

- `setup_id`, `segment`, and `gpst`: GPST seconds since 1980-01-06 as a decimal
  string. Native numerical processing rounds CommonNEX picoseconds to integer
  nanoseconds, ties to even, like the existing STEC reader.
- `orbit="broadcast_lnav"`, `phase="receiver_exported"` and
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

The initial backend accepts canonical `GPS_LNAV` and `QZS_LNAV` RawBits, not
receiver envelopes. At least one recorded check must pass and none may fail.
Subframes 1-3 must be available within a 120-second context window and pass
RTKLIB's subframe/IOD consistency checks. Their latest navigation-context time
is the complete ephemeris's availability time. Untimed RawBits do not update
the navigation cache. Truncated week numbers are resolved against receiver GPST,
not the host date; TOE/TOC remain the decoded model reference times.

Queries select only ephemerides already available at the observation time,
within the broadcast fit half-window capped at two hours. The latest applicable
health report is respected; an unhealthy update does not fall back to an older
healthy record. Satellite positions include transmission-time iteration and
Earth rotation for the line of sight used by IPP calculation.

This is causality at navigation-context resolution, not exact wire-arrival
ordering. Same-context records may be consumed together. Each satellite retains
at most eight ephemeris entries, so feed bounded chronological batches instead
of an entire archive of RawBits before processing observations. RawBits and
Observation catalogs are independent; they need not contain matching timestamps.
Galileo, BeiDou and modern CNAV orbit decoding are not implemented in this first
backend. Existing RawBits preservation for those families is unchanged.

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

Only the final example CLI converts numerical batches into JSONL. Numerical
work and navigation decoding remain in native code with the GIL released.
Broadcast ephemerides are processing state, not a new CommonNEX catalog.
