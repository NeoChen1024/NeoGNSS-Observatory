# CommonNEX minimal receiver message profiles

Status: v0 design draft with an implemented UBX/SBF importer subset.

[Overview](overview.md)

## Purpose

Profiles define the message configuration required for the current station
design and the corresponding input-adapter contract.

Requirements are capability-specific: Observation, RawBits and auxiliary records.
Observation and RawBits are core families, not mutually mandatory outputs.
Missing auxiliary output does not make either capability invalid.
Missing required time/boundary information must produce an explicit error or
incomplete status under the adapter contract, not guessed completion.

| Input | Observation target | RawBits target | Optional auxiliary / navigation |
| --- | --- | --- | --- |
| UBX | RXM-RAWX, NAV-TIMEGPS, NAV-EOE | RXM-SFRBX | NAV-CLOCK, NAV-PVT, TIM-TP, MON-SYS |
| SBF | Measurements: MeasEpoch, MeasExtra, EndOfMeas | RawNavBits group plus synchronous receiver navigation time | Clock/pulse/environment/status blocks; decoded navigation if wanted |
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
that decoder is implemented; its supported RawBits families remain independently
importable. No receiver reconfiguration or archive rewriting is implied.

RINEX is an input adapter, not a receiver message profile. Only standard RINEX
3.x and 4.x in-scope content is targeted; RINEX 2 is excluded. Enumerate supported
versions and observation/navigation variants instead of inventing receiver
configuration requirements.

The Observation column applies only when that capability is requested. A UBX
source exporting RXM-SFRBX but no RXM-RAWX is a supported design use case for
RawBits-only import. Validate actual model/firmware output and time anchors;
do not require RAWX to associate navigation epochs. TIMEGPS/EOE availability
and other justified navigation-time mappings are adapter-specific, not assumed
for every receiver. If time cannot be resolved after bounded association
attempts, skip and report those records; do not persist null `nav_epoch_gpst`.
RawBits-only means no observations, not no usable navigation time.
The standard message spelling is RXM-SFRBX.

## Time and completion contracts

UBX observation epochs use RAWX measurement time. TIMEGPS supplies navigation
context and is not allowed to overwrite it. EOE closes its associated navigation
epoch; the adapter must specify the relationship to RAWX completion explicitly.

SBF measurement epochs and companion-block completion require an explicit
MeasEpoch/MeasExtra/EndOfMeas association rule. RawBits stores associated navigation time directly,
not given an additional transmission-time column in the RawBits family.
The current SBF importer anchors it to the latest valid PVTCartesian (4006),
PVTGeodetic (4007), ReceiverTime (5914), or EndOfPVT (5921) in stream order.
Include at least one of these when recording RawBits. MeasEpoch timestamps do
not substitute for independent navigation context. RawBits block SIS timestamps
are neither retained nor used as fallback; complete SIS time need not be
recoverable from an individual canonical body. Unknown/expired context emits
null-time RawBits; invalid navigation anchors disable the current context.
UBX EOE does not reset the RawBits anchor. Both protocols follow the
[receiver-time association policy](telemetry-time.md).

RTCM3 mapping must resolve the full epoch/date/time-scale context; a partial
time-of-week alone is not a complete GPST timestamp. The adapter must validate
MSM multiple-message completion across the relevant station/message sequence,
not close an epoch merely because one selected constellation was processed.
Exact message IDs, time anchoring and sequence rules remain mapping review
items; Core does not invent them.

