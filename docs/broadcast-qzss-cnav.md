# QZSS CNAV broadcast decoding

The [broadcast API](broadcast-decode-realtime.md) decodes `QZS_CNAV` from
CommonNEX `CNAV_300_V1` bodies on `QZS_L2C` and `QZS_L5_I`. This is a derived
parameter API, not a CommonNEX catalog or an orbit-calculation backend.
GPS CNAV, QZSS CNAV-2 and QZSS augmentation services are not covered here.

The reviewed specification is [IS-QZSS-PNT-006](https://qzss.go.jp/en/technical/download/pdf/ps-is-qzss/is-qzss-pnt-006.pdf),
sections 4.3 and 5. All types defined in its CNAV inventory are supported.
Failed RawBits checks are rejected. A passing check does not establish health,
authenticity or parameter applicability. Header preamble, broadcasting PRN ID,
TOW range and source signal are checked separately.

## Types and units

All outputs retain `setup_id`, `satellite_system`, `broadcasting_satellite`,
`message_family`, `bitstream_source`, nullable `nav_epoch_gpst` and
`output_sequence`. CNAV adds `message_type`, `prn_id_raw`, `tow_count` and `alert`.
The PRN ID is the six low PRN bits, 1-10 for QZSS positioning PRNs 193-202;
it must agree with the CommonNEX broadcasting satellite.
`tow_count` is the transmitted end-of-message TOW in six-second units. It never
replaces the input navigation-context timestamp.

- Codes, indices, weeks, page numbers and counters are Arrow int64; flags are bool.
- Physical parameters are float64 unless specified below. Angles and angular
  rates use radians and radians/second; angular acceleration uses radians/s².
  EOP explicitly uses arcseconds and per-day rates.
- Time coordinates, reference TOW, clock offsets and group delays use
  decimal128(38,12) seconds. Binary-scaled offsets are rounded directly to integer
  picoseconds, ties to even. Polynomial rates remain float64.
- `_gpst` fields are nullable. Transmitted reference weeks/TOW are retained;
  modulo weeks resolve against reception context, not host time. A standalone
  clock's absent week is associated with the nearest week in that context.
  EOP instead requires the matching UTC reference described below.
- `l1_unhealthy`, `l2_unhealthy`, `l5_unhealthy` preserve the broadcast health
  bits. They are not inferred from observed signal tracking. URA indices remain
  indices, not guessed meter-valued uncertainties.

## MessageOutput

Each received message produces its applicable records immediately. Repeated
content is emitted again. Reduced/Midi almanacs are per subject satellite, never
a delayed constellation collection. Subject PRN ID zero is invalid/dummy and
produces no almanac entry; IDs outside 1-10 are counted and omitted.

| Kind | Type | Fields in addition to the common header |
| --- | --- | --- |
| `cnav_ephemeris_1` | 10 | `week_raw`, `top_s`, `toe_s`, three health flags, `ura_ed_index`, `delta_a_m`, `a_reference_m`, `a_dot_m_s`, `delta_n_rad_s`, `delta_n_dot_rad_s2`, `m0_rad`, `eccentricity`, `omega_rad`, `integrity_status_flag`, `ephemeris_status_flag` |
| `cnav_ephemeris_2` | 11 | `toe_s`, `omega0_rad`, `i0_rad`, `delta_omega_dot_rad_s`, `omega_dot_reference_rad_s`, `idot_rad_s`, `cis_rad`, `cic_rad`, `crs_m`, `crc_m`, `cus_rad`, `cuc_rad` |
| `cnav_clock` | 30,31,32,33,35,37,61 | `top_s`, `toc_s`, `top_gpst`, `toc_gpst`, `ura_ned0_index`, `ura_ned1_index`, `ura_ned2_index`, `af0_s`, `af1_s_s`, `af2_s_s2` |
| `cnav_group_delay` | 30,61 | `reference_signal`, `tgd_s`, `isc_l1_ca_cb_s`, `isc_l2c_s`, `isc_l5_i_s`, `isc_l5_q_s` |
| `cnav_ionosphere` | 30,61 | `region` (`WIDE_AREA` or `JAPAN`), `alpha0`-`alpha3`, `beta0`-`beta3` |
| `cnav_prediction_week` | 30 | Eight-bit `week_raw`, `top_s`, `top_gpst` |
| `cnav_reduced_almanac` | 12,31 | `subject_sv_id`, `week_raw`, `toa_s`, `reference_gpst`, `orbit_reference`, `delta_a_m`, `a_reference_m`, `omega0_rad`, `phi0_rad`, three health flags |
| `cnav_midi_almanac` | 37 | `subject_sv_id`, `week_raw`, `toa_s`, `reference_gpst`, `orbit_reference`, three health flags, `delta_eccentricity`, `delta_i_rad`, nullable `eccentricity`/`i0_rad`, `omega_dot_rad_s`, `sqrt_a`, `omega0_rad`, `omega_rad`, `m0_rad`, `af0_s`, `af1_s_s` |
| `cnav_eop` | 32 | `teop_s`, `top_s`, `reference_gpst` (null until snapshot association), `pm_x_arcsec`, `pm_y_arcsec`, `pm_x_rate_arcsec_day`, `pm_y_rate_arcsec_day`, `ut1_minus_utc_s`, `ut1_minus_utc_rate_s_day` |
| `cnav_utc` | 33 | `a0_s`, `a1_s_s`, `a2_s_s2`, `delta_tls_s`, `tot_s`, `top_s`, `week_raw`, `reference_gpst`, `wn_lsf_raw`, `dn`, `delta_tlsf_s`, `utc_reference=UTC_NICT` |
| `cnav_time_offset` | 35 | `tggto_s`, `week_raw`, `reference_gpst`, `gnss_id`, `source_time_system=QZSST`, `target_time_system`, `a0_s`, `a1_s_s`, `a2_s_s2` |
| `cnav_text` | 15 | Four-bit `text_page`, 29-byte binary `payload`, escaped `display_text`, `printable_ascii` |
| `cnav_qznma_payload` | 60, L5 only | Binary `payload`, `payload_bit_length=238`; MSB-first with zero trailing byte padding |
| `cnav_test_mode` | 0 | Common header only; not usable navigation parameters |

`cnav_prediction_week.week_raw` is modulo 256; other CNAV reference week fields
are modulo 8192. `dn` retains the transmitted leap-second day code. Unavailable
GNSS ID 0 and reserved IDs remain MessageOutput; they are not snapshot models.
Known time-offset targets are 1=GST, 2=GLOT and 3=GPST. Extracting a GLOT offset
does not add GLONASS scientific processing.

Klobuchar coefficients retain second/semicircle-power units and are not converted
to a radians-based polynomial. ISC/TGD use the QZSS L1C/A-or-L1C/B reference, not
GPS L1 P(Y). EOP is **UT1 minus UTC**, not GPS CNAV's UT1 minus GPST.
QZNMA is payload extraction only, not signature assembly or authentication.
Its data region follows [IS-QZSS-SAS-001 §5.2.10](https://qzss.go.jp/en/technical/download/pdf/ps-is-qzss/is-qzss-sas-001.pdf).

Ephemeris `a_reference_m` is 42,164,200 m and
`omega_dot_reference_rad_s` is −2.6×10⁻⁹π rad/s. Neither is a GPS-orbit default.
The additional CNAV rates are preserved rather than projected into LNAV fields.
`sqrt_a` is sqrt(m), not meters.

Almanac `orbit_reference` is `QZO` for QPNT-006 slots 2-5, `GEO_QGEO` for 7-9,
and `UNKNOWN` otherwise. Midi eccentricity/inclination add the documented
reference values (0.06 and π/4 for QZO; zero for GEO/QGEO); unknown references
leave normalized values null while retaining the deltas. Reduced almanacs retain
their distinct `phi0_rad` and semi-major-axis difference. Consumers apply the
orbit-specific constants in QPNT §5.7.2.2, not the Midi-almanac defaults.

## Ephemeris assembly and snapshots

`cnav_ephemeris` combines fresh type 10, type 11 and clock fields from one
broadcaster and one signal-source set. Type 10/11 `toe_s` must agree; type 10
and clock `top_s` must agree. Changed same-part content invalidates the pending
assembly. Completion clears its pieces, so another output requires fresh pieces.
The timeout is 144 seconds for L2C and 72 seconds for L5, three maximum broadcast
intervals from QPNT Table 4.3.1-1. Repeats do not extend it; discontinuity clears
it. Alert-marked or untimed reports do not enter assemblies.

The assembled record includes all three parameter groups, `toe_gpst`, `toc_gpst`,
`top_gpst`, `first_received_gpst` and
`orbit_clock_reference=QZS_L1_CP_QZS_L5_Q`. Its common header belongs to the
last contributing message. CNAV has no LNAV IODE/IODC to synthesize.
Transmission week and reference week may differ across Sunday; toe is resolved
relative to transmitted week/TOW, and toc/top relative to that toe. Invalid TOW
ranges or non-elliptical ephemerides are not emitted as assembled parameters.

Snapshots retain competing candidates, without consumer-side source selection.
Alert-marked records and test/text/authentication payloads are not candidates.
The following filters are separate from reception timeouts and signal health:

| Kind | Snapshot rule / `applicability` |
| --- | --- |
| `cnav_ephemeris` | Within ±1 hour of both toe and toc: `ORBIT_CLOCK_FIT` |
| `cnav_clock` | Within ±1 hour of toc: `CLOCK_FIT` |
| Reduced/Midi almanac, UTC, known-target time offset | Within ±72 hours of their resolved reference: `REFERENCE_WINDOW` |
| EOP | Match a source-local UTC candidate with equal teop/tot and top; use its reference GPST. Conflicting weeks or no match omit the EOP candidate. Within ±6 hours: `REFERENCE_WINDOW`; `within_fit_interval` additionally indicates ±1 hour |
| Group delay, ionosphere | `UNKNOWN`: no broadcast reference epoch establishing a validity interval |

The half-widths implement QPNT Table 4.3.1-2's explicitly doubled absolute-time
differences, not its nominal update cadence. EOP/UTC pairing follows §5.13.2.
`REFERENCE_WINDOW` does not certify health, orbit-reference availability or
applicability to every observation signal. No model is applied to observations.

Python snapshots aggregate compatible almanacs separately into
`cnav_reduced_almanac_entries`/`cnav_midi_almanac_entries` and corresponding
`*_sets` summaries. They follow the existing source-list/conflict contract,
never combine Reduced and Midi models, and do not claim complete membership.
Other snapshot catalog names match their MessageOutput kind. Untimed parameters
remain MessageOutput only; missing chronology is never invented.
