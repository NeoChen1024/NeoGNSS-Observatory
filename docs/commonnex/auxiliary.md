# CommonNEX auxiliary schemas

Status: v0 contract. UBX/SBF MeasurementClock, ReceiverClock, ReceiverStatus
and PulseTiming import are implemented. Receiver-clock analysis consumes these
CommonNEX catalogs; raw parsing is confined to the importer.

[Overview](overview.md) · [Receiver time association](telemetry-time.md)

## Scope

Keep receiver-reported quantities at their own cadence, independently of
Observation. Import normalizes units and documented signs but does not apply
clock correction to observations, unwrap bias/counters, infer adjustment
amounts, interpolate temperature or perform thermal fits. Those are downstream
analysis tasks.

Use four typed catalogs: `receiver-clock`, `measurement-clock`,
`pulse-timing`, and `receiver-status`. Temperature and uptime from the same
status report belong together; do not duplicate temperature onto clock rows.
Receiver solution import is a separate future feature, not a requirement here.

## Common receiver context

| Field | Type | Meaning |
| --- | --- | --- |
| `setup_id` | string | Logical station |
| `gpst` | GpstTimestamp? | Reliable sample/event time or navigation association; null when unavailable |
| `receiver_uptime_s` | Duration? | Reported or freshly associated receiver uptime; no extrapolation/unwrap |
| `time_basis` | enum | SOURCE, NAVIGATION, or UNKNOWN |
| `uptime_basis` | enum? | REPORTED or ASSOCIATED; null without uptime |
| `source_message` | string | Concrete protocol message name needed to interpret reporting semantics |

Use the shared [association, restart and placement rules](telemetry-time.md).
Pulse GPST denotes the target pulse, not message arrival. Nearby status uptime
does not claim an exact future-pulse uptime. Observation/MeasurementClock time
remains independent. No row IDs, epoch foreign keys or generic `reference`,
`quality`, `source_identity` containers are required. Concrete reference and
validity fields remain essential.

All time quantities use DECIMAL(38,12) seconds: GpstTimestamp is seconds since
1980-01-06 GPST, TimeDelta is signed, Duration is nonnegative. Convert small
native time components directly; never form a large absolute binary64 time.
Use round-half-to-even at 1 ps where necessary.

## ReceiverClock

| Field | Type | Meaning |
| --- | --- | --- |
| `clock_bias_s` | TimeDelta? | Receiver time minus declared reference-system time |
| `clock_frequency_offset` | int64? | Relative frequency; positive means faster; step = 0.000001 ppb |
| `time_accuracy_s` | Duration? | Source time accuracy, not implicitly a standard deviation |
| `frequency_accuracy` | uint64? | Source frequency accuracy; step = 0.000001 ppb |
| `reference_time_scale` | enum | Reference of the estimates, not the sample time axis |

UBX NAV-CLOCK maps clkB ns to seconds, clkD ns/s to ppb, tAcc ns to
seconds and fAcc ps/s to 0.001 ppb units before canonical integer scaling.
SBF PVT RxClkBias uses milliseconds and RxClkDrift ppm; retain its TimeSystem.
Missing or unusable estimates are null, never substituted from other quantities.
NAV-TIMEGPS fTOW completes navigation time; it is not clock bias and needs no
duplicate `navigation_time_offset_s` field here.

## MeasurementClock

One row per complete source measurement epoch, not per satellite.
The current implemented context is `setup_id` and non-null measurement `gpst`.

| Field | Type | Meaning |
| --- | --- | --- |
| `adjustment_reported` | bool? | Explicit source adjustment flag, including false |
| `cumulative_adjustment_ms` | uint64? | Source-reported cumulative millisecond count, including zero |

RAWX recStat.clkReset supplies only the boolean. SBF MeasEpoch revision 1
CumClkJumps supplies only the cumulative count, natively modulo 256; revision 0
does not define it. uint64 is the storage container, not a guarantee of a native
64-bit, monotonic or nonwrapping counter. Preserve source values without
deriving the missing field. Do not equate adjustment with reboot or loss of lock.
Contradictory counters in one assembled measurement epoch remain errors.

