# Receiver antenna phase calibration

PPP and STEC share a project-owned receiver antenna model. Python selects and
validates absolute ANTEX 1.4 records; native C++ evaluates PCO/PCV in batches.
The model is independent of RTKLIB's GPS-only receiver ANTEX frequency slots.
This does not enable multi-GNSS PPP/STEC by itself. Satellite antenna models,
receiver code biases and phase wind-up have separate ownership.

## Record and frequency selection

Select an exact antenna type/radome. A matching individual serial calibration
wins over a type mean; otherwise the first catalog's matching type mean wins.
All validity records come from that one catalog and selection kind. Overlapping
records are rejected. Missing dates are errors, not stale-calibration fallback.
Source ANTEX files are read-only; no synthetic calibration is written into them.

Each requested ANTEX frequency identifier is resolved in this order:

1. Original calibration for the requested system/frequency.
2. An original calibration at the identical carrier frequency.
3. Linear interpolation between the nearest lower and upper original
   frequencies, **both within 25 MHz of the target**.
4. Direct substitution of the closest original frequency within 25 MHz.
5. Error when none of these applies.

The 25 MHz limit is inclusive and is an engineering approximation policy, not
an accuracy bound. No frequency slope is extrapolated. Resolved models never
become sources for another resolution. Equal-frequency sources prefer the
requested system; otherwise conflicting patterns are an error, while identical
patterns use deterministic identifier order. No per-frequency catalog splicing
or implicit averaging is performed.

For GPS G01/G02-only calibration:

| Target | Result |
| --- | --- |
| Galileo E01 / BeiDou C01, 1575.42 MHz | Same-frequency G01 |
| Galileo E07 / BeiDou C07, 1207.14 MHz | G02 substitution, 20.46 MHz separation |
| GPS G05 / Galileo E05 / BeiDou C05, 1176.45 MHz | Unavailable; G02 is 51.15 MHz away |
| Galileo E06, 1278.75 MHz | Unavailable; cross-band G01/G02 interpolation is disallowed |

C07 identifies the carrier used by BeiDou B2I/B2b, not a code-bias identity.
Subsequent STEC processing must still select exact observation codes and verify
bias-product coverage. Supported model frequency identifiers cover GPS,
Galileo, BeiDou, QZSS and SBAS. GLONASS and NavIC scientific processing remain
out of scope; their catalog records are not frequency-substitution sources.

## Direction, units and correction

ANTEX millimeters become meters. Retain the original north/east/up PCO vector,
zenith grid, NOAZI pattern and, when present, azimuth-dependent grid. Interpolate
PCV over angle without extrapolating outside the calibrated zenith range.
PPP excludes out-of-grid observations and counts them in
`receiver_antenna_masked_observations`. STEC keeps raw continuity but marks the
corrected GF phase unavailable with issue bit 32 when geometry is known and
outside that range; it does not use those samples for leveling.
An explicit installation azimuth rotates geographic azimuth into antenna axes.
With unknown orientation, use NOAZI and assume the PCO axes align north/east;
this assumption does not establish that the physical antenna was oriented north.

For antenna-frame unit line of sight `u`, the modeled phase range error is
`d = -dot(PCO, u) + PCV`. Subtract it from carrier phase expressed in meters.
Frequency interpolation is a weighted sum of complete directional corrections;
each source retains its own angular grid. ARP displacement remains separate.
Do not interpret ANTEX phase calibration as a measured code/group-delay bias.

Results record `native`, `same-frequency`, `interpolated`, or
`near-frequency substitution`, original source identifiers/frequencies, signed
frequency differences and weights. They do not assign invented calibration
uncertainties. Configuration/continuation state retains the actual numerical
model; output metadata contains compact selection information.

## Current consumers

`ngo-ppp` requires receiver calibration from `antenna_catalogs`, optionally with
`antenna_serial_number` and `antenna_azimuth_deg`. Receiver phase PCO/PCV is
applied once before RTKLIB, using transmit-time orbit geometry, Earth rotation
and the prior static position (initially the configured marker position).
RTKLIB continues to apply marker-to-ARP displacement and its satellite model.
The receiver ANTEX phase pattern is no longer also applied to pseudorange;
code OSB correction remains separate. Prior-position geometry is not a new
iterated antenna model inside the filter. Calibration record changes flag phase
discontinuity without resetting the whole position filter.

`ngo-stec` defaults to `receiver_antenna = "required"`: load the selected
CommonNEX Setup companion `antenna.calibration_file`, using Setup identity and
orientation. An explicit `receiver_antenna = "none"` permits uncorrected research
processing and marks it in metadata. Missing or unsupported required calibration
never silently switches to this mode.

STEC corrects emitted GF phase before leveling; pseudorange is unchanged. Raw
GF still drives slip detection and is retained in continuity state. Missing
geometry makes the corrected phase unavailable without destroying raw phase
continuity. A calibration-record change closes the affected arc (reason 8), so
leveling samples from different calibration records are not mixed. Changed
calibration contents/configuration require `--rebuild` for existing STEC outputs;
existing datasets are not automatically rewritten. Satellite antenna phase
corrections and phase wind-up remain absent from STEC.
