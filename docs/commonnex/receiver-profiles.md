# CommonNEX minimal receiver message profiles

Status: v0 design draft; not an implemented format or API.

[Overview](overview.md)

## Purpose

Profiles define the message configuration required for the current station
design and the corresponding input-adapter contract.

Requirements are capability-specific: Core observations, optional RawNav and
optional auxiliary records. Missing auxiliary output does not make Core invalid.
Missing required time/boundary information must produce an explicit error or
incomplete status under the adapter contract, not guessed completion.

| Input | Core target | RawNav target | Optional auxiliary / navigation |
| --- | --- | --- | --- |
| UBX | RXM-RAWX, NAV-TIMEGPS, NAV-EOE | RXM-SFRBX | NAV-CLOCK, NAV-PVT, TIM-TP, MON-SYS |
| SBF | Meas3 measurement output, EndOfMeas | RawNavBits group | Clock/pulse/environment/status blocks; decoded navigation if wanted |
| RTCM3 | MSM7 for each enabled in-scope constellation, resolvable full time context | Not supplied by ordinary decoded ephemeris messages | Applicable broadcast ephemerides for DecodedNav; station descriptors |
| RINEX | Supported observation records and interpretation metadata | Not reconstructed from decoded NAV | Supported NAV records for DecodedNav |

This is the agreed configuration direction, not a claim that Meas3/RTCM3
adapters are implemented. Meas3 and RawNavBits configuration groups must be
expanded into exact block/revision coverage before implementation. Do not
describe groups as if each were a single wire message.

RINEX is an input adapter, not a receiver message profile. Enumerate supported
versions and observation/navigation variants instead of inventing receiver
configuration requirements.

## Time and completion contracts

UBX observation epochs use RAWX measurement time. TIMEGPS supplies navigation
context and is not allowed to overwrite it. EOE closes its associated navigation
epoch; the adapter must specify the relationship to RAWX completion explicitly.

SBF measurement epochs and companion-block completion require an explicit
Meas3/EndOfMeas association rule. RawNav content is assigned to NavigationEpoch,
not given an additional transmission-time column in the extension.

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

Enabling a profile selects what the receiver supplies; it does not make optional
families mandatory Core fields. An Observatory recording configuration may
require RawNav while an observation-only CommonNEX consumer ignores it.
Decode supported enabled families in the same pass, independently of whether a
PPP, SBAS or clock-analysis facility is currently running.
