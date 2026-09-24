# Single GPST processing policy

All project-owned observation timelines, partitions, query windows, plots and
derived metadata use GPST. There is no selectable UTC processing mode.

## Representation

### CommonNEX input boundary

CommonNEX uses `DECIMAL(38,12)` GPST seconds. The implementations below do not
infer the source time scale from the constellation of each observation:

| Source | Time mapping |
| --- | --- |
| UBX RAWX | Measurement week plus receiver-exported GPS-aligned TOW; exact binary64-to-picosecond rounding before adding week |
| SBF MeasEpoch | GPS-aligned WNc plus integer millisecond TOW, converted exactly |
| UBX RawBits anchor | Valid NAV-TIMEGPS week/iTOW/fTOW, preserving the full reported precision; no requirement that SFRBX precede EOE |
| SBF RawBits anchor | Valid synchronous navigation block WNc/TOW; never RawNavBits SIS timestamp |
| RTCM3 MSM4/5/6/7 | Explicit GPST reference resolves weekless integer-millisecond epochs; BeiDou BDT adds 14 seconds; see [RTCM3](commonnex/rtcm3.md) |
| Future RINEX adapter | Resolve declared scale, full date/week and any required leap-second context explicitly; not implemented by CommonNEX import yet |

The GPS-only RINEX processing restriction below describes the existing solver
input paths, not a finished general CommonNEX RINEX time-conversion adapter.
Satellite identity does not change the receiver observation epoch's scale:
a BeiDou row in RAWX or MeasEpoch must not receive a second BDT-to-GPST offset.
No fine broadcast inter-system correction is applied to these observation times.
Timescale normalization does not remove receiver clock error. CommonNEX excludes
inputs with applied observation clock-offset correction and never undoes one;
internal receiver clock jumps remain native evidence, not a reason to reject RAWX.

RawBits and telemetry use the [receiver-time policy](commonnex/telemetry-time.md).
Unknown GPST remains null. Untimed rows retain the last known archive day, or
1980/01/06 before any known date; directory placement is not an assigned timestamp.
Measurement time can advance its timeout high-water mark but cannot become a
RawBits timestamp. Arrival ordering, navigation association and measurement time
are distinct. No live wall clock or offline processing speed changes that policy.

### Existing processing representations

`gpst`, `start_gpst`, `end_gpst` and `hour_gpst` denote continuous seconds since
1980-01-06 00:00:00 GPST. These are **not Unix timestamps**. Archive indexes use
integer `gpst_ms` milliseconds. `start_gpst_ms` / `end_gpst_ms` are exact
artifact bounds, while unsuffixed fields remain seconds. Receiver-clock
products use integer `gpst_ns`; NAV-CLOCK retains its millisecond iTOW resolution.
Raw measurement timestamps retain their original fractional precision.

GPST calendar rendering uses calendar arithmetic from that epoch, with no
timezone conversion. Assigned UBX files use
`GPST-%Y-%m-%d--%H-%M-%S-mmm.ubx`, with exactly three millisecond digits;
PNG labels/names also explicitly include GPST.
No GPST label ends in `Z`, `%z`, or `+0000`.

Days and hours are half-open GPST intervals: 86,400 and 3,600 seconds.
Continuous observation arcs survive file/day/hour boundaries; IPP display
values alone are rebased to each arc's first valid sample in the GPST hour.

## Input boundaries

- UBX archive timing is anchored by valid NAV-TIMEGPS or RAWX GPS week/TOW,
  never by NAV-PVT's UTC calendar. NAV/EOE defines millisecond navigation
  epochs without snapping to seconds. RAWX's steered measurement time is
  associated by the epoch's stream framing, not exact timestamp equality;
  week carry is handled explicitly and original bytes are retained. Empty
  RAWX reports cannot anchor or split epochs.
  Reconstruction splits at GPST midnight or a NAV-to-NAV interval exceeding
  50 seconds by default (`--gap-timeout`). This is not a sampling-rate or
  gapless-observation claim; downstream scientific QC remains independent.
- The logger requires valid week/TOW in NAV-TIMEGPS in every EOE interval and
  matching EOE iTOW. It buffers frames and routes the complete epoch at EOE.
  The first new-day epoch belongs entirely to the new day. Missing EOE at EOF,
  missing/invalid TIMEGPS, conflicting timestamps or non-increasing epochs
  fail explicitly. Subsecond fTOW is preserved, not used to shift nominal
  logger epochs to a neighboring second.
- RINEX observations must declare GPS time for project processing. Navigation
  records keep their standards-defined, constellation-specific native fields;
  the decoder converts them internally. Do not rewrite NAV fields as if they
  all had the same native time scale.
- SBF is immutable input. Receiver-time offsets, validity and GNSS timing
  system settings still matter; GPST does not make receiver clocks perfect.
- External product timestamps, download protocols and source metadata retain
  their specified semantics. Standard-format UTC fields are not relabeled.
