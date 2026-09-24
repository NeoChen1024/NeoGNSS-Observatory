# RTCM3 observation import

The RTCM3 adapter produces the existing CommonNEX Observation and measurement
completion Events through the same native engine as UBX/SBF. Python can consume
the Arrow batches directly or persist them with the [batch importer](importer.md).
It adds no Core fields and no receiver telemetry. Parsed ephemerides, station
descriptors, corrections and proprietary messages are skipped, not converted to
DecodedNav or reconstructed RawBits. Keep original recordings for unsupported
content.

## Coverage

| Input | Result |
| --- | --- |
| GPS 1074-1077 | MSM4/5/6/7 observations |
| Galileo 1094-1097 | MSM4/5/6/7 observations |
| SBAS 1104-1107 | MSM4/5/6/7 observations |
| QZSS 1114-1117 | MSM4/5/6/7 observations |
| BeiDou 1124-1127 | MSM4/5/6/7 observations |
| GLONASS / NavIC | Intentional scope exclusions |
| Legacy observations and MSM1/2/3 | Unsupported; counted and skipped |
| Other RTCM3 message types | Counted and skipped without payload decoding |

Framing checks the ten-bit payload length and CRC24Q. Reserved header bits are
ignored and valid zero-length filler frames produce no scientific records.
Malformed selected observation payloads fail explicitly. The MSM satellite
and signal mask product must not exceed the standard's 64-cell limit; unexpected
trailing payload extensions are ignored. Complete valid UBX/SBF
frames in a mixed recording are skipped atomically. Physical file and feed-chunk
boundaries do not finish frames or measurement groups.

Supported signal IDs map to RINEX satellite identities and two-character signal
codes. Unknown or reserved signal mappings are counted, never assigned a guessed
frequency. Code and phase ranges are reconstructed in meters; carrier phase is
converted to cycles using the signal frequency. MSM5/7 range rate maps to Doppler
with the opposite sign, in Hz. MSM4/6 do not supply Doppler. C/N0 is dB-Hz.
Unavailable wire values become null independently; zero is not a universal
missing-value marker. Quantization is not stored as a measurement uncertainty.

Lock indicators map to the existing interval/lower-bound duration representation,
and half-cycle ambiguity remains a source-reported flag. Loss of lock and
half-cycle subtraction are not inferred. The source smoothing declaration maps
to `receiver_corrections.code_smoothing_applied`; observations are not adjusted.

## Explicit time and station context

RTCM3 MSM contains milliseconds within a week, not an absolute week or date.
Supply `--rtcm-reference-gpst` as an integer number of GPST seconds since
1980-01-06 00:00:00. This is not a Unix timestamp, UTC date or host arrival time.
The first observation must be less than half a week from that reference.
Subsequent epochs resolve to the nearest week relative to the previous resolved
epoch. Continuous week rollover is supported; gaps of half a week or longer
are ambiguous and require a new explicitly anchored import. Exactly half-week
ties are rejected. A plausible but incorrect supplied reference cannot be
detected from MSM alone.

GPS, Galileo, QZSS and SBAS MSM week epochs align with GPST for this mapping.
BeiDou MSM declares BDT: add exactly 14 seconds before resolving the GPST week,
including rollover. No leap-second guess or fine broadcast inter-system
correction is applied. Integer milliseconds become exact `DECIMAL(38,12)` GPST
seconds. No observation timestamp is obtained from ephemerides or filenames.

One import represents one reference station. `--rtcm-station-id` selects its
12-bit RTCM station ID; other stations are skipped and counted. Without a
selector the engine binds the first valid MSM station header and rejects a change.
This ID is decoder context, not a new CommonNEX Core field. Setup metadata is
still supplied explicitly at initialization. `--source-antenna` must be zero.

RTCM3 files are read in the supplied chronological order, without independent
head sorting. Weekless head timestamps cannot safely order multiweek recordings.
Recursive discovery selects `*.rtcm3` and `*.rtcm3.xz` in deterministic directory
and filename order; use explicit file arguments when that is not chronological.
Resume requires the same reference and station selector and restores the
resolved week and station context from the saved continuation state.

## Completion and use

An MSM sequence stays pending while the multiple-message flag is set. Completion
requires a compatible closing message for the same station and measurement
epoch, including contributions from other supported constellations. Pending
groups are bounded and an unfinished tail is withheld for raw-context replay.
Intentionally excluded GLONASS/NavIC contributions are skipped. Their common
header can close an already timed supported group with matching station and
issue-of-data, using the existing GPST unchanged. DF393 declares the physical
epoch boundary across constellations; this does not require GLONASS/NavIC time
conversion or observation decoding. After a detected corrupt frame the engine
resynchronizes at the next selected MSM closing marker, withholding that
sequence. Unsupported in-scope MSM1/2/3 contributions likewise cause their
sequence to be omitted and counted rather than labeled complete. Valid legacy
RTK messages are counted and skipped independently; their separate service
does not complete or invalidate MSM groups.
Completion means the received sequence is complete, not that every configured
satellite or signal was observed.

For example, GPST week 2400 plus 100000 seconds is 1451620000 GPST seconds;
replace this example with independently known acquisition time:

```sh
ngo-cnex-import run -p rtcm3 --station data/my-setup \
  --rtcm-reference-gpst 1451620000 --rtcm-station-id 42 \
  first.rtcm3 second.rtcm3
```

The same context is available without Parquet:

```python
from neognss_observatory.cnex_stream import CnexStream

stream = CnexStream(
    "rtcm3", "my-setup", period_seconds=1,
    rtcm_reference_gpst_s=1451620000, rtcm_station_id=42,
)
stream.feed(chunk)  # bytes, at most 64 KiB per live feed
group = stream.drain()
```

`ngo-cnex-live -p rtcm3` accepts the same reference/station options for TCP
acquisition. Delivery time never becomes measurement time. Explicit stream
discontinuities reset association; supply a fresh reference for a new session
if the original reference is no longer within half a week of its first epoch.
See [live delivery](live.md) for lifecycle and backpressure.

The MSM field and completion contracts are checked against
[BD 410003 A-2022](https://m.beidou.gov.cn/zt/bdbz/202407/W020240718511922437593.pdf),
especially sections 6.5.15.4.4-5 (trailing data and mask bounds), 6.5.15.4.8
(cross-constellation DF393 completion) and 7 (framing). The adapter reports
ignored non-observation, unsupported observation and intentionally excluded
observation message counts separately, alongside unknown signals, invalid
frames, other stations and incomplete epochs.
