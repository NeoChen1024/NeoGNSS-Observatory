# Implemented RawBits adapters

`libcppgnss::decode_raw_bits` performs protocol-specific repacking and scoped
integrity checks. Observatory code associates UBX navigation time and exports
Arrow batches; Python writes the `raw-bits` catalog with Zstandard level 3.
RawBits-only inputs are valid. Neither observations nor an ephemeris solution
are required. No navigation correction or FEC repair is performed during import.

## Coverage

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
| BeiDou D1/D2 | 4047, unclassified subtype | Explicit D1/D2 sigId | `D1D2_300_V1` | Actual UBX/SBF |
| BeiDou B-CNAV1 | 4218 | Not mapped | `BCNAV1_1800_V1` | Actual SBF |
| BeiDou B-CNAV2 | 4219 | Not mapped | `BCNAV2_576_V1` | Actual SBF |
| BeiDou B2b, B-CNAV3 / PPP-B2b / unclassified | 4242 | Not mapped | `B2B_984_V1` | Actual SBF; guarded type routing |
| SBAS L1 | 4020 | SBAS L1 | `SBAS_L1_250_V1` | Actual UBX/SBF |
| SBAS L5 | 4021 | Not mapped | `SBAS_L5_250_V1` | Actual SBF |
| GPS/QZSS CNAV-2 | 4221 / 4227 | Not mapped | `CNAV2_1800_V1` | Documentary; constructed containers only |
| QZSS L1S | 4228 | QZSS L1S | `QZS_L1S_250_V1` | Actual UBX; documentary SBF |
| QZSS L5S | 4246 | Not mapped | `QZS_L5S_250_V1` | Documentary; constructed containers only |
| QZSS L6, unclassified service | 4069,4270,4271 | Not mapped | `L6_2000_V1` | Documentary; constructed containers only |

Current real-data checks cover Era A/B samples, a complete Era C SBF day and
the complete BEE0 2026-09-13 recording; B2b/QZSS CNAV classification additionally
uses the complete BEE0 2026-09-16/17 recordings. They establish these receiver mappings,
not full archive coverage or exhaustive receiver/firmware support. Constructed
inputs check bit order, padding, lengths, routing and Arrow output; they are
not RF evidence. Galileo QP/restricted signals without an agreed receiver
representation remain outside this implementation. GLONASS/NavIC are excluded.

## Unclassified semantics

- `BDS_D1D2_UNCLASSIFIED` retains SBF legacy bits when no explicit subtype is
  supplied. UBX sigId explicitly distinguishes D1/D2 and is mapped accordingly.
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

The GALRawCNAV adapter reads the official 12-byte prefix directly.
UBX SBAS and QZSS L1S may carry a ninth container word: only the documented
250-bit body is retained, without assuming that trailing word is always zero.

## Time and downstream scope

UBX uses valid NAV-TIMEGPS week/iTOW/fTOW (full reported navigation precision); SBF uses
valid receiver TOW/WNc from PVTCartesian (4006), PVTGeodetic (4007),
ReceiverTime (5914), or EndOfPVT (5921). Fix type is not time validity.
RawBits arriving with a usable anchor is emitted immediately with that anchor's
`nav_epoch_gpst`. NAV-EOE only closes a matching navigation completion Event;
it does not flush RawBits or clear its anchor. SFRBX may arrive after EOE.

### Anchor timeout and missing time

Use the shared [receiver-time policy](telemetry-time.md): 10-period freshness,
nullable navigation GPST, uptime association and restart boundaries. Emit complete
RawBits immediately even without time. There is no time-waiting backlog or
retroactive timestamp filling. Unknown-time records stay in the last known GPST
directory, or `1980/01/06/` before any known date; their timestamp remains null.
File, chunk and GPST-day boundaries do not reset receiver context.

RawBits block SIS timestamps are not
stored, used for partitioning, or used as a fallback. They need not be recoverable
from the canonical body alone. An individual RawBits block does not create a
navigation completion event. Canonical payloads remain unchanged, including
any erroneous broadcast time bits. Receiver-navigation time reversals still fail.

`ngo-sbas-grid-parquet` selects only `SBAS_L1` from mixed RawBits. Other families,
including SBAS L5 and QZSS L1S/L5S, do not enter the L1 grid decoder.

## Sources

- [Canonical slices and primary ICD references](raw-bits-layouts.md).
- [Septentrio mosaic-X5 4.15 reference guide](https://docs.sparkfun.com/SparkFun_GNSS_mosaic-X5/assets/component_documentation/firmware/mosaic-X5_Firmware_v4.15.0_Reference_Guide.pdf).
- [Septentrio mosaic-G5 1.0.1 reference guide](https://docs.sparkfun.com/SparkFun_GNSS_mosaic-G5_P3/assets/component_documentation/firmware/v1.0.1/mosaic-G5%20Firmware%20v1.0.1%20Reference%20Guide.pdf), CNAV-2 and QZSS L1S/L5S/L6D/L6E.
- [Septentrio PolaRx5 reference guide, mirror](https://www.gnss-imu.com/down/upload/20220923/1663940448.pdf), legacy QZSRawL6 4069.
- [Galileo OS SIS ICD 2.2](https://www.gsc-europa.eu/sites/default/files/sites/all/files/Galileo_OS_SIS_ICD_v2.2.pdf), nominal/alert page structure.
