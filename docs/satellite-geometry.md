# Satellite orbit and clock evaluation

Design draft for a project-owned satellite geometry layer in `libneognss-obs`.
**Nothing in this document is implemented yet.** Offline and realtime
[STEC](stec.md) still use RTKLIB-EX propagation and product readers; their
documents describe current behavior until each migration lands. Implementation
order and validation tasks are tracked in the [roadmap](TODO.md#project-owned-satellite-orbit-and-clock-evaluation).

## Scope

The layer evaluates satellite position, velocity and clock for processors that
need geometry outside a positioning solver: offline and realtime STEC, broadcast
navigation adapters and later research consumers. A positioning backend keeps
its own satellite models so orbit, clock, attitude and antenna corrections stay
consistent within one solution; [PPP](ppp.md) therefore keeps RTKLIB-EX.

Supported systems are GPS, Galileo, BeiDou and QZSS. GLONASS and NavIC are out
of scope. SBAS GEO ephemerides are not part of this layer.

Out of scope for the evaluator itself:

- Signal group delays (TGD, BGD, ISC). They are signal-specific and remain with
  the caller, using the decoder's typed fields.
- Conversion between broadcast antenna phase centre and centre of mass, which
  needs satellite ANTEX offsets and attitude.
- Reference-frame transformation between WGS84, GTRF and CGCS2000.
- Time-scale offsets between GPST, GST and BDT (GGTO/BGTO). Clocks are reported
  against each system's own time scale; see [outputs](#satellite-state).

## Layers

1. **Orbit models** are stateless functions from typed parameters and a time to
   a satellite state. They perform no selection or health interpretation.
   - Legacy Keplerian: GPS/QZSS LNAV, Galileo I/NAV and F/NAV, BeiDou D1/D2,
     including the BeiDou GEO inclined-frame rotation.
   - Modern Keplerian with a time-varying semi-major axis and mean-motion rate:
     GPS/QZSS CNAV and BeiDou B-CNAV1/2/3.
2. **Broadcast ephemeris store** consumes ephemeris occurrences emitted by the
   [broadcast message decoder](broadcast-message-decoder.md). It selects one
   parameter set per query and applies family-specific applicability and health.
3. **Precise product store** reads SP3 orbits and RINEX clock products and
   interpolates them.

A light-time helper iterates transmission time against any source in layers 2
or 3. Layers 2 and 3 implement one batch evaluation interface.

## Time and arithmetic

Queries use exact GPST picosecond ticks, the native representation described in
the [time policy](time-policy.md) and [native architecture](native-architecture.md#native-time).
Differences such as `t - toe` are formed exactly in ticks before conversion to
binary64 seconds. Decoder reference times are already placed on the GPST axis:
BeiDou `toe_gpst` is BDT plus 14 s and Galileo GST uses its nominal alignment
with GPST, so the exact difference equals the difference in the system's own
time scale. The nanosecond-level GGTO residual matters for clocks, not orbits.

## Orbit models

Each system uses its own ICD constants: gravitational parameter, Earth rotation
rate and the relativistic constant `F = -2 sqrt(mu) / c^2`. There is no shared
default. GPS and QZSS use the IS-GPS-200 values; Galileo uses the Galileo OS SIS
ICD values; BeiDou uses CGCS2000 values. Modern-model reference semi-major axes
and node-rate references are family constants already listed in the
[GPS and QZSS CNAV](broadcast-cnav.md) and [BeiDou](broadcast-beidou.md#orbit-and-clock)
contracts. Each constant is cited at
its definition in code.

The clock polynomial is `af0 + af1 dt + af2 dt^2` from `toc`. The periodic
relativistic term `F e sqrt(A) sin(E)` is reported separately rather than folded
into the clock.

## Broadcast ephemeris store

The store consumes decoder ephemeris records (`ephemeris` for LNAV,
`cnav_ephemeris`, `gal_inav_ephemeris`, `gal_fnav_ephemeris`,
`bds_d1_ephemeris`, `bds_d2_ephemeris` and B-CNAV ephemerides). It never
decodes navigation bits itself. It replaces `BroadcastNavigation`, which also
decodes LNAV through RTKLIB-EX and keeps separate Galileo and BeiDou decoder
instances; realtime STEC migrates to decoder, store and evaluator together.

A query carries an evaluation time and an availability cutoff, normally the
reception time. Selection considers candidates received strictly before the
cutoff, keeps those applicable at the evaluation time and chooses the latest
received. Applicability rules are shared with decoder snapshots rather than
reimplemented: GPS fit interval and IODC, GPS and QZSS CNAV windows, Galileo age windows
and the equivalent BeiDou rules. Health follows each family's own fields:
Galileo I/NAV and F/NAV signal health are evaluated separately and one family's
report does not mask the other's; BeiDou uses SatH1 or the B-CNAV health field;
GPS and QZSS use the LNAV or CNAV health fields. A missing health interpretation
is `HEALTH_UNKNOWN`, not healthy.

Retention is bounded per satellite and family. A discontinuity does not discard
completed candidates; they remain subject to applicability.

## Precise product store

SP3-c/d orbits and RINEX 3 clock files are read for the requested files only.
Adjacent daily files are joined; at an equal boundary epoch, values reported in
the later file replace individually reported satellites, matching the current
clock merge.

- Orbits use Lagrange interpolation over 11 consecutive nodes centred on the
  query, the current STEC stencil. Nodes must be consecutive product epochs; a
  missing epoch inside the stencil is `PRODUCT_GAP`. There is no extrapolation
  beyond the first or last node (`PRODUCT_EDGE`). SP3 bad-position and clock
  sentinels are `MISSING_VALUE`; maneuver flags are `PRODUCT_FLAGGED`.
- Clocks interpolate linearly between two bracketing samples whose spacing does
  not exceed a caller limit (default 60 s, matching current STEC and PPP). An
  exact epoch uses the sample. A missing bracketing sample is `PRODUCT_GAP`.
- SP3 clocks never replace a missing clock-file value unless the caller enables
  that explicitly; the reported clock source identifies which was used.

Precise clocks exclude the periodic relativistic term by IGS convention; the
store reports `-2 r.v / c^2` separately, consistent with broadcast output.

Consumers decide which products they require. Offline STEC will require SP3
only: clock values do not enter the geometry-free combination, and SP3 clock
sentinels and maneuver flags replace the current CLK availability screen.
Its satellite health will come from the station's own decoded RawBits instead
of RINEX navigation files.

## Satellite state

| Field | Meaning |
| --- | --- |
| `position_m`, `velocity_m_s` | ECEF at the evaluation time in the system's own frame |
| `clock_s` | Satellite clock offset against GPST (GPS/QZSS), GST (Galileo) or BDT (BeiDou), excluding the relativistic term |
| `relativistic_s` | Periodic relativistic clock correction |
| `reference_point` | `BROADCAST` (the family's ICD orbit reference) or `CENTER_OF_MASS` (SP3) |
| `clock_reference` | Signal combination defining the clock, for example GPS L1/L2 P(Y), Galileo E1/E5b (I/NAV) or E1/E5a (F/NAV), BeiDou B3I |
| `clock_source` | `BROADCAST`, `CLOCK_PRODUCT`, `SP3` or `NONE` |
| `status` | See below; other fields are undefined unless `AVAILABLE` |
| Source identity | Family, issue of data and `toe` for broadcast; product epoch span for precise |

Status values are `AVAILABLE`, `NO_CANDIDATE`, `OUTSIDE_APPLICABILITY`,
`UNHEALTHY`, `HEALTH_UNKNOWN`, `PRODUCT_GAP`, `PRODUCT_EDGE`, `MISSING_VALUE`,
`PRODUCT_FLAGGED` and `UNSUPPORTED`. A position can be `AVAILABLE` while
`clock_source` is `NONE`; consumers needing clocks check both.

The broadcast reference point of each family is recorded with its ICD citation
in code. Comparisons between broadcast and precise orbits must convert reference
points explicitly; this layer does not do it.

## Light-time helper

Given a receiver ECEF position, reception time and orbit source, the helper
iterates transmission time from the geometric range, evaluates the source at the
transmission time with the reception time as availability cutoff, and rotates
the transmit-time ECEF position into the reception-time frame using the
IS-GPS-200 Earth rotation rate. It returns transmission time, rotated position,
line-of-sight unit vector and range, plus the source status. Iteration stops on
a sub-picosecond change or a fixed iteration bound. Receiver clock offset and
atmospheric delays are not applied. Offline and realtime STEC share this helper.

## Interfaces

Native consumers call a span-based batch interface without Arrow conversion.
Sources are owned by one processing thread; concurrent use requires external
synchronization. The layer does not use the process-wide RTKLIB lock.

Python receives Arrow batches. A query batch contains `satellite_system`,
`satellite_number` and `gpst` as `decimal128(38,12)`, plus an optional
availability cutoff; the result batch contains the state fields above with
string enums. Native evaluation releases the GIL; there are no per-epoch
callbacks.
