# Receiver-to-CommonNEX mappings

[Receiver configuration profiles](receiver-profiles.md) | [Importer](importer.md)

UBX/SBF wire parsing belongs to libcppgnss. Observatory measurement normalization,
RawBits repacking/classification, time association and Arrow export belong to
libneognss-obs. The contracts below describe retained scientific meaning, not
an alternative implementation walkthrough. Protocol revisions select adapters;
CommonNEX readers do not decode source envelopes.

## Observation mappings

RAWX version 1 and MeasEpoch revisions 0/1 supply observations; MeasExtra
revisions 0-3 add companion information. Unknown signal mappings are counted,
not guessed. GLONASS/NavIC are excluded. Meas3 observations remain unsupported.

## Time and completion contracts

UBX observation epochs use RAWX measurement time. TIMEGPS supplies navigation
context and is not allowed to overwrite it. EOE closes its associated navigation
epoch; a complete RAWX independently completes its measurement record.
The canonical navigation timestamp includes TIMEGPS fTOW, without truncating
sub-millisecond precision. EOE matches the original iTOW token, then emits
completion at that TIMEGPS report's full timestamp, including across resume.
This preserves reported precision, not precise RawBits reception time.

SBF measurement epochs use the MeasEpoch/MeasExtra/EndOfMeas association below.
RawBits stores associated navigation time directly,
not given an additional transmission-time column in the RawBits family.
The current SBF importer anchors it to the latest valid PVTCartesian (4006),
PVTGeodetic (4007), ReceiverTime (5914), or EndOfPVT (5921) in stream order.
Include at least one of these when recording RawBits. MeasEpoch timestamps do
not substitute for independent navigation context. RawBits block SIS timestamps
are neither retained nor used as fallback; complete SIS time need not be
recoverable from an individual canonical body. Unknown/expired context emits
null-time RawBits; invalid navigation anchors disable the current context.
UBX EOE does not reset the RawBits anchor. Both protocols follow the
[receiver-time association policy](receiver-time.md).

## Observation quality mapping

### MeasExtra uncertainty bounds

Use separate `quality.*_stddev_is_lower_bound` fields for code, phase and Doppler.
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
Use the finite nonnegative DopplerVarFactor only to derive Doppler stddev;
the normalized record does not store the factor.
Revision 0 lacks CumLossCont/CarMPCorr; revision 3 adds CN0HighRes and extended
signal IDs. Reserved bits and padding are not scientific fields. Decode N as
modulo 256 using actual block/sub-block lengths, not as a plain loop bound.

### Source mappings

These mappings are implemented for the supported source revisions above.

| Source | `cn0_db_hz` | `half_cycle_ambiguity` | `half_cycle_subtracted` |
| --- | --- | --- | --- |
| UBX RAWX | `cno`, dB-Hz | Reverse the defined `halfCyc` validity indication for applicable observations | `subHalfCyc` |
| SBF Measurements | Decoded `MeasEpoch.CN0`, augmented by applicable MeasExtra high-resolution bits | Type1/Type2 `ObsInfo` bit 2 | null |

SBF base C/N0 has 0.25 dB-Hz resolution and signal-dependent decoding;
MeasExtra adds `CN0HighRes * 0.03125` dB-Hz. The 0-7 high-resolution value
is a fractional extension, not a strength rank. UBX NAV-SAT/NAV-SIG `qualityInd`
0-7 is tracking status, not C/N0 or a direct RAWX validity replacement.

Missing half-cycle subtraction information remains null, not false. A lock
reset does not prove a half-cycle subtraction. A set subtraction flag reports
an already-performed receiver operation; preserve the exported phase.

Source variances become common standard deviations after unit/sentinel decoding.
Preserve direct standard deviations without inventing values for absent fields.
Companion-block association must use matching epoch, antenna and signal context;
missing MeasExtra does not discard usable MeasEpoch observables. Additional
quality remains null. Preserve association across physical file boundaries.

SBF signal number 38 (QZSS L1C/B) maps to RINEX signal identity `J1E`,
distinct from L1 C/A (`J1C`) and L1C (`J1L`). This mapping also applies to
MeasExtra companion association; the carrier frequency remains 1575.42 MHz.

## Telemetry mappings

### Navigation-window association

UBX NAV-PVT is the trigger. Pending MON-SYS and RAWX clock evidence belong to
the following PVT cycle. NAV-TIMEGPS with matching iTOW supplies full GPST,
including fTOW; RAWX retains its own independent timestamp. TIM-TP belongs to
the report cycle but retains its own target-pulse time. SBF PVT Cartesian and
Geodetic with the same TOW/WNc are one epoch. Source-timed pre-PVT reports wait
for their window; ReceiverStatus after EndOfPVT still belongs to that epoch.

Repeated MON-SYS without intervening PVT preserves older unassigned status in
a partial row. FineTime is used internally to validate SBF status time, not
stored as an independent telemetry field.

### Clock, status and adjustment quantities

