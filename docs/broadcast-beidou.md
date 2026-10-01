# BeiDou broadcast decoding

The [broadcast API](broadcast-decode-realtime.md) consumes canonical CommonNEX
RawBits. These outputs are derived parameter records, not CommonNEX catalogs.
They preserve broadcaster, family and contributing signals. No correction is
applied to observations, and modern parameters are not reduced to a legacy
RTKLIB orbit model.

## Coverage

| Family | Implemented MessageOutput |
| --- | --- |
| D1 | SF1-3 ephemeris assembly; SF1 Klobuchar; per-satellite almanacs, health, UTC and GPS/Galileo time offsets |
| D2 | SF1 pages 1, 3-10 ephemeris assembly; page 2 Klobuchar; SF5 almanacs, health, UTC and GPS/Galileo time offsets |
| B-CNAV1 | SF2 complete orbit/clock/group delay; SF3 pages 1-4 integrity, SISAI, BDGIM, UTC, EOP, BGTO and midi/reduced almanacs |
| B-CNAV2 | Types 10, 11, 30-34, 40; independently decoded orbit/clock/model fields and assembled ephemeris |
| B-CNAV3 | Types 10, 30, 40; orbit/clock, integrity, BDGIM, UTC, EOP, BGTO and midi/reduced almanacs |
| PPP-B2b | Types 1-7 masks, orbit, code bias, clock and URA; type 63 status/null information |

Output names start with `bds_d1_`, `bds_d2_`, `bds_bcnav1_`, `bds_bcnav2_`,
`bds_bcnav3_` or `bds_pppb2b_`. Reserved/unsupported payloads are counted, not
invented. A legacy `header` output does not imply that the rest of that page has
been decoded. Unclassified B2b/D1D2 input remains unsupported by this decoder.

## Types, units and time

Integer fields and indices are int64; physical quantities are float64 unless
listed otherwise. Availability is bool; transmitted flag codes are int64. Lists preserve field order, and
missing quantities are null. Timestamps, native time coordinates and clock
offsets in seconds use decimal128(38,12). Binary-scaled clock offsets are rounded
to picoseconds, ties to even; polynomial rates remain float64.

`nav_epoch_gpst` is reception context, never replaced by broadcast SOW. BDT
reference epochs convert with the 1,356-week origin difference and GPST = BDT +
14 seconds. Truncated weeks are resolved against known reception context, not
host time. TOE/TOC are resolved on the nearest side of the broadcast week
boundary. Without sufficient context, resolved times are null; numeric parameter
extraction is still possible. Broadcast UTC/BGTO models are preserved, not
applied to CommonNEX timestamps.

### Orbit and clock

Legacy ephemerides expose `sqrt_a` (sqrt(m)), `eccentricity`, `m0_rad`,
`omega_rad`, `omega0_rad`, `i0_rad`, `delta_n_rad_s`, `omega_dot_rad_s`,
`idot_rad_s`, `cuc_rad`, `cus_rad`, `cic_rad`, `cis_rad`, `crc_m`, `crs_m`,
`toe_bdt_s`, `toc_bdt_s`, `toe_gpst`, `toc_gpst`, `transmission_gpst`, `week_raw`,
`af0_s`, `af1_s_s`, `af2_s_s2`, `aode`, `aodc`, `urai`, `health`,
`tgd_b1i_s` and `tgd_b2i_s`. AODE/AODC are broadcast data-age codes, not invented
monotonic version counters. `health` is SatH1, and URAI retains its ICD index.
The realtime STEC adapter shares this decoder, then uses RTKLIB for propagation.

Modern ephemerides use the same angular/harmonic and clock units, but replace
`sqrt_a` with `satellite_type`, `delta_a_m`, `a_dot_m_s`, and also include
`delta_n_dot_rad_s2`. Satellite type 1/2/3 means GEO/IGSO/MEO; 0 is reserved.
The reference semi-major axis is 42,162,200 m for GEO/IGSO and 27,906,100 m for
MEO. Consumers must retain this modern time-varying orbit model.
B-CNAV1/2 retain broadcast IODE/IODC. B-CNAV3 has neither and does not synthesize
them. Group delay/ISC fields identify B1Cp, B1Cd, B2ap, B2ad or B2bI explicitly;
their reference is the B3I satellite clock convention, not a generic DCB.

D1 collects fresh SF1-3; D2 collects fresh SF1 pages 1 and 3-10. A new first
fragment starts a new cycle. Assembly requires contiguous broadcast SOW steps
(6 s for D1, 3 s between D2 pages, including the omitted page 2), equal TOE/TOC
and at most 90 s reception-context span. Completed state is cleared.
B-CNAV1 SF2 is self-contained and requires IODE = low eight bits of IODC.
B-CNAV2 requires fresh type 10/11 (successive 3 s SOW), a matching clock IODC,
equal TOE/TOC and the same 90 s assembly bound. B-CNAV3 pairs fresh type 10/30
with equal TOE/TOC within that bound. This is conservative assembly, not a
statement that all BeiDou services broadcast every model within 90 seconds.
No assembly crosses a declared discontinuity, source or navigation family.

### System models and almanacs

- `klobuchar`: `alpha` and `beta` float64 lists retain the ICD's seconds and
  semicircle powers.
- `bdgim`: nine `alpha_tecu` coefficients, including the negative unsigned
  fifth coefficient. No BDGIM evaluation is performed.
- `utc`: `a0_s`, `a1_s_s`, optional modern `a2_s_s2`, leap-second counts,
  reference week/time and future leap day. Legacy UTC has native SOW but no
  manufactured reference week.
- `bgto`: modern GNSS ID, native reference epoch, `a0_s`, `a1_s_s`, `a2_s_s2`,
  and `available`; ID 0 is unavailable. Legacy outputs separately name GPS and
  Galileo coefficients. Do not apply the modern model to legacy coefficients.
