# SBAS broadcast decoding

[BroadcastMessageDecoder](broadcast-decode-realtime.md) decodes standard SBAS
L1 message types 0, 1, 2–7, 9, 10, 12, 17, 18, 24–28 and 63. It provides
MessageOutput through typed Arrow batches and the example JSONL CLI. Applying
corrections, mask/issue association, aging, service-region assembly and SBAS
state snapshots are downstream work, not implemented by this decoding stage.
The downstream [SBAS grid processor](subframes.md) implements per-source
ionospheric snapshots and statistics with explicit research aging rules.

## Scope and identity

One shared L1 decoder serves MSAS, BDSBAS, KASS, GAGAN and SouthPAN. Provider
identity does not select a separate standard-field decoder or rank the outputs.
L5/DFMC, QZSS L1S, BeiDou navigation B1C and PPP-B2b are distinct formats.
GLONASS/NavIC scientific parameters remain out of scope, including the optional
GLONASS timing part of MT12. Reserved/internal-test MT62 remains unsupported.

Input is CommonNEX `SBAS_L1` / `SBAS_L1_250_V1`, following the
[canonical formats](commonnex/raw-bits-formats.md). Outputs are derived products,
not CommonNEX catalogs. Repeated received messages remain repeated occurrences.
All output kinds share these fields:

| Field | Arrow type | Meaning |
| --- | --- | --- |
| `setup_id` | string | Input logical station |
| `satellite_system` | string | `S`, the broadcaster's system |
| `broadcasting_satellite` | int64 | Broadcaster in CommonNEX Sxx numbering, not wire PRN |
| `message_family` | string | `SBAS_L1` |
| `bitstream_source` | list<string> | Original signal contributors |
| `nav_epoch_gpst` | decimal128(38,12)? | Unmodified input navigation context |
| `message_type` | int64 | Original wire message type |
| `output_sequence` | int64 | Decoder-run output order, not persistent identity |

Provider lookup/ranking is left to consumers. The one supported service-specific
payload interpretation is SouthPAN early Open Service MT0 from S22 (PRN122),
as documented by its operator. Native `SBAS::parse_l1` defaults to the standard
profile; callers explicitly choose `L1Profile::southpan_open` to decode that
embedded MT2 body. The CommonNEX adapter selects it for S22 only. Other MT0
bodies remain uninterpreted, not guessed from their apparent bit patterns.

Input integrity failures, invalid content and unsupported types are counted,
not emitted as valid decoded parameters. The SBAS parser independently checks
preamble and CRC. Original rejected/unsupported bodies remain upstream RawBits.

## Types and conventions

Output indices, codes and counters are int64; booleans remain bool. Physical
parameters are float64 in the units indicated by field names. Time coordinates,
durations and clock offsets are decimal128(38,12) seconds. Binary encoded clock
offsets are rounded directly with integer arithmetic, ties to even, to avoid an
intermediate binary64 rounding. Clock rates remain float64.

Repeated groups are non-null `list<struct>` with `[]` for no meaningful entries.
Optional scalar/group values are null, never a fabricated zero. Their Arrow
types remain fixed even when every value is null or every list is empty.
XYZ vectors are three-element `list<float64>` in ECEF order. Angular coordinates
use degrees. Native network reference times use SNT; they are not relabeled
GPST or resolved using the host date. MT12's explicitly GPS fields stay GPS.

`mask_bit` is a one-based position in the fixed mask. `mask_position` is the
one-based ordinal among enabled bits, not a PRN or IGP geographic identifier.
Unresolved positions remain useful message fields; no mask is required merely
to decode them. A mask from another broadcaster is not implicitly borrowed.

## Message fields

Unless otherwise stated, numeric codes below are int64, physical values are
float64, and fields ending in a time duration/coordinate `_s` are decimal seconds.

