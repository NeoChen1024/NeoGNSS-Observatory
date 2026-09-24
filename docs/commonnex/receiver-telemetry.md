# CommonNEX receiver telemetry

The `receiver-telemetry` catalog stores common receiver quantities and ordered
measurement-clock/pulse reports. Source mappings are defined in
[receiver mappings](receiver-mappings.md#telemetry-mappings).

## Row and timing contract

One row collects a navigation epoch's receiver reports. A new PVT epoch
publishes the preceding window with `collection_complete=true`. This indicates
window closure, not that every report or signal was present. Chunk, ordinary
file, midnight and live-delivery boundaries never finalize this state.

Scalar duplicates with identical values merge; conflicting values raise.
No status/temperature forward fill occurs. Lists retain every occurrence in
acquisition order. Unknown times remain null, never inferred from host time or
uptime. Undated partial windows use the last known archive day, initially the
GPST origin. Measurements and pulse target time do not replace navigation time.

Explicit end/discontinuity emits partial pending rows with
`collection_complete=false`. File import retains pending state in its checkpoint
by default; `--finalize-telemetry` declares a true end and emits those rows.
Live duration/EOF uses the same finalizer. Resource limits can also emit partial
rows; `telemetry_partial_rows` reports these cases. This is receiver-epoch
assembly, not sensor interpolation. See [importer](importer.md) and
[live API](live.md) for operational limits and finalization.

## Common and navigation-clock fields

All fields below are nullable except `setup_id` and `collection_complete`.
All time types use DECIMAL(38,12) seconds; GPST origin is 1980-01-06.

| Field | Type | Meaning |
| --- | --- | --- |
| setup_id | string | Logical station |
| gpst | GpstTimestamp? | Full navigation epoch time |
| receiver_uptime_s | Duration? | Source uptime associated with this window |
| receiver_temperature_c | float32? | Receiver internal temperature |
| cpu_load_percent | float32? | Source CPU utilization in percent |
| clock_bias_s | TimeDelta? | Receiver minus reference-system time |
| clock_frequency_offset | int64? | Unit 0.000001 ppb |
| time_accuracy_s | Duration? | Source accuracy, not assumed standard deviation |
| frequency_accuracy | uint64? | Unit 0.000001 ppb |
| clock_reference_time_scale | enum? | GPST, GST, BDT or UNKNOWN |
| collection_complete | bool | Window closed by following PVT epoch |

## Ordered report lists

Lists are non-null, with non-null struct items. No report means `[]`, not null.
Unknown fields inside reports are null. Explicit false/zero values are retained.

`measurement_clock` items:

| Field | Type | Meaning |
| --- | --- | --- |
| gpst | GpstTimestamp | Original measurement time |
| adjustment_reported | bool? | Source explicitly reports a measurement-clock adjustment |
| cumulative_adjustment_ms | uint64? | Source cumulative millisecond adjustment counter |
| cumulative_adjustment_modulus_ms | uint64? | Known wrap modulus in milliseconds |

A modulus is
positive and its counter is smaller than it; an unknown modulus remains null,
not an assertion that the counter never wraps. The uint64 container does not
change source wrapping behavior. No counter is synthesized from reset flags,
and no adjustment is applied to observations.

`pulse_timing` items:

| Field | Type |
| --- | --- |
| gpst | GpstTimestamp? |
| reference_time_scale | enum? |
| quantization_error_s | TimeDelta? |
| quantization_error_valid, locked, utc_available | bool? |
| raim_status | enum? |
| utc_standard | uint8? |
| sync_age_s | Duration? |
| sync_age_saturated | bool? |

Pulse error means actual edge minus ideal edge: positive is late. Missing or
invalid error values remain null without discarding other pulse fields. Zero
alone is not invalid. Each item's GPST is its target pulse time, not the parent
navigation window time. Reference/lock/synchronization declarations describe
the source report; no correction is applied to observations or pulse timestamps.

## Scope

Telemetry normalizes useful receiver-independent quantities, not vendor status
structures. Common quantities may be null when a protocol does not provide them.
Memory/I/O utilization, resource maxima, boot and message counters, command
counts, raw state/error flags and frontend statistics are outside this catalog.
They remain available in raw archives and protocol parsers. Source validity
flags still guide import even when not independently stored.

## Processing boundary

Events remain separate. Receiver restart inference and RawBits time association
use receiver evidence; no clock correction, unwrap or temperature interpolation
is performed here. Receiver-clock analysis reads this table, flattens pulses
with their own timestamps, and reduces each measurement list for its numerical
unwrap: any reported reset, and the last available cumulative counter. The full
ordered measurement list remains on derived clock rows.

## Sources

- [u-blox interface](https://content.u-blox.com/sites/default/files/documents/u-blox-F9-HPG-1.51_InterfaceDescription_UBXDOC-963802114-13124.pdf)
- [F9T polarity experiment](https://www.anderswallin.net/2019/10/ublox-f9t-qerr-correction/)
- [Septentrio reference, ReceiverStatus/AGCState pp. 402-405](https://docs.sparkfun.com/SparkFun_GNSS_mosaic-X5/assets/component_documentation/firmware/mosaic-X5_Firmware_v4.15.0_Reference_Guide.pdf)