- `eop`: native `teop_bdt_s`, polar motion in arcseconds, drift in arcseconds/day,
  UT1-minus-UTC in decimal seconds and its float64 seconds/day rate.
- `sisai`/`status`/`integrity`: ICD indices and flags, not inferred sigmas.
  B-CNAV2 type 10 has no health field and emits a distinct `integrity` record.
- Legacy `almanac`, modern `midi_almanac` and `reduced_almanac` emit one meaningful
  subject slot per occurrence, never wait for a constellation-wide set.
  `subject_sv_id` identifies that satellite, not the broadcaster. Dummy PRN/type
  slots are omitted. Legacy expanded slots require a recently received AmEpID=3
  declaration and valid AmID; they are not guessed from page number alone.

Legacy almanacs preserve `delta_i_rad` relative to 0 for GEO or 0.30 semicircles
for MEO/IGSO. Modern midi almanacs use 0 for GEO or 55 degrees for MEO/IGSO.
Reduced almanacs retain `delta_a_m`, `omega0_rad`, `phi0_rad` and `health`; the
same modern semi-major-axis references apply. They are not complete ephemerides.
Legacy reference week/toa association requires matching toa and source-local
health-page context no older than 2,250 s. Unresolved entries are MessageOutput
only. Compatible timed almanacs aggregate across broadcasters only in snapshots.

## PPP-B2b corrections

Only the ICD-assigned C01-C05/C59-C63 ranging codes are accepted. A PPP family
label on another broadcaster is rejected with a diagnostic, even when its CRC
passes. Such messages are not parsed using a speculative PPP layout.

The outer prefix is not CRC protected. `service_available` reflects its status
bit, not an authenticated assertion. Unavailable service clears that source's
snapshot candidates; decoded contents remain observable in MessageOutput.
Malformed variable-length messages do not publish partially decoded corrections.

Each correction has `epoch_bdt_sod_s`, nullable `epoch_gpst`, `iod_ssr`,
`valid_until_gpst`, `message_type` and `service_available`. Resolve seconds of
BDT day to the nearest day around reception, not to the GPST date blindly.
No correction value changes the reception timestamp.

| Kind | Additional fields |
| --- | --- |
| `mask` | `iodp`, ordered `slots: list<int64>` for all set bits, including excluded systems needed to preserve compact indexing |
| `orbit` | `entries`: slot, IODN, IOD Corr, radial/along/cross corrections in m, URAI |
| `code_bias` | `entries`: slot, ICD signal/tracking index, bias in m |
| `clock_mask` | IODP; `entries`: one-based compact mask ordinal, null direct slot, IOD Corr, C0 in m |
| `clock_direct` | `entries`: direct slot, null mask ordinal, IOD Corr, C0 in m |
| `ura` | IODP; `entries`: one-based compact mask ordinal and URAI |

Slots 1-63 are BeiDou, 64-100 GPS, 101-137 Galileo. Direct entries outside these
supported systems are omitted. Compact-mask entries remain ordinal and require
the exact matching mask, including all its set bits. Signal indices use PPP-B2b
ICD table 6-5, not UBX/SBF signal IDs. The minimum signed orbit/clock code lies
outside the specified range and becomes null. Code biases retain their entire
signed 12-bit range; no undocumented sentinel is imposed. URAI=0 is unknown/unreliable; 63 is an upper-range
indicator, not a numerical sigma. Types 6/7 emit separate clock and orbit records
with their own epochs. Mask/SSR/navigation-IOD/correction-IOD pairing and actual
correction application are deliberately consumer responsibilities.

Snapshots retain source-specific candidate corrections within their ICD nominal
windows: orbit/URA 96 s, clock 12 s, code bias 86,400 s, all half-open from the
correction epoch. `TIME_WINDOW_IOD_MATCH_REQUIRED` expressly does not certify
IOD consistency with a consumer's chosen orbit. Masks have no ICD fixed lifetime.

Navigation ephemeris snapshots currently use a conservative implementation
selection window of one hour either side of TOE and TOC, labeled `AGE_WINDOW`.
This is not an ICD accuracy guarantee or a health decision. Other model snapshots
remain `UNKNOWN`; seven-day retention is a resource bound, not scientific
validity. Consumers still select source, health policy and propagation model.

## Remaining scope

- [ ] Decode D2 regional differential/integrity and grid-ionosphere payloads.
- [ ] Complete expanded almanac health pages and other reserved/newer service
  pages only after their definitions have been verified.
- [ ] Independently validate/decode B-CNAV1 SF1 BCH; its encoded bits are not
  treated as an unencoded PRN/SOH field.
- [ ] Add backend support for modern BeiDou orbit/rate models and optional
  PPP-B2b mask/IOD resolution/application. Parameter output does not promise a
  corresponding RTKLIB backend.
- [ ] Refine navigation model-specific snapshot applicability beyond the
  explicitly qualified selection policy above.

## Sources

CSNO [B1I ICD 3.0](http://en.beidou.gov.cn/SYSTEMS/ICD/201902/P020190227702348791891.pdf),
sections 5.2-5.3; [B1C ICD 1.0](http://en.beidou.gov.cn/SYSTEMS/ICD/201806/P020180608519640359959.pdf),
sections 6-7; [B2a ICD 1.0](http://en.beidou.gov.cn/SYSTEMS/ICD/201806/P020180608518432765621.pdf),
sections 6-7; BDS-SIS-ICD-B2b-1.0 (July 2020), sections 6-7; and
BDS-SIS-ICD-PPP-B2b-1.0 (July 2020), sections 6-7. The latter two editions are
identified in the [RawBits reference table](commonnex/raw-bits-formats.md#primary-references).
