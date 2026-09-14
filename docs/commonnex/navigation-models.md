# Additional DecodedNav models

Status: selected design draft, not implemented adapters or verified full
protocol coverage. [DecodedNav](decoded-nav.md) defines common headers,
tagged unions, `NavigationTime`, and GPS LNAV/Galileo EPH.

The inventories below follow [RINEX 4.02](https://files.igs.org/pub/data/format/rinex_4.02.pdf),
Tables A10-A11, A19-A28 and A33-A37. Scientific units and semantics, not ASCII
field positions, define CommonNEX. A question mark denotes nullability.
All model-specific reserved codes, source revisions, assembly rules, sign
mappings and numerical bounds must still be checked against the applicable
ICD before implementing an adapter. Shared field shapes are not shared physics.

## Shared EPH rules

Reuse established orbit/clock names and units where their meanings agree.
Required model coefficients and reference times must form a coherent data set.
Incomplete raw assembly remains RawBits; complete unhealthy ephemerides remain
storable. Optional diagnostics do not gate storage. Never synthesize missing
coefficients, issue numbers, health, or cross-family bias values. Issue codes
are not globally unique record IDs or automatic overwrite keys.

EPH normally partitions by native TOC's nominal GPST day; SBAS instead uses its
shared `reference_time`. Model-specific requirements supersede a blanket rule
requiring GPS-style issue identifiers. Native times and extra GPST coordinates
use `NavigationTime`; model coefficients are not silently reparameterized.

### Dynamic orbit fields

GPS/QZSS CNAV and CNAV-2 and BeiDou CNV1/2/3 use `sqrt_a0: float64` (sqrt(m)),
`a_dot_m_per_s: float64`, `delta_n0_rad_per_s: float64`, and
`delta_n0_dot_rad_per_s2: float64`, alongside eccentricity, reference angles,
angular rates and six harmonic corrections defined in GPS LNAV. Do not also
store a derivable semi-major axis. These field names identify reference values
and rates, not permission to use one constellation's propagation constants
for another. Clock coefficients retain `af0_s: TimeDelta`,
`af1_s_per_s: float64`, and `af2_s_per_s2: float64`.

## BeiDou D1/D2 EPH

Use `C / D1|D2`. Share a parameter structure but retain message type: D1 is
MEO/IGSO legacy navigation and D2 is GEO legacy navigation. Do not guess MEO
versus IGSO from PRN. Use the GPS LNAV orbit/clock field inventory with native
BDT `toe`, `toc`, and optional `transmission_time`, replacing GPS-specific
issue, health, delay and L2 fields with:

| Field | Type | Meaning |
| --- | --- | --- |
| `aode`, `aodc` | `uint8` | Original age codes, 0-31; not seconds or GPS IODE/IODC |
| `sat_h1` | `bool?` | Native autonomous health flag; true means unhealthy |
| `tgd1_b1_b3_s`, `tgd2_b2_b3_s` | `TimeDelta?` | Signal-specific group delays |
| `urai` | `uint8?` | Directly supplied accuracy index |
| `sv_accuracy_m` | `float64?` | Source or standard-mapped nominal accuracy |

Retain discrete age codes rather than converting them into a precise
`Duration`. AODE/AODC are required for this model; special accuracy meanings
must not become ordinary standard deviations.

## BeiDou CNV1/CNV2/CNV3 EPH

Use separate `C / CNV1`, `C / CNV2`, and `C / CNV3` branches. Reuse the dynamic
orbit and clock fields, native BDT TOE/TOC, optional transmission time, and:

| Field | Type | Meaning |
| --- | --- | --- |
| `sat_type` | `uint8` | Native code: 0 reserved, 1 GEO, 2 IGSO, 3 MEO |
| `top` | `NavigationTime?` | Model's t_op reference |
| `sisai_oe`, `sisai_ocb`, `sisai_oc1`, `sisai_oc2`, `sismai` | integer? | Separate native accuracy/integrity indicators; signedness and exact widths require field-specific mapping |
| `health_code` | `uint8?` | Two-bit native health code |

Do not finalize all SISAI fields as unsigned: RINEX examples include negative
values. Preserve valid signed indicators and distinguish source missing-value
sentinels using the applicable ICD, not a generic zero/negative filter.
These indicators are not one interchangeable `accuracy_m` or `stddev`.

| Branch | Delay fields (`TimeDelta?`) | Integrity scopes | Issue fields |
| --- | --- | --- | --- |
| CNV1 | `isc_b1cd_s`, `tgd_b1cp_s`, `tgd_b2ap_s` | B1C | `iode: uint8`, `iodc: uint16` |
| CNV2 | `isc_b2ad_s`, `tgd_b1cp_s`, `tgd_b2ap_s` | B2a and B1C | `iode: uint8`, `iodc: uint16` |
| CNV3 | `tgd_b2bi_s` | B2b | No invented IODE/IODC |

Integrity uses applicable nullable `b1c`, `b2a`, or `b2b` structs containing
`dif: bool?`, `sif: bool?`, `aif: bool?`, preserving native polarity.
Unprovided scopes are absent, not false. CNV3 here is MEO/IGSO B2b navigation,
not an indiscriminate container for every B2b service payload.
CNV1/2 must establish their orbit/clock issue consistency; CNV3 uses its own
time/message assembly rules. Do not reject CNV3 for lacking GPS-style issues.

## GPS CNAV and CNAV-2 EPH

Use separate `G / CNAV` and `G / CNV2` branches with dynamic orbit/clock fields.
Native time is GPST (`identity`). Keep `toe` and `toc`; when importing the
single RINEX epoch, populate both using the model's standard equality, not
guesswork. `top: NavigationTime?` is distinct and must not replace TOE.
Resolve the source wn_op when constructing TOP. No LNAV IODE/IODC is invented.

| Field | Type |
| --- | --- |
| `urai_ed`, `urai_ned0` | `int8?` |
| `urai_ned1`, `urai_ned2` | `uint8?` |
| `tgd_s`, `isc_l1ca_s`, `isc_l2c_s`, `isc_l5i5_s`, `isc_l5q5_s` | `TimeDelta?` |
| CNV2 only: `isc_l1cd_s`, `isc_l1cp_s` | `TimeDelta?` |

CNAV health uses `l1_unhealthy`, `l2_unhealthy`, `l5_unhealthy` (`bool?`);
CNV2 uses `l1c_unhealthy: bool?`. CNAV flags are
`integrity_status_flag`, `l2c_phasing_flag`, `alert_flag` (`bool?`);
the RINEX CNV2 model supplies `integrity_status_flag: bool?`.
Do not equate L2C phasing with observation half-cycle ambiguity or combine
integrity/alert into health. Keep exact code ranges and native polarity in
adapter definitions, not inferred from integer container width.

## QZSS EPH

Keep `J / LNAV`, `J / CNAV`, and `J / CNV2` as separate branches, sharing
numerical field definitions with the corresponding GPS models but not their
entire decoding rules. Preserve QZSS native broadcast time identity; interpret
RINEX time fields according to source version instead of relabeling solely
because the satellite system is J. The time-scale registry must resolve this
source representation distinction.

LNAV retains `iode: uint8`, `iodc: uint16`, `sv_health_bits: uint8?`,
`ephemeris_status_bits: uint8?`, `tgd_s: TimeDelta?`,
`fit_interval_flag: bool?`, `ura_index: uint8?`, and `sv_accuracy_m: float64?`.
The ephemeris-status location was called Code on L2 in older definitions;
interpret it by protocol/ICD revision, not blindly as GPS `codes_on_l2`.
The RINEX fixed L2P flag is a format constant, not receiver tracking evidence.
Fit flag 0 means two hours and 1 means more than two hours: retain the flag,
not a guessed exact `fit_interval_s`.

CNAV/CNV2 reuse the corresponding dynamic orbit, accuracy, health and named
delay field structures. Their flags differ from GPS:

| Flag (`bool?`) | CNAV | CNV2 |
| --- | --- | --- |
| `integrity_status_flag` | Present when supplied | Present when supplied |
| `ephemeris_status_flag` | Present when supplied | Present when supplied |
| `alert_flag` | Present when supplied | Not supplied by this RINEX record |

The CNAV bit corresponding to GPS L2C phasing means QZSS ephemeris status.
Check do-not-use TGD/ISC bit patterns before scaling and map them, and their
RINEX blanks, to null rather than a plausible negative delay. Missing optional
delays do not invalidate complete orbit/clock parameters. Other QZSS services
are not implicitly covered by these EPH branches.

## SBAS EPH

Use `S / SBAS`, a Cartesian model of the SBAS satellite itself based on MT9,
with optional MT17 health. This is neither other satellites' corrections nor
the MT18/26 grid, and MT17 almanac is not a substitute for MT9 ephemeris.

| Field | Type | Meaning |
| --- | --- | --- |
| `satellite_number` | `uint16` | Common satellite identity, not mixed RINEX/air-interface numbering |
| `reference_time` | `NavigationTime` | Shared orbit/clock reference |
| `transmission_time` | `NavigationTime?` | Known transmission start, not receiver epoch |
| `x_m`, `y_m`, `z_m` | `float64` | Earth-fixed position |
| `vx_m_per_s`, `vy_m_per_s`, `vz_m_per_s` | `float64` | Velocity |
| `ax_m_per_s2`, `ay_m_per_s2`, `az_m_per_s2` | `float64` | Acceleration |
| `agf0_s` | `TimeDelta` | Clock bias coefficient |
| `agf1_s_per_s` | `float64` | Relative frequency bias coefficient |
| `iodn` | `uint8` | Navigation issue |
| `ura_index` | `uint8?` | Original index |
| `sv_accuracy_m` | `float64?` | Available nominal accuracy |
| `ranging_status` | enum | `available`, `not_available`, `unknown`, specifically URA-level availability |
| `mt17_health_bits` | `uint8?` | Known four-bit MT17 health, otherwise null |

Convert RINEX km-based vectors to m-based units. Define the terrestrial frame
and propagation conventions in the model mapping before implementation.
Do not add a fictitious af2 or duplicate TOE/TOC. RINEX SBAS reference time is
GPST; raw-source SBAS time semantics must be explicitly mapped, not assumed
identity for every service. Resolve RINEX PRN-minus-100 numbering consistently
with Core (for example S29 versus air-interface PRN 129).

Split RINEX combined health into MT17 availability/health and URA status.
URA index 15 / RINEX 32767 maps to `not_available` with null meter accuracy,
not a numeric standard deviation. `available` is not overall healthy status.
Require complete MT9 vectors, clock coefficients, reference time and IODN;
do not wait for optional MT17. Any attached MT17 health needs justified
satellite/time association and actual derivation references.

## STO: system time offsets

Use a shared structure with explicit direction and evaluation time scale.
The broadcasting system/satellite is independent of the two compared systems.

| Field | Type | Meaning |
| --- | --- | --- |
| `from_time_system`, `to_time_system` | enum | Difference direction: from minus to |
| `reference_time` | `NavigationTime` | Polynomial epoch; native scale defines its independent variable |
| `transmission_time` | `NavigationTime?` | Known transmission time |
| `a0_s` | `TimeDelta` | Constant coefficient |
| `a1_s_per_s` | `float64` | Linear coefficient |
| `a2_s_per_s2` | `float64?` | Quadratic coefficient when the model defines it |
| `utc_realization` | `string?` | Required applicable UTC proxy/realization |
| `sbas_system` | `string?` | Applicable SBAS system identity |

The polynomial describes the standard offset component, not subtraction of
seconds measured from unrelated native origins. Origins, fixed relationships,
and leap seconds are handled separately. Direction normalization must preserve
the correct independent time scale and coefficient signs; it does not apply a
clock correction to observations. A known first-order model has null a2;
an explicitly supplied quadratic zero remains zero. Missing a required term
does not make a complete evaluable model.

Keep leap-second declarations separate from a0. The selected logical content
is current offset, optional future offset and optional effective time with
explicit scope; exact field types and transition encoding remain to be defined.
Missing leap data does not prevent storing STO, but may prevent UTC conversion.
No UTC processing/partition mode is introduced.

STO's own GPST coordinate defaults to nominal alignment (identity for GPST),
not self-referential correction by the same STO. Other records may reference
it for broadcast-model conversion. Partition by nominal GPST reference day;
do not assume a universal day-long validity or extrapolate without a model rule.

## EOP: Earth orientation parameters

Store broadcast models separately in meaning from external precise ERP/EOP
products. The broadcasting satellite is optional and does not limit applicability
to that satellite. Use this shared structure with model-specific requirements:

| Field | Type |
| --- | --- |
| `reference_time` | `NavigationTime` |
| `transmission_time` | `NavigationTime?` |
| `xp_rad`, `yp_rad` | `float64` |
| `xp_rate_rad_per_s`, `yp_rate_rad_per_s` | `float64?` |
| `xp_accel_rad_per_s2`, `yp_accel_rad_per_s2` | `float64?` |
| `ut1_reference_system` | enum: `UTC` or `GPST` |
| `delta_ut1_s` | `TimeDelta` |
| `delta_ut1_rate_s_per_s` | `float64?` |
| `delta_ut1_accel_s_per_s2` | `float64?` |

The difference is UT1 minus the declared reference system. Resolve that
reference using constellation/ICD semantics, not magnitude heuristics. Do not
silently convert UT1-UTC into UT1-GPST. Convert arcseconds and per-day rates
to radians and per-second units, with a unit day of 86400 seconds.
Acceleration fields are true second derivatives; adapters must distinguish
them from quadratic coefficients (evaluation uses one-half acceleration times
elapsed time squared).

Null higher-order terms are allowed when the model does not provide them, not
as a universal instruction to substitute zero. RINEX can use zero for unavailable
parameters: map to null only where model/source evidence establishes absence;
otherwise preserve the value without claiming an observed zero. Detailed
availability mapping remains to be specified. Require reference time, pole
coordinates, UT1 offset/basis and any model-required rates. Unresolved UT1 basis
is unsupported mapping. Partition by nominal GPST reference day.

## ION: broadcast ionosphere models

Use a tagged union of `Klobuchar`, `NeQuickG`, and `BDGIM`. These are model
inputs, not evaluated STEC/VTEC or SBAS grid data. System/message/subtype still
selects the algorithm: identical coefficient shapes do not justify applying
GPS constants or algorithms to every constellation.

| Model | Required coefficients | Types and native units |
| --- | --- | --- |
| Klobuchar | `alpha0`-`alpha3`, `beta0`-`beta3` | alpha0/beta0: signed `TimeDelta`; remaining terms: `float64`, s/semicircle^n |
| NeQuick-G | `ai0_sfu`, `ai1_sfu_per_deg`, `ai2_sfu_per_deg2` | `float64` |
| BDGIM | `alpha1_tecu`-`alpha9_tecu` | `float64` |

Keep model-native semicircle/degree/sfu units: changing coefficient units also
changes the algorithm's independent variable. Klobuchar beta0 is a coefficient,
not a necessarily nonnegative Duration. Preserve QZSS WIDE/JAPN subtypes rather
than merging them or inferring them from station position. NeQuick-G adds
`disturbance_flags: uint8?`, range 0-31, with bit 4 for region 1 through bit 0
for region 5. Unknown is null, not zero. These are regional flags, not a single
model-valid switch. BDGIM coefficients in TECU are not nine spatial grid values.

ION's RINEX 4 epoch is transmission time, not a made-up TOE/TOC. Store known
`transmission_time: NavigationTime?`; otherwise require reliable acquisition
navigation-epoch time context. Partition by transmission time's nominal GPST day
when known, otherwise by the acquisition epoch's GPST day, retaining which
association is present. Acquisition is not transmission or a validity interval.

If neither time basis exists, skip the model and report/count the exclusion.
This includes complete untimed RINEX header coefficients. Do not derive a time
from filenames, neighboring ephemerides, filesystem dates or import time, and
do not introduce collection-level untimed DecodedNav files. Preserve other
usable records and the raw archive. This is an explicit limitation of the
conditional RINEX preservation goal, not an unknown-time Observation exception.

Require all model coefficients; do not merge incomplete sets across satellites
or reuse old terms to fill gaps. All-zero sets require source-defined
interpretation, not automatic deletion. Do not invent a day-long validity.
Repeated broadcasts remain distinct RawBits occurrences; DecodedNav deduplication
is not required.