RINEX epochs use their declared time system and record structure. Decode header
scale factors and applicable event metadata. `SYS / PHASE SHIFT` is explicitly
unsupported and ignored, not mapped to CommonNEX or applied/undone. Its presence
alone does not reject a file; see the
[phase-shift exclusion](rinex-mapping.md#unsupported-phase-shift-declaration).
Only DBHZ S observables are supported. An explicitly different unit is an
unsupported-unit error. Interpret an omitted unit header under the applicable
version's rules, not by guessing from numerical magnitude. Inputs declaring
applied observation clock-offset correction (`RCV CLOCK OFFS APPL=1`) are
unsupported, not silently accepted or undone. An omitted header follows the
supported version's documented default. Optional epoch offset estimates are
RINEX-specific auxiliary quantities, not instructions to correct observations.

## Observation quality mapping

### MeasExtra uncertainty bounds

Use separate `stddev_is_lower_bound` fields for code, phase and Doppler.
MeasExtra has no global saturation bit: `CodeVar` and `CarrierVar` independently
use 65534 as their clipped maximum and 65535 as unavailable. Check these codes
before conversion. Code stddev is sqrt(CodeVar * 1e-4) meters; phase stddev is
sqrt(CarrierVar * 1e-6) cycles. Store both as float32, not integer variances.
The clipped maximum maps to true, other available codes to false, and unavailable
codes to null stddev and null bound flag.

Doppler variance is carrier variance times `DopplerVarFactor` in Hz2/cycles2.
For a finite positive factor, propagate the carrier uncertainty bound flag to
Doppler quality. This is a derived bound, not an independently clipped Doppler
counter, and says nothing about clipping of the Doppler observable itself.
Unavailable inputs must not produce an apparently uncensored uncertainty.

The importer implements MeasExtra revisions 0-3. A finite zero Doppler factor
produces zero derived uncertainty with a false lower-bound flag, not proof of
perfect accuracy. Invalid/missing inputs remain null. RAWX bound flags remain
unknown; a finite RAWX stddev alone does not establish bound semantics.

MeasExtra joins the same source epoch by WNc/TOW, RxChannel, native signal and
antenna. MeasEpoch Type2 inherits the parent channel. Either block may arrive
first, including across a chunk or file boundary; matching EndOfMeas triggers
the join. The replay cursor starts at the first contributing block. Unmatched
or ambiguous keys are counted, not guessed or applied multiple times.

MeasExtra CodeVar/CarrierVar replace unavailable base uncertainties. CN0HighRes
adds only to an available base C/N0. Available MeasExtra LockTime supplies the
longer lock counter (65534 is a lower bound); unavailable extra lock leaves the
base duration intact. CumLossCont is preserved modulo 256 without unwrapping.
Map MPCorrection and SmoothingCorr by 0.001 m, and CarMPCorr by 1/512 cycles,
into `receiver_corrections`; no correction is applied during import.
Preserve the finite nonnegative DopplerVarFactor as `doppler_variance_factor`.
Revision 0 lacks CumLossCont/CarMPCorr; revision 3 adds CN0HighRes and extended
signal IDs. Reserved bits and padding are not scientific fields. Decode N as
modulo 256 using actual block/sub-block lengths, not as a plain loop bound.

### Source mappings

These are selected schema mappings, not a claim of implemented adapter coverage.

| Source | `cn0_db_hz` | `half_cycle_ambiguity` | `half_cycle_subtracted` |
| --- | --- | --- | --- |
| UBX RAWX | `cno`, dB-Hz | Reverse the defined `halfCyc` validity indication for applicable observations | `subHalfCyc` |
| SBF Measurements | Decoded `MeasEpoch.CN0`, augmented by applicable MeasExtra high-resolution bits | Type1/Type2 `ObsInfo` bit 2 | null |
| RTCM3 MSM7 | Decoded DF408 CNR, 0.0625 dB-Hz resolution | DF420 | null |
| RINEX 3/4 | Supported DBHZ S observable after storage scaling | Interpret phase LLI bit 1 under its version-specific semantics | null |

SBF base C/N0 has 0.25 dB-Hz resolution and signal-dependent decoding;
MeasExtra adds `CN0HighRes * 0.03125` dB-Hz. The 0-7 high-resolution value
is a fractional extension, not a strength rank. UBX NAV-SAT/NAV-SIG `qualityInd`
0-7 is tracking status, not RINEX SSI or a direct RAWX validity replacement.
Only RINEX import fills `rinex_ssi`, `rinex_lli` and `rinex_epoch_flag`.
Do not synthesize these fields from another protocol's indicators.

Missing half-cycle subtraction information remains null, not false. A lock
reset does not prove a half-cycle subtraction. A set subtraction flag reports
an already-performed receiver operation; preserve the exported phase.

Source variances become common standard deviations after unit/sentinel decoding.
Preserve direct standard deviations without inventing values for absent fields.
Companion-block association must use matching epoch, antenna and signal context;
missing MeasExtra does not discard usable MeasEpoch observables. Additional
quality remains null. Preserve association across physical file boundaries.

## Progress

### Selected contracts

- [x] Separate measurement timestamps from navigation-context time.
- [x] Select SBF Measurements as the acquisition profile; retain explicit
  historical-input limitations rather than requiring identical message sets.
- [x] Exclude applied observation clock-offset correction; preserve source
  measurement-clock evidence separately without substituting navigation telemetry.
- [x] Bound RawBits anchor age by 10 nominal periods; retain untimed records without backlog.

### Source validation still required

- [ ] Enumerate importer source-to-canonical signal mappings and their validated
  protocol revisions; source version selection is not a CommonNEX storage field.
- [ ] Define source validity, uncertainty scaling and no-data mappings.
- [ ] Define lock-duration units, quantization intervals and saturation limits.
- [ ] Finalize companion association and missing-companion completion rules.
- [ ] Verify phase/Doppler conventions without unwrapping or slip repair.
- [x] Retain SBF per-observation smoothing state independently of MeasExtra amounts.
- [ ] Define remaining phase-convention and DCB/PCV metadata retention.
- [ ] Verify future RINEX epoch-offset signs and reject applied correction without
  applying corrections or using navigation telemetry as a substitute.

### Implementation

- [x] Implement UBX/SBF measurement-clock evidence and completion events.
- [ ] Implement remaining protocol adapters and discontinuity Events.

References: [RINEX 4.02](https://files.igs.org/pub/data/format/rinex_4.02.pdf),
[u-blox F9 HPG 1.51](https://content.u-blox.com/sites/default/files/documents/u-blox-F9-HPG-1.51_InterfaceDescription_UBXDOC-963802114-13124.pdf),
[mosaic-X5 4.15.0](https://docs.sparkfun.com/SparkFun_GNSS_mosaic-X5/assets/component_documentation/firmware/mosaic-X5_Firmware_v4.15.0_Reference_Guide.pdf),
[RTCM MSM overview](https://www.euref.eu/sites/default/files/symposia/2018Amsterdam/03-02-Soehne.pdf).

Every adapter documents required messages, exact source revisions, usable
time, completion, missing-companion behavior and supported mappings. Emit no
fabricated epoch to satisfy a profile. An incomplete tail remains incomplete.
An explicit request for unsupported protocol/capability fails clearly.

## Configuration versus storage

Enabling a profile selects what the receiver supplies; core membership does not
make every family mandatory in every source. An Observatory recording configuration may
require RawBits while an observation-only CommonNEX consumer ignores it.
Decode supported enabled families in the same pass, independently of whether a
PPP, SBAS or clock-analysis facility is currently running.
