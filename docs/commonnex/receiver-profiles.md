# CommonNEX receiver message profiles

[Overview](overview.md) | [Receiver mappings](receiver-mappings.md)

Profiles describe receiver output configuration, not mandatory co-presence of
all catalogs. Observation-only and RawBits-only stations are valid. UBX and SBF
are the supported acquisition protocols; RINEX/RTCM3 import is out of scope.

| Capability | UBX messages | SBF messages |
| --- | --- | --- |
| Observation | RXM-RAWX | MeasEpoch and matching EndOfMeas; MeasExtra recommended |
| RawBits | RXM-SFRBX | Supported RawNavBits blocks |
| Navigation time / completion | NAV-TIMEGPS; NAV-EOE for completion | PVTCartesian, PVTGeodetic, ReceiverTime or EndOfPVT for supported navigation anchors |
| Receiver telemetry windows | NAV-PVT | PVTCartesian or PVTGeodetic |
| Clock estimates | NAV-CLOCK | PVT clock estimates |
| Uptime, temperature, CPU | MON-SYS | ReceiverStatus |
| Pulse timing | TIM-TP | xPPSOffset |

RAWX completes its own measurement record and does not require EOE. MeasEpoch
requires matching EndOfMeas. MeasExtra absence leaves base observations usable.
TIMEGPS does not replace RAWX time. RawBits can be retained with null time when
navigation context is unavailable; a time anchor is not required for preservation.
PVT triggers the delayed telemetry window; status/pulse messages alone do not
provide the complete normal telemetry assembly profile.

Measurements is preferred over Meas3 for supported observation import. When
both are logged, use Measurements without duplicating observations. Meas3-only
input has unsupported observations, but supported RawBits remain importable.
Group names are configuration conveniences, not individual wire messages.
See [mapping coverage](receiver-mappings.md) for exact blocks and revisions.

Configured tracking and message selection do not establish actual coverage.
Different recording interfaces may omit messages; missing optional telemetry
does not invalidate observations or RawBits. Decode available supported families
independently of which downstream processor will run.