UBX NAV-CLOCK supplies bias (ns), drift (ns/s), tAcc (ns), fAcc (ps/s).
SBF PVT supplies bias (ms), drift (ppm) and TimeSystem; Error != 0 makes its
estimates unavailable. MON-SYS supplies temperature, uptime and utilization;
SBF ReceiverStatus supplies temperature (raw minus 100; zero is unavailable),
uptime and CPU load. CPU averaging windows may differ between receivers.
Fine-time status remains an importer validity check, not a stored field.
Missing data is never substituted with zero.


SBF MeasEpoch revision 1 supplies CumClkJumps with modulus 256. UBX RAWX
supplies only the reset flag; both counter fields remain null. A declared modulus describes source wrapping, not the integer container width.
The common counter/validity semantics are in [receiver telemetry](receiver-telemetry.md).

### Pulse timing

Pulse error means actual edge minus ideal edge, positive late. SBF Offset is
ns; negative means early. SyncAge=255 is capped; receiver-time mode's zero is
not GNSS-lock evidence. UBX qErr maps as -qErr picoseconds; qErrInvalid makes
the numeric error null without discarding other pulse fields. TIM-TP describes
the next pulse. Only locked GPST-based TIM-TP currently has resolved target GPST;
UTC/GST/BDT targets remain null without a supported conversion. Zero qErr alone
is not invalid. UBX sign follows the previously adopted F9T experiment and is
not independently verified on this station's hardware.

## RawBits mapping

### Coverage

SBF mappings below accept revision 0 and validate the documented container
length, satellite domain and block-specific Source values. UBX accepts SFRBX
version 2. Other revisions/signals are counted, never guessed from payload size.

| Family | SBF blocks | UBX source | Canonical format | Evidence |
| --- | --- | --- | --- | --- |
| GPS/QZSS LNAV | 4017 / 4066 | GPS/QZSS L1 C/A | `LNAV_300_V1` | Actual UBX/SBF |
| GPS/QZSS CNAV | 4018,4019 / 4067,4068 | GPS/QZSS L2C and L5-I | `CNAV_300_V1` | Actual UBX/SBF |
| Galileo I/NAV | 4023 | E1-B, E5b-I | `INAV_228_V1` | Actual UBX/SBF |
| Galileo F/NAV | 4022 | E5a-I | `FNAV_238_V1` | Actual SBF; UBX documentary mapping |
| Galileo C/NAV | 4024 | Not mapped | `CNAV_PAGE_486_V1` | Actual SBF |
| BeiDou D1/D2 | 4047, documented satellite assignment | Explicit D1/D2 sigId | `D1D2_300_V1` | Actual UBX/SBF |
| BeiDou B-CNAV1 | 4218 | Not mapped | `BCNAV1_1800_V1` | Actual SBF |
| BeiDou B-CNAV2 | 4219 | Not mapped | `BCNAV2_576_V1` | Actual SBF |
| BeiDou B2b, B-CNAV3 / PPP-B2b / unclassified | 4242 | Not mapped | `B2B_984_V1` | Actual SBF; guarded type routing |
| SBAS L1 | 4020 | SBAS L1 | `SBAS_L1_250_V1` | Actual UBX/SBF |
| SBAS L5 | 4021 | Not mapped | `SBAS_L5_250_V1` | Actual SBF |
| GPS/QZSS CNAV-2 | 4221 / 4227 | Not mapped | `CNAV2_1800_V1` | Documentary; constructed containers only |
| QZSS L1S | 4228 | QZSS L1S | `QZS_L1S_250_V1` | Actual UBX; documentary SBF |
| QZSS L5S | 4246 | Not mapped | `QZS_L5S_250_V1` | Documentary; constructed containers only |
| QZSS L6, unclassified service | 4069,4270,4271 | Not mapped | `L6_2000_V1` | Documentary; constructed containers only |

Actual receiver evidence is scoped to the listed mapping and check regions,
not exhaustive firmware support or every content subtype. Documentary mappings
have source-definition and constructed-container checks, not RF validation.
Missing samples remain explicit in [TODO](TODO.md).

### Receiver packing

Read each exported U4 as a little-endian integer, then concatenate integers
most-significant bit first to form `W`. It is not the wire-byte string.

| Format | UBX exported words | SBF exported words |
| --- | --- | --- |
| LNAV_300_V1 | Low 30 bits of each of ten words | Low 30 bits of each of ten words |
| CNAV_300_V1 | W[0:300] | W[0:300] |
| INAV_228_V1 | W[0:114] + W[128:242] | W[0:228] |
| FNAV_238_V1 | W[0:238], documentary | W[0:238] |
| CNAV_PAGE_486_V1 | Unsupported | W[0:486] |
| D1D2_300_V1 | Low 30 bits of each of ten words | W[0:300] |
| BCNAV1_1800_V1 | Unsupported | W[0:1800] |
| BCNAV2_576_V1 | Unsupported | W[0:576] |
| B2B_984_V1 | Unsupported | W[0:984] |
| SBAS_L1_250_V1 | W[0:250] | W[0:250] |
| SBAS_L5_250_V1 | Unsupported | W[0:250] |
| CNAV2_1800_V1 | Unsupported | W[0:1800], documentary |
| QZS_L1S_250_V1 | W[0:250] | W[0:250], documentary |
| QZS_L5S_250_V1 | Unsupported | W[0:250], documentary |
| L6_2000_V1 | Unsupported | W[0:2000], documentary |

