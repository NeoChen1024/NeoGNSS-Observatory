# CommonNEX minimal receiver message profiles

Status: v0 design draft; not an implemented format or API.

[Overview](overview.md)

## Purpose

Profiles define the message configuration required for the current station
design and the corresponding input-adapter contract.

Requirements are capability-specific: Observation, RawNav and auxiliary records.
Observation and RawNav are core families, not mutually mandatory outputs.
Missing auxiliary output does not make either capability invalid.
Missing required time/boundary information must produce an explicit error or
incomplete status under the adapter contract, not guessed completion.

| Input | Observation target | RawNav target | Optional auxiliary / navigation |
| --- | --- | --- | --- |
| UBX | RXM-RAWX, NAV-TIMEGPS, NAV-EOE | RXM-SFRBX | NAV-CLOCK, NAV-PVT, TIM-TP, MON-SYS |
| SBF | Measurements: MeasEpoch, MeasExtra, EndOfMeas | RawNavBits group | Clock/pulse/environment/status blocks; decoded navigation if wanted |
| RTCM3 | MSM7 for each enabled in-scope constellation, resolvable full time context | Not supplied by ordinary decoded ephemeris messages | Applicable broadcast ephemerides for DecodedNav; station descriptors |
| RINEX | Supported observation records and interpretation metadata | Not reconstructed from decoded NAV | Supported NAV records for DecodedNav |

This is the agreed configuration direction, not a claim that complete
CommonNEX adapters are implemented. Expand configuration groups into exact
block/revision coverage; a group is not a single wire message.

The initial SBF observation importer targets MeasEpoch (4027), MeasExtra (4000)
and EndOfMeas (5922). This Measurements-based profile is preferred over Meas3
for simpler decoding and retention of its measurement/quality fields, accepting
the larger logging volume. Match MeasExtra to the appropriate epoch, antenna,
satellite and signal; it is not an independent observation epoch. Preserve state
and pending companion blocks across file and day boundaries.

Meas3 is not required for the initial importer and is a deferred capability,
not an automatic alternate backend. When both representations are recorded,
use Measurements without emitting duplicate observations from Meas3. A
Meas3-only source must explicitly report unsupported observation input until
that decoder is implemented; its supported RawNav families remain independently
importable. No receiver reconfiguration or archive rewriting is implied.

RINEX is an input adapter, not a receiver message profile. Enumerate supported
versions and observation/navigation variants instead of inventing receiver
configuration requirements.

The Observation column applies only when that capability is requested. A UBX
source exporting RXM-SFRBX but no RXM-RAWX is a supported design use case for
RawNav-only import. Validate actual model/firmware output and time anchors;
do not require RAWX to associate navigation epochs. TIMEGPS/EOE availability
and other justified navigation-time mappings are adapter-specific, not assumed
for every receiver. If time cannot be resolved after bounded association
attempts, skip and report those records; do not persist null nav_epoch_id.
RawNav-only means no observations, not no usable navigation time.
The standard message spelling is RXM-SFRBX.

## Time and completion contracts

UBX observation epochs use RAWX measurement time. TIMEGPS supplies navigation
context and is not allowed to overwrite it. EOE closes its associated navigation
epoch; the adapter must specify the relationship to RAWX completion explicitly.

SBF measurement epochs and companion-block completion require an explicit
MeasEpoch/MeasExtra/EndOfMeas association rule. RawNav content is assigned to NavigationEpoch,
not given an additional transmission-time column in the RawNav family.

RTCM3 mapping must resolve the full epoch/date/time-scale context; a partial
time-of-week alone is not a complete GPST timestamp. The adapter must validate
MSM multiple-message completion across the relevant station/message sequence,
not close an epoch merely because one selected constellation was processed.
Exact message IDs, time anchoring and sequence rules remain mapping review
items; Core does not invent them.

RINEX epochs use their declared time system and record structure. Decode header
scale factors, phase shifts, clock application and applicable event metadata.
Do not equate an unknown signal-strength unit with C/N0.

Every adapter documents required messages, exact source revisions, usable
time, completion, missing-companion behavior and supported mappings. Emit no
fabricated epoch to satisfy a profile. An incomplete tail remains incomplete.
An explicit request for unsupported protocol/capability fails clearly.

## Configuration versus storage

Enabling a profile selects what the receiver supplies; core membership does not
make every family mandatory in every source. An Observatory recording configuration may
require RawNav while an observation-only CommonNEX consumer ignores it.
Decode supported enabled families in the same pass, independently of whether a
PPP, SBAS or clock-analysis facility is currently running.
