# Galileo broadcast decoding

The [broadcast API](broadcast-decode-realtime.md) consumes CommonNEX `GAL_INAV`,
`GAL_FNAV` and `GAL_CNAV`. It extracts parameters and assembles messages; it does
not authenticate navigation data or apply corrections to observations. Outputs
are derived products, not new CommonNEX catalogs.

The field definitions follow [Galileo OS SIS ICD 2.2](https://www.gsc-europa.eu/sites/default/files/sites/all/files/Galileo_OS_SIS_ICD_v2.2.pdf)
and [HAS SIS ICD 1.0](https://www.gsc-europa.eu/sites/default/files/sites/all/files/Galileo-HAS-SIS-ICD_in_force.pdf).
Reserved payloads are not invented into scientific parameters. Failed input
checks are rejected; I/NAV alert/horizontal pages remain unsupported.

## Shared representation

Records carry the existing station, broadcasting satellite, family, signal
contributors, nullable `nav_epoch_gpst` and output sequence. The input timestamp
remains the receiver navigation context, not a reconstructed RF reception time.

- Codes, indices, identifiers, masks and raw status fields are int64. Explicit
  predicates such as `crc_valid` are bool. Binary fragments are MSB-first bytes
  with zero padding at the end of a partial byte.
- Physical values use float64. Orbital angles/rates use radians and radians/s;
  harmonic angular corrections are radians, distances meters, `sqrt_a` sqrt(m).
- Reference times, offsets and delays use decimal128(38,12) seconds, with exact
  binary-rational to picosecond rounding, ties to even. Polynomial rates use
  float64 in s/s and s/s². Native TOW/week fields remain available.
- GST week zero maps to GPST week 1024. GPST coordinates use nominal GST/GPST
  alignment, with modulo weeks resolved against input context and independent
  TOE/TOC week carry. Broadcast GGTO is preserved, not applied as a precision
  clock correction to observations or these nominal coordinates.
- A passing CRC, matching issue and in-range reference time do not imply healthy
  signals. SHS/DVS, SISA and service status remain separate from assembly success.

## I/NAV and F/NAV

`gal_inav_word_N` and `gal_fnav_page_N` emit each decoded unit, including repeats.
Their `message_type` identifies the original word/page. The two I/NAV data slices
are joined into the standardized 128-bit word; parity/tails are not reintroduced.

| Units | Extracted content |
| --- | --- |
| I/NAV 1-4 | `iod_nav`; `toe_s`, `toc_s`; `m0_rad`, `eccentricity`, `sqrt_a`, `omega0_rad`, `i0_rad`, `omega_rad`, `idot_rad_s`, `omega_dot_rad_s`, `delta_n_rad_s`; `cuc_rad`, `cus_rad`, `crc_m`, `crs_m`, `cic_rad`, `cis_rad`; `sisa_index`; `svid`; `af0_s`, `af1_s_s`, `af2_s_s2`, according to the word layout |
| I/NAV 5 | Ionosphere, `bgd_e1_e5a_s`, `bgd_e1_e5b_s`, `e5b_health`, `e1b_health`, `e5b_data_invalid`, `e1b_data_invalid`, `week_raw`, `tow_s` |
| I/NAV 6 | UTC model and `tow_s` |
| I/NAV 7-10 | Almanac pieces, `iod_a`, reference week/TOW when present; word 10 also supplies GGTO |
| I/NAV 0 | `time_valid`, `week_raw`, `tow_s`; WN/TOW must not be used when the flag is false |
| I/NAV 16 | `gal_reduced_ced`: `delta_a_m`, `ex`, `ey`, `delta_i_rad`, `omega0_rad`, `lambda0_rad`, `af0_s`, `af1_s_s` |
| I/NAV 17-20 | `iod_nav_lsb` and 15 `parity_octets` for FEC2 |
| I/NAV 22 | `constellation_id`, ISM `payload`, independently verified `crc_valid`; recognized Galileo SL3 also produces `gal_ism_sl3` |
| F/NAV 1 | `svid`, `iod_nav`, clock polynomial, `sisa_index`, ionosphere, E1/E5a BGD, `e5a_health`, GST week/TOW and `e5a_data_invalid` |
| F/NAV 2-4 | Ephemeris pieces and `iod_nav`; pages 2/3 provide GST, page 4 provides UTC and GGTO |
| Type 63 | `gal_inav_dummy` or `gal_fnav_dummy`, without invented satellite parameters |

F/NAV pages 5/6 contain almanacs. In partial almanac unit records, `s1_`, `s2_`
and `s3_` prefix the subject slots, not broadcasting satellite identities.
Split F/NAV longitude bits are `s2_omega0_msb`/`s2_omega0_lsb` until assembled.

Semantic outputs shared by both navigation families:

| Kind | Fields and interpretation |
| --- | --- |
| `gal_ionosphere` | `ai0_sfu`, `ai1_sfu_degree`, `ai2_sfu_degree2`, `storm_flags`; five region flags in transmitted MSB-first order. Keep native sfu/degree polynomial units |
| `gal_utc` | `a0_s`, `a1_s_s`, signed integer `delta_tls_s`/`delta_tlsf_s`, `tot_s`, 8-bit `week_raw`/`wn_lsf_raw`, `dn`, nullable `reference_gpst` |
| `gal_ggto` | `a0g_s`, `a1g_s_s`, `t0g_s`, 6-bit `week_raw`, `available`, nullable `reference_gpst`. Sign is GST minus GPST. All four wire fields set to ones means unavailable |
| `gal_inav_ephemeris`, `gal_fnav_ephemeris` | Complete family-specific parameter set, original `week_raw`, resolved `toe_gpst`/`toc_gpst`/`transmission_gpst`, and `first_received_gpst` |
| `gal_inav_almanac`, `gal_fnav_almanac` | One `subject_sv_id`, `iod_a`, two-bit `week_raw`, `toa_s`, `reference_gpst`, `delta_sqrt_a`, normalized `sqrt_a`, `eccentricity`, `omega_rad`, `delta_i_rad`, normalized `i0_rad`, `omega0_rad`, `omega_dot_rad_s`, `m0_rad`, `af0_s`, `af1_s_s` and the family's SHS fields |

Almanac normalization uses nominal semi-major axis 29,600,000 m and inclination
56 degrees. Longitude follows the almanac's reference-time convention, not the
full ephemeris's weekly-epoch convention. Dummy subject zero emits no almanac.
Reduced CED remains a distinct parameter set: its clock-minus-radial-error terms
cannot be substituted into full CED. The importer navigation anchor does not
establish exact transmission start, so this decoder does not invent its reference
epoch or advertise a valid Reduced CED snapshot.

### Assembly and applicability

Assemblies are source-local, including signal contributor set and navigation
family. I/NAV requires fresh words 1-5; F/NAV requires fresh pages 1-4. Matching
IODnav, broadcasting SVID and consistent time references are required. Timeouts
are 90 seconds for I/NAV and 150 seconds for F/NAV, three nominal subframes.
These are reception bounds, not parameter validity claims. Completion clears the
pieces; repeated units do not extend the initial deadline.

Almanacs emit as soon as that subject's pieces and reference are present, without
waiting for every satellite. IODa/reference conflicts reset the pending pieces.
Almanac reception bounds are 180 seconds for I/NAV and 300 seconds for F/NAV.
Because IODa identifies a whole batch, matching IODa alone cannot identify the
three-subject group. Paired navigation contexts must be ordered and less than
30 seconds apart for I/NAV 7/8 and 9/10, 60 seconds for 8/9, or 100 seconds for
F/NAV 5/6. Ambiguous delayed combinations are not assembled.
Discontinuity clears all pending Galileo assemblies.

Ephemeris snapshots use `applicability=AGE_WINDOW` for reference ages in [0,4 h]
at both TOE and TOC. This is the decoder's temporal screening policy, not a
health certification or a mandatory consumer selection rule. Almanac, ionosphere,
UTC and available GGTO candidates retain `UNKNOWN` applicability. Python
aggregates compatible almanacs across broadcasters into separate
`gal_inav_almanac_entries`/`gal_fnav_almanac_entries` and `*_sets` summaries.
Conflicting candidates remain separate; no complete constellation is claimed.

### SAR, authentication and integrity support

For unambiguous E1-B input, `gal_osnma_fragment.payload` preserves the 40 OSNMA
bits. `gal_sar_fragment` contains `start`, `long_message` and the 20-bit `data`.
Consecutive compatible SAR fragments produce `gal_sar_rlm` with `beacon_id`
(60 bits), `message_code`, binary `parameters`, `parameter_bit_length` (16 or 96),
`long_message` and `first_received_gpst`. Repeated start bits delimit spare data;
missing continuity, a changed length or a context gap over three seconds clears
an incomplete RLM. Neither fragment output nor RLM assembly asserts authentication.
For E5b-only or ambiguous combined contributors, `gal_inav_auxiliary_bits.payload`
preserves the 64 non-navigation bits without assigning E1-specific semantics.

`gal_ism_sl3` retains the 12-bit `week_raw`, `t0_s`, `mask_msb`, `satellite_mask`,
indices `pconst_index`, `psat_index`, `ura_index`, `ure_index`, `bnom_index` and
`validity_index`. It also provides normalized `pconst`, `psat`, `ura_m`, `ure_m`,
`bnom_m`, `validity_s`, `reference_gpst` and `crc_valid`. Only passing ISM CRC and
the explicit reference/validity interval admit a `TIME_WINDOW` snapshot.
Other ISM constellations/service levels remain payloads. FEC2 parity extraction
does not yet recover missing CED words; OSNMA signature/key assembly and
authentication verification are not implemented.

## C/NAV and HAS

The decoder handles the standardized 486-bit C/NAV page and HAS MT1. It does not
interpret the 14 reserved leading bits as a navigation word. Dummy HAS pages
are counted and skipped. Outputs preserve `has_status`, `has_message_type`,
`message_id`, source identity and input navigation context.

| Kind | Contents |
| --- | --- |
| `gal_has_page` | `message_size_pages`, `page_id`, 53-byte `encoded_page` |
| `gal_has_message` | Reconstructed MT1 binary `payload`, `content_flags`, `toh_s`, resolved `reference_gpst`, `mask_id`, `iod_set_id`, `first_received_gpst` |
| `gal_has_mask` | Per-GNSS `subject_system`, `navigation_index`, `satellite_ids`, `signal_indices`, flattened row-major `cell_mask` |
| `gal_has_orbit` | Typed `entries`: subject system/SVID, navigation index, reference IOD, nullable `radial_m`, `along_track_m`, `cross_track_m` |
| `gal_has_clock_full`, `gal_has_clock_subset` | Typed `entries`: subject identity, nullable `iod_reference`, nullable `clock_m`, applied integer `multiplier`, `do_not_use` |
| `gal_has_code_bias` | Typed `entries`: subject identity, nullable reference IOD, `signal_index`, nullable `bias_m` |
| `gal_has_phase_bias` | Typed `entries`: subject identity, nullable reference IOD, `signal_index`, nullable `bias_cycles`, `discontinuity_indicator` |
| `gal_has_status` | Explicit don't-use service notification |

The correction blocks carry `validity_index` and nullable `valid_until_gpst`.
Signal indices are HAS Table 20 indices, interpreted with `subject_system`, not
receiver signal IDs or arbitrary RINEX observation codes. Navigation index zero
means GPS LNAV or Galileo I/NAV. Other indices remain explicit rather than being
guessed. Signs are the broadcast coefficient signs; no orbit/clock/bias
correction is applied. Unavailable numerical codes become null. The distinct
clock don't-use sentinel sets `do_not_use=true` and `clock_m=null`.

Reconstruction uses RS(255,32) erasure decoding from distinct page IDs, separately
per broadcaster/source and message ID. Size/status changes or contradictory
duplicate pages reset the assembly. It must finish within 150 seconds; completed
pieces are cleared. No CRC-failed page is repaired into accepted data.
Cross-broadcaster HAS page assembly is not implemented.

Masks and IOD associations are retained for less than 30 minutes. A message
without its mask still emits the reconstructed MT1 but no guessed corrections;
`has_missing_mask` counts it. Clock/bias records may have null reference IOD until
the matching orbit issue has been received. Malformed bodies do not install
partial mask/issue state. Reserved future content is not claimed as decoded.

HAS TOH resolves to the current or immediately preceding nominal GST hour, never
a future reference epoch. Snapshot blocks have `applicability=TIME_WINDOW` only
inside their explicit half-open validity interval. This does not certify mask/IOD
compatibility with a consumer's ephemeris or service fitness. HASS=0 (test) and
HASS=1 (operational) remain distinguishable. HASS=3 clears all retained HAS state
and HAS snapshot candidates; HASS=2 is reserved and not assembled. Consumers
remain responsible for service selection and correction application.

## Backend use and bounds

Realtime STEC uses this same I/NAV/F/NAV parameter decoder through an RTKLIB
adapter, not a second bit-field decoder. It retains its own availability, health
and orbit-selection policy. HAS, Reduced CED, SAR, OSNMA and ARAIM outputs do not
implicitly enable new STEC calibration or positioning behavior.

Use the common API's bounded batches and candidate limits. HAS additionally
allows at most 128 source contexts, 32 pending message IDs per source and at most
32 pages per assembly. Missing time permits direct unit/fragment output but no
timed assembly or candidate insertion. No input file, day or batch boundary is
an implicit reset; explicit discontinuities are.