Remove only specified tails/container padding; do not repeat receiver inversion,
deinterleaving or FEC repair. Equal word counts do not prove equivalent packing.
UBX has no SFRBX payload timestamp. SBF RawBits SIS timestamps are ignored for
association and storage; use the shared [receiver-time contract](receiver-time.md).

## Unclassified semantics

- SBF `BDSRaw` maps C01-C05 and C59-C63 to `BDS_D2`, and C06-C58 to
  `BDS_D1`, following the GEO versus MEO/IGSO ranging-code assignments in
  B1I ICD 3.0 and B3I ICD 1.0. Unknown assignments retain
  `BDS_D1D2_UNCLASSIFIED`; do not infer a subtype from reception cadence.
  Both use unchanged `D1D2_300_V1` bodies. Family identity and BCH/parity
  validity are independent: failed checks remain attached to classified bits.
  UBX sigId explicitly distinguishes D1/D2 and is mapped accordingly.
- B2b routing requires both receiver and independent message CRC success and
  agreement between the body prefix PRN and SBF satellite identity. Under the
  July 2020 ICD assignments, types 10/30/40 select `BDS_BCNAV3`, while 1-7/63
  select `BDS_PPP_B2B`. All other cases retain `BDS_B2B_UNCLASSIFIED`.
  CRC alone, reserved prefix values or PRN ranges never select the service.
  No classifier state is shared across occurrences; body/checks are unchanged.
  This is supported-type routing, not full content validation or a PPP decoder.
- `QZS_L6_UNCLASSIFIED` preserves full 2000-bit messages, including RS parity.
  L6D/L6E contributors are retained when known; neither implies CLAS/MADOCA.
  Legacy 4069 Source 0 means unknown (empty contributors), 1 L6D, 2 L6E.

These are explicit legal families, not missing identity or opaque UBX/SBF blobs.
Satellite identity remains mandatory. All repeated occurrences remain distinct.
Identity follows the Observation RINEX domain: `G/E/C/J/S`; SBAS PRN 137 is
`S37`, QZSS PRN 193 is `J01`. Grid and all SBAS plotting products retain the
same S-number identity; they do not convert back to broadcast PRNs.

## Checks and interpretation

LNAV uses `parity/normalized_word_chain`, with possible preceding-word states
rather than assuming zero history. BeiDou legacy uses BCH(15,11) with the
documented normalized packing. Neither independent check replaces receiver
validity: their scopes differ and real receiver results can disagree.

CRC checks retain their defined scopes: CNAV/SBAS and B2a/B2b message regions,
Galileo page regions, and separate SF2/SF3 regions for B1C and CNAV-2.
No independent LDPC, CNAV-2 SF1 BCH, or L6 RS syndrome success is claimed.
L6 keeps the reported RS result and corrected-symbol count.
L5S retains the receiver's check only: the documented 250-bit container alone
does not establish an independent checksum algorithm for every service mode.

I/NAV preserves the receiver's even/odd pair with both tails removed. The
fixed 228-bit container is retained even if page discriminator bits are corrupt;
it does not assert that an alert transmission actually occurred. The CRC check
covers the supplied canonical `[0:220]` pair. Alert horizontal assembly across
independent records is not implemented, nor is missing content synthesized.

Receiver channel and nullable source diagnostics (`sbf_viterbi_count`,
`sbf_rs_corrected_symbols`) are retained only when applicable. RS counts are
symbols, not bit errors. Fields reserved or inapplicable in a block are null.
Receiver checks use `source_field` evidence; transport CRC never becomes a
navigation validity assertion.

GALRawCNAV uses the documented 12-byte payload prefix.
UBX SBAS and QZSS L1S may carry a ninth container word: only the documented
250-bit body is retained, without assuming that trailing word is always zero.

## Sources

- [Canonical slices and primary ICD references](raw-bits-formats.md).
- [Septentrio mosaic-X5 4.15 reference guide](https://docs.sparkfun.com/SparkFun_GNSS_mosaic-X5/assets/component_documentation/firmware/mosaic-X5_Firmware_v4.15.0_Reference_Guide.pdf).
- [Septentrio mosaic-G5 1.0.1 reference guide](https://docs.sparkfun.com/SparkFun_GNSS_mosaic-G5_P3/assets/component_documentation/firmware/v1.0.1/mosaic-G5%20Firmware%20v1.0.1%20Reference%20Guide.pdf), CNAV-2 and QZSS L1S/L5S/L6D/L6E.
- [Septentrio PolaRx5 reference guide, mirror](https://www.gnss-imu.com/down/upload/20220923/1663940448.pdf), legacy QZSRawL6 4069.
- [Galileo OS SIS ICD 2.2](https://www.gsc-europa.eu/sites/default/files/sites/all/files/Galileo_OS_SIS_ICD_v2.2.pdf), nominal/alert page structure.
