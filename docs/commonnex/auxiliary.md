# CommonNEX receiver telemetry

The implemented `receiver-telemetry` catalog replaces the four separate clock,
measurement-clock, status and pulse catalogs. No legacy-reader fallback is
provided. Re-import old products explicitly; tools never delete old data.

## Row and timing contract

One row collects a navigation epoch's receiver reports. UBX and SBF share the
same delayed assembler in the Python-free native engine. A new PVT epoch
publishes the preceding window with `collection_complete=true`. This indicates
window closure, not that every report or signal was present. Chunk, ordinary
file, midnight and live-delivery boundaries never finalize this state.

UBX NAV-PVT is the trigger. Pending MON-SYS and RAWX clock evidence belong to
the following PVT cycle. NAV-TIMEGPS with matching iTOW supplies full GPST,
including fTOW; RAWX retains its own independent timestamp. TIM-TP belongs to
the report cycle but retains its own target-pulse time. SBF PVT Cartesian and
Geodetic with the same TOW/WNc are one epoch. Source-timed pre-PVT reports wait
for their window; ReceiverStatus after EndOfPVT still belongs to that epoch.

Scalar duplicates with identical values merge; conflicting values raise.
No status/temperature forward fill occurs. Lists retain every occurrence in
acquisition order. Unknown times remain null, never inferred from host time or
uptime. Undated partial windows use the last known archive day, initially the
GPST origin. Measurements and pulse target time do not replace navigation time.

Explicit end/discontinuity emits partial pending rows with
`collection_complete=false`. File import retains pending state in its checkpoint
by default; `--finalize-telemetry` declares a true end and emits those rows.
Live duration/EOF uses the same finalizer. The assembler checks its pending
state budget every 128 report updates and emits partial rows above 4 MiB;
summary `telemetry_partial_rows` exposes these cases. Repeated MON-SYS without
intervening PVT retains older unassigned status in a partial row, rather than
overwriting it. This is receiver-epoch assembly, not sensor interpolation.

## Common and navigation-clock fields

All fields below are nullable except `setup_id` and `collection_complete`.
All time types use DECIMAL(38,12) seconds; GPST origin is 1980-01-06.

| Field | Type | Meaning |
| --- | --- | --- |
| setup_id | string | Logical station |
| gpst | GpstTimestamp? | Full navigation epoch time |
| receiver_uptime_s | Duration? | Source uptime associated with this window |
| receiver_temperature_c | float32? | Receiver internal temperature |
| cpu_load_percent, cpu_load_max_percent | float32? | Source load and source maximum |
| memory_usage_percent, memory_usage_max_percent | float32? | Source memory usage |
| io_usage_percent, io_usage_max_percent | float32? | Source I/O usage |
| fine_time | bool? | Explicit SBF fine-time status |
| clock_bias_s | TimeDelta? | Receiver minus reference-system time |
| clock_frequency_offset | int64? | Unit 0.000001 ppb |
| time_accuracy_s | Duration? | Source accuracy, not assumed standard deviation |
| frequency_accuracy | uint64? | Unit 0.000001 ppb |
| clock_reference_time_scale | enum? | GPST, GST, BDT or UNKNOWN |
| collection_complete | bool | Window closed by following PVT epoch |

UBX NAV-CLOCK supplies bias (ns), drift (ns/s), tAcc (ns), fAcc (ps/s).
SBF PVT supplies bias (ms), drift (ppm) and TimeSystem; Error != 0 makes its
estimates unavailable. MON-SYS supplies temperature, uptime and utilization;
SBF ReceiverStatus supplies temperature (raw minus 100; zero is unavailable),
uptime, CPU load and fine-time state. Missing data is never substituted with zero.
`_archive_day` exists only on native batches for Python partition routing.

## Ordered report lists

Lists are non-null, with non-null struct items. No report means `[]`, not null.
Unknown fields inside reports are null. Explicit false/zero values are retained.

`measurement_clock` items:

| Field | Type | Mapping |
| --- | --- | --- |
| gpst | GpstTimestamp | Original measurement time |
| adjustment_reported | bool? | RAWX recStat.clkReset |
| cumulative_adjustment_ms | uint64? | MeasEpoch revision 1 CumClkJumps |

The uint64 container does not remove the native modulo-256 behavior. Neither
missing field is inferred from the other. No adjustment is applied to observations.

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

Pulse error means actual edge minus ideal edge, positive late. SBF Offset is
ns; negative means early. SyncAge=255 is capped; receiver-time mode's zero is
not GNSS-lock evidence. UBX qErr maps as -qErr picoseconds; qErrInvalid makes
the numeric error null without discarding other pulse fields. TIM-TP describes
the next pulse. Only locked GPST-based TIM-TP currently has resolved target GPST;
UTC/GST/BDT targets remain null without a supported conversion. Zero qErr alone
is not invalid. UBX sign follows the previously adopted F9T experiment and is
not independently verified on this station's hardware.

## Vendor status structs

A null struct means no report; null members mean unavailable quantities.
`ubx_status`: boot_type uint8?, notice_count/warning_count/error_count uint16?.

`sbf_status`: receiver_state_flags and receiver_error_flags uint32?,
external_error_flags and command_count uint8?, frontends list<struct>.
Flags preserve source bit definitions; external frequency/time are not duplicated
as booleans. CmdCount zero means unavailable; the native counter wraps 255 to 1.
Frontend items contain:

| Field | Type | Meaning |
| --- | --- | --- |
| frontend_code | uint8 | FrontEndID bits 0-4, RF frontend code, not RINEX signal |
| antenna_id | uint8 | FrontEndID bits 5-7 |
| gain_db | int8? | Gain in dB; raw -128 becomes null |
| pll_locked | bool | False for Gain=-128 sentinel |
| sample_variance | uint8? | Normalized IF variance, nominal 100; raw zero becomes null |
| blanking_percent | uint8 | Percentage of blanked samples |

Blanking=0 also occurs without blanking hardware; do not infer capability.
Frontend arrays retain source order and antenna identity, independently of
the selected observation antenna.

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

## Deferred work

### Deferred external-sensor raw text

No station currently has the relevant external sensor hardware. Defer all
parser, schema and importer implementation for this feature.

- [ ] Investigate SBF ASCIIIn and raw NMEA as carriers for external temperature,
      humidity and pressure sensor reports when hardware becomes available.
- [ ] Define a separate `raw-txt` catalog associated with navigation epochs,
      sharing the delayed epoch assembly used by telemetry in live and file
      import. Preserve message order and original payload bytes; do not require
      UTF-8, trim text, normalize line endings or deduplicate reports.
- [ ] Verify ASCIIIn timestamp, input-port and fragmentation semantics, and
      distinguish receiver-embedded text from mixed-stream NMEA or a separate
      sensor connection before finalizing fields and record boundaries.
- [ ] Preserve unknown times without synthesizing GPST. Navigation association
      denotes reception context, not necessarily the sensor measurement time.
- [ ] Keep sensor-value parsing, units and calibration downstream of raw-text
      capture. The catalog name and direction are planned; no final schema is
      specified yet.