A future RINEX adapter may retain per-epoch `rinex_receiver_clock_offset_s`
separately. Input declaring applied observation clock-offset correction is
rejected; no generic correction-state workflow is required.

## PulseTiming

| Field | Type | Meaning |
| --- | --- | --- |
| `pulse_quantization_error_s` | TimeDelta? | Actual edge minus ideal edge, quantization component only; positive late |
| `quantization_error_valid` | bool? | Explicit source validity; invalid quantity is null |
| `reference_time_scale` | enum | Pulse reference system |
| `utc_standard` | uint8? | UBX standardized UTC realization code, when applicable |
| `utc_available` | bool? | Explicit source UTC availability |
| `locked` | bool? | Explicit GNSS pulse lock state |
| `raim_status` | enum? | UNAVAILABLE, NOT_ACTIVE or ACTIVE, not pass/fail |
| `sync_age_s` | Duration? | Reported time since synchronization |
| `sync_age_saturated` | bool? | At source's capped upper value, hence a lower bound |

Only one sawtooth quantity is stored; do not also create `pps_offset_s` for
the same report. It is not total PPS error including antenna/cable/position
effects. Downstream correction of a measured edge is `measured - error`;
import only records the error.

SBF xPPSOffset.Offset ns is negative for early pulses, so multiply by 1e-9.
SyncAge=255 is capped; receiver-time mode reports zero, not proof of GNSS lock.
The block is emitted after the pulse.

UBX TIM-TP describes the next pulse. qErr is signed picoseconds;
qErrInvalid makes the numerical value null. Zero alone is not invalid.
TpNotLocked indicates local-time pulse generation and potentially invalid
week/TOW. Preserve reference flags; do not treat UTC/GST/BDT as GPST without
a valid conversion. The current TIM-TP adapter resolves target GPST only for
locked GPST-based pulses; other time bases retain quantities/flags with null GPST.
This conversion limitation is not a reason to replace pulse time with navigation time.

Canonical UBX mapping is `-qErr * 1e-12`: a first-hand F9T experiment found
that qErr is added to an interval measured from reference to u-blox pulse.
This supports positive qErr meaning early, opposite the canonical error sign.
Evidence limitation: the cited manufacturer interface defines units/flags but
does not explicitly state the polarity equation; this mapping has not been
verified with this station's hardware.

Sources:
- [u-blox interface](https://content.u-blox.com/sites/default/files/documents/u-blox-F9-HPG-1.51_InterfaceDescription_UBXDOC-963802114-13124.pdf#page=211)
- [First-hand F9T polarity experiment](https://www.anderswallin.net/2019/10/ublox-f9t-qerr-correction/)
- [Septentrio reference](https://docs.sparkfun.com/SparkFun_GNSS_mosaic-X5/assets/component_documentation/firmware/mosaic-X5_Firmware_v4.15.0_Reference_Guide.pdf#page=379)

## ReceiverStatus

Common uptime is normally directly reported here.

| Field | Type | Meaning |
| --- | --- | --- |
| `receiver_temperature_c` | float32? | Receiver internal temperature in degrees Celsius |
| `fine_time` | bool? | Explicit source fine-time status, when provided |

Use MON-SYS runTime/tempValue and SBF ReceiverStatus UpTime/Temperature.
Do not claim ambient or oscillator-crystal temperature. Missing status remains
absent/null. Temperature association for plotting belongs downstream.

## Implementation tracking

- [x] Import measurement-clock evidence independently of observations.
- [x] Import all three additional telemetry catalogs through Arrow batches.
- [x] Use the common nullable-GPST/uptime policy for RawBits and telemetry.
- [x] Persist restart evidence and resume the same association state.
- [x] Move receiver-clock analysis to CommonNEX input; retain derived unwrap,
      temperature joins and adjustment inference downstream.