| Kind / MT | Additional fields |
| --- | --- |
| `sbas_service_status` / 0 | `restriction=DO_NOT_USE_FOR_SAFETY`, `payload_interpretation`; nullable `embedded_fast_issue {iodp, iodf}` and `embedded_fast_corrections` using fast entries below |
| `sbas_prn_mask` / 1 | `iodp`, `entries: [{mask_position, mask_bit}]` |
| `sbas_fast_corrections` / 2–5 | `iodp`, `iodf`, `fast_block` (0–3), `entries: [{mask_position, correction_m, udrei, status}]` |
| `sbas_integrity` / 6 | `iodf_by_block: list<int64>` of length 4; `entries: [{mask_position, udrei, status}]` |
| `sbas_fast_degradation` / 7 | `iodp`, `latency_s`; `entries: [{mask_position, degradation_index, degradation_m_s2}]` |
| `sbas_geo_ephemeris` / 9 | `reference_sod_s`, `time_system=SNT`, `ura_index`, `broadcast_status`, `position_m`, `velocity_m_s`, `acceleration_m_s2`, `clock_offset_s`, `clock_drift_s_s` |
| `sbas_degradation` / 10 | Coefficients and intervals listed below |
| `sbas_network_time` / 12 | `time_system=SNT`, `utc_id`, `utc_available: bool`, `gps_tow_s`, `gps_week_mod1024`, nullable `utc_parameters` below |
| `sbas_geo_almanac` / 17 | `reference_sod_s`, `time_system=SNT`; `entries: [{prn, subject_number?, health_status, service_provider_id, ranging_on, precision_corrections_on, basic_corrections_on, position_m, velocity_m_s}]` |
| `sbas_ionospheric_mask` / 18 | `band`, `iodi`, `band_count`; `entries: [{mask_position, mask_bit, latitude_deg, longitude_deg}]` |
| `sbas_mixed_corrections` / 24 | `iodp`, `iodf`, `fast_block`, `fast_corrections` and `long_term_corrections` lists |
| `sbas_long_term_corrections` / 25 | `entries` using long-term entries below |
| `sbas_ionospheric_delays` / 26 | `band`, `block`, `iodi`; `entries: [{mask_position, vertical_delay_m?, givei, status}]` |
| `sbas_service_region` / 27 | `iods`, `message_count`, `message_number` (both 1–8), `priority`, `delta_udre_inside_index`, `delta_udre_outside_index`, decoded dimensionless `delta_udre_inside`/`delta_udre_outside`; `regions` below |
| `sbas_covariance` / 28 | `iodp`; `entries: [{mask_position, scale_exponent, e_elements, r_upper}]` |
| `sbas_null` / 63 | No additional scientific fields |

### Degradation and network time

MT10 exposes all in-scope coefficients: `brrc_m`, `cltc_lsb_m`, `cltc_v1_m_s`,
`iltc_v1_s`, `cltc_v0_m`, `iltc_v0_s`, `cgeo_lsb_m`, `cgeo_v_m_s`, `igeo_s`,
`cer_m`, `ciono_step_m`, `iiono_s`, `ciono_ramp_m_s`, `rss_udre: bool`,
`rss_iono: bool`, and dimensionless `ccovariance`. The RSS flags describe the
broadcast combination rule; this decoder does not calculate a protection level.

MT12 `utc_parameters` contains `a0_s`, `a1_s_s`, `reference_tow_s`,
`reference_week_mod256`, `leap_week_mod256`, `leap_day`, `leap_seconds` and
`future_leap_seconds`. The last two are signed integer seconds, and the week/day
fields are integer codes. UTC ID 7 means no UTC parameters, so the whole struct
is null. Truncated weeks are not resolved into an absolute epoch by this layer.

### Long-term entries

MT24 and MT25 share exactly the same entry type:

- Integer `half` (0/1 within the standard MT25 body; MT24 uses half 1),
  `mask_position`, `issue`, `iodp`, and boolean `velocity_code`.
- `delta_x_m`, `delta_y_m`, `delta_z_m`, `clock_offset_s`.
- Nullable `delta_vx_m_s`, `delta_vy_m_s`, `delta_vz_m_s`,
  `clock_drift_s_s` and native SNT `reference_sod_s`, present only for velocity
  code 1. Absence is not a measured zero rate.

Each half retains its own IODP. The fast half's IODP in MT24 must not overwrite
the long-term half's issue. A zero mask position denotes a dummy and is omitted;
unhealthy real subjects are not omitted. Matching `issue` to an ephemeris and
applying correction signs/formulas is the consumer's responsibility.

### Service regions and covariance

MT27 `regions` entries contain `latitude1_deg`, `longitude1_deg`,
`latitude2_deg`, `longitude2_deg` and `quadrangle: bool`. Coordinate 3 uses
latitude 1/longitude 2; coordinate 4 uses latitude 2/longitude 1. The standard
boundary order is 1–2–3–1 for triangles and 1–3–2–4–1 for quadrangles. These are
latitude/longitude-space boundaries, not an instruction to draw great circles.
Repeated messages are emitted separately, not assembled or applied geographically.

MT28 `e_elements: list<int64>` retains ten encoded upper-triangular elements in
order E11,E22,E33,E44,E12,E13,E14,E23,E24,E34. `r_upper: list<float64>` has the
same order after multiplying by `2^(scale_exponent-5)`. This is a dimensionless
factor of the relative covariance, not covariance in square meters. Reconstruct
the upper-triangular R with zero lower entries; the relative matrix is RᵀR.
The decoder does not project it along a user line of sight or apply UDRE.

## Status, missingness and limitations

- UDREI 0–13 maps to `USABLE`, 14 to `NOT_MONITORED`, 15 to `DO_NOT_USE`.
  MT26 delay code 511 yields null delay and `DO_NOT_USE`; otherwise GIVEI 15
  yields `NOT_MONITORED`. A numeric delay with that status is not usable.
- These are broadcast indicators, not complete application eligibility.
  GEO URA 15 similarly reports `DO_NOT_USE`. Index values are not linear weights.
- MT5 has 12 meaningful correction slots; position 52 is spare. MT26 position
  is `15 * block + entry_index + 1`, with zero-based entry index. Consumer mask
  association determines which transmitted slots refer to active grid points.
- MT17 omits zero-PRN entries. `subject_number` is Sxx for PRN120–158, otherwise
  null while the transmitted `prn` remains available. Its function flags are
  independent of whether an almanac entry was successfully decoded.
- MT9's first eight payload bits are reserved in the selected ICAO field table;
  the Arrow output does not invent an IODN. Original bits remain upstream.
- MT0 remains type 0 even when its embedded fast corrections are decoded.
  `payload_interpretation` is `SOUTHPAN_OPEN_MT2` or `UNINTERPRETED`; unknown
  interpretation has null issue and an empty correction list. No holdoff,
  recovery, safety claim or automatic use/discard policy is imposed here.
- No SBAS candidates are currently added to periodic state snapshots. SBAS
  messages contribute to the existing lifetime decoded-message statistic.

## Remaining downstream work

- [x] Source-local ionospheric mask/issue association and research freshness rules in the grid processor.
- [x] Per-source grid snapshots and window statistics.
- [ ] Satellite correction state and service-region assembly.
- [x] Grid research MT0 annotations and correction/mask aging.
- [ ] General correction application and service-specific recovery policies.
- [x] Unified snapshot-first grid processing using the shared decoder; remove legacy interval APIs.
- [ ] Additional service/profile extensions when documented and useful.

## References

- [ICAO SBAS working document](https://www.icao.int/sites/default/files/sp-files/airnavigation/Documents/NSP5_Report%20on%20Agenda%20Item%202.APPENDIX%20A2%20-%20DFMC%20SBAS%20SARPS%20Part%20B.pdf), L1 sections 3.5.4 and 3.5.6, Tables B-37–B-53: selected field-layout reference, not a claim of aviation certification or latest operational profile coverage.
- [BDSBAS-B1C ICD 1.0](https://en.beidou.gov.cn/SYSTEMS/ICD/202008/P020200803538292532733.pdf): relative covariance factor semantics.
- [SouthPAN early Open Service factsheet](https://www.ga.gov.au/__data/assets/pdf_file/0003/123699/SRF156631-SouthPAN-factsheet.pdf): PRN122 MT0 contains MT2 data, without Safety of Life service.
- [ENRI message field tables](https://www.enri.go.jp/jp/research/organization/nav/program/message.html): distinguish SBAS and L1-SAIF columns.
