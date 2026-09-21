# RawBits signal and family registry

Status: v0 registry design, cross-checked against the primary sources below.
Names are CommonNEX identifiers, not quotations of receiver enum names.
This registry does not implement an importer or certify receiver support.
See [layouts](raw-bits-layouts.md) for exact retained bits and check scopes.

## Scope and interpretation

This catalog covers the open navigation/augmentation families surveyed for
GPS, Galileo, BeiDou, QZSS and SBAS. It is not an inventory of every RF signal,
restricted service, pilot, future message or RINEX observation code. GLONASS
and NavIC remain excluded. Observation signal codes retain their separate
system-specific RINEX representation.

Signal identity, semantic family and canonical unpacking format are orthogonal.
Frequency or length alone selects none of them. A listed family does not prove
that a given receiver exports it. Source protocol revisions and discriminator
values are importer concerns, not canonical row fields or reader dependencies.

## Broadcast signal identifiers

Each `bitstream_source` list element is one of these case-sensitive identifiers.
The names retain the agreed coarse granularity where no component suffix was
selected. In particular, `GPS_L2C` does not assert the receiver reported CM/CL
separately, even though the ICD defines the data-bearing component. Do not add
pilot entries as contributors merely because a receiver tracks them.

| Identifier | Standard signal / component | Navigation semantics | Source |
| --- | --- | --- | --- |
| `GPS_L1_CA` | L1 C/A | `GPS_LNAV` | G200 |
| `GPS_L2C` | L2C | `GPS_CNAV` | G200 |
| `GPS_L5_I` | L5 I5 data | `GPS_CNAV` | G705 |
| `GPS_L1C` | L1C, coarse signal identity | `GPS_CNAV2` | G800 |
| `GAL_E1_B` | E1-B data | `GAL_INAV` | GAL |
| `GAL_E5A_I` | E5a-I data | `GAL_FNAV` | GAL |
| `GAL_E5B_I` | E5b-I data | `GAL_INAV` | GAL |
| `GAL_E6_B` | E6-B data | `GAL_CNAV` | HAS |
| `BDS_B1I` | B1I | `BDS_D1` or `BDS_D2` | BLEG |
| `BDS_B2I` | B2I | `BDS_D1` or `BDS_D2` | BLEG |
| `BDS_B3I` | B3I | `BDS_D1` or `BDS_D2` | B3 |
| `BDS_B1C` | B1C, coarse signal identity | `BDS_BCNAV1` | B1C |
| `BDS_B2A` | B2a, coarse signal identity | `BDS_BCNAV2` | B2A |
| `BDS_B2B` | B2b, coarse signal identity | `BDS_BCNAV3` or `BDS_PPP_B2B`; routing must be justified | B2B, PPP |
| `QZS_L1_CA` | L1C/A | `QZS_LNAV` | QPNT |
| `QZS_L1_CB` | L1C/B, distinct from L1C | `QZS_LNAV` | QPNT |
| `QZS_L2C` | L2C | `QZS_CNAV` | QPNT |
| `QZS_L5_I` | L5 I5 data | `QZS_CNAV` | QPNT |
| `QZS_L1C` | L1C, coarse signal identity | `QZS_CNAV2` | QPNT |
| `QZS_L1S` | L1S | `QZS_L1S` | QL1S |
| `QZS_L5S` | L5S; operating mode must be respected | `QZS_L5S` | QTV |
| `QZS_L6D` | L6D | `QZS_L6_UNCLASSIFIED` until service is established | QSERV |
| `QZS_L6E` | L6E | `QZS_L6_UNCLASSIFIED` until service is established | QSERV |
| `SBAS_L1` | SBAS L1 | `SBAS_L1` | SBAS, SBF |
| `SBAS_L5` | SBAS L5 | `SBAS_L5` | SBAS |

The suffix `I` in B1I/B2I/B3I is part of the standard signal name; it is not
an interchangeable alias for B1C/B2a/B2b. QZSS L1C/B carries LNAV and must not
be mapped to L1C CNAV-2. QZSS-hosted SBAS transmissions retain their SBAS signal
and family interpretation; spacecraft operator alone does not select QZS_L1S.
Satellite/PRN normalization is a separate importer mapping, not this registry.

For combined Galileo I/NAV, `["GAL_E1_B", "GAL_E5B_I"]` denotes known
contributors without ordered half-page assignments. Do not emit both a coarse
and a refined identifier for one contributor. Native composite tracking names
do not license adding pilot components to the bitstream-source list.

## Legal family / unpacking-format pairs

`CHECKED` means the retained layout/check scopes have actual receiver evidence
described in raw-bits-layouts.md, not every content subtype or both protocols.
`DOCUMENTED` means the candidate is supported by source definitions but receiver
normalization still needs validation. Only independently supported importer
mappings may emit records, regardless of this registry status.

| `message_family` | `body_format` | Bits | Status / limitation |
| --- | --- | ---: | --- |
| `GPS_LNAV` | `LNAV_300_V1` | 300 | CHECKED |
| `QZS_LNAV` | `LNAV_300_V1` | 300 | CHECKED in SBF |
| `GPS_CNAV` | `CNAV_300_V1` | 300 | CHECKED |
| `QZS_CNAV` | `CNAV_300_V1` | 300 | CHECKED in SBF L2C/L5 |
| `GAL_INAV` | `INAV_228_V1` | 228 | CHECKED nominal pages only |
| `GAL_FNAV` | `FNAV_238_V1` | 238 | CHECKED in SBF; UBX still unverified |
| `GAL_CNAV` | `CNAV_PAGE_486_V1` | 486 | CHECKED in SBF |
| `BDS_D1` | `D1D2_300_V1` | 300 | CHECKED common BCH packing; D1 identification must be justified |
| `BDS_D2` | `D1D2_300_V1` | 300 | CHECKED common BCH packing; D2 identification must be justified |
| `BDS_BCNAV1` | `BCNAV1_1800_V1` | 1800 | CHECKED SF2/SF3 CRC regions, not complete BCH/LDPC validation |
| `BDS_BCNAV2` | `BCNAV2_576_V1` | 576 | CHECKED systematic-region CRC |
| `BDS_BCNAV3` | `B2B_984_V1` | 984 | CHECKED SBF; guarded routing for types 10/30/40 |
| `BDS_PPP_B2B` | `B2B_984_V1` | 984 | CHECKED SBF types 1-5/63; 6/7 documentary routing |
| `SBAS_L1` | `SBAS_L1_250_V1` | 250 | CHECKED |
| `SBAS_L5` | `SBAS_L5_250_V1` | 250 | CHECKED in full SBF recording |
| `GPS_CNAV2` | `CNAV2_1800_V1` | 1800 | DOCUMENTED; no current sample |
| `QZS_CNAV2` | `CNAV2_1800_V1` | 1800 | DOCUMENTED common framing/FEC dimensions; no current sample |
| `BDS_D1D2_UNCLASSIFIED` | `D1D2_300_V1` | 300 | CHECKED layout; neither source subtype nor documented satellite assignment establishes D1/D2 |
| `BDS_B2B_UNCLASSIFIED` | `B2B_984_V1` | 984 | CHECKED layout; service unresolved |
| `QZS_L1S` | `QZS_L1S_250_V1` | 250 | CHECKED UBX; DOCUMENTED SBF |
| `QZS_L5S` | `QZS_L5S_250_V1` | 250 | DOCUMENTED; not SBAS L5 |
| `QZS_L6_UNCLASSIFIED` | `L6_2000_V1` | 2000 | DOCUMENTED; source may identify L6D/L6E without identifying service |

The table fixes names for reviewed layouts, not the full content-decoder field
catalogs. Format unpackers expose structural regions and check scopes; family
decoders interpret semantic fields, time scales, units and message types. Shared
GPS/QZSS packing does not assert shared ephemeris constants or time semantics.
`CNAV_PAGE_486_V1` is distinct from `CNAV_300_V1`; length coincidence with any
other CRC region is not a format equivalence. No system prefix is required.

The two B2b families share an outer unpacker, not a correction/ephemeris decoder.
The retained 984-bit region is a 12-bit prefix followed by the 972-bit coded
message; the systematic 486-bit region ends in CRC. The service's subsequent
field interpretation differs. A CRC pass alone cannot select the family.
The implemented supported-type classifier additionally requires receiver CRC
success, prefix PRN agreement and an assigned type under the July 2020 ICDs;
see the [routing contract](raw-bits-layouts.md#b2b-open-service-versus-ppp-b2b).
Use `BDS_B2B_UNCLASSIFIED` for unresolved/reserved/failed-check records.
The formal July 2020 ICDs also define identical LDPC matrices. Their prefix
semantics and systematic data fields differ; see the
[B2b comparison](raw-bits-layouts.md#b2b-open-service-versus-ppp-b2b) for exact
slices, defined message types and service-identification limits.

`CNAV2_1800_V1` denotes the receiver-normalized sequence of 52 SF1 symbols,
1200 SF2 symbols and 548 SF3 symbols. The latter two contain respectively
600 and 274 systematic bits including CRC. This is NOT
`BCNAV1_1800_V1` (72 + 1200 + 528). Receiver deinterleaving/ordering must be
documented by the receiver before enabling an adapter; equal dimensions alone
do not establish correctness. The implemented documentary adapters follow the
Septentrio deinterleaved 1800-symbol definition; no sample validation is claimed.

## Reserved families and unfinished extensions

`QZS_L1S` and `QZS_L5S` have distinct approved 250-bit container formats,
not aliases of SBAS formats. Their standard and verification modes must not
be collapsed into the SBAS L5 family solely by frequency or data rate.

L6D/L6E are signal names, not an unconditional CLAS/MADOCA service selector.
Retain the documented full 2000 bits including RS parity as `L6_2000_V1`, with
`QZS_L6_UNCLASSIFIED` until the service is established. L6D/L6E remain signal
contributors, not service names.

Galileo OS SIS ICD 2.2 also introduces E5a-QP, and the G2 technical note describes
future QP/data evolution. These are not evidence of a new I/NAV/F/NAV mapping
in current receiver recordings. Keep their identity/body definitions TODO until
the intended exported content and receiver mapping are studied. Likewise,
pilots/restricted signals are not implicitly enabled as RawBits sources by their
appearance in a general RF or RINEX signal table.

## Validation performed and limits

This registry review cross-checked the published sources and the existing
full-file BEE0 validation results; it did not rerun that recording or claim new
RF captures. The retained records include GPS/QZSS legacy and CNAV, Galileo
I/F/C-NAV, BeiDou legacy/B1C/B2a/B2b and SBAS L1/L5. No CNAV-2 or QZSS
L1S/L5S/L6 receiver mapping is promoted to sample-validated by this review.
The record of exact bit/check evidence remains raw-bits-layouts.md.

The formal July 2020 B2b and PPP-B2b ICDs were compared directly, including
all 324 nonzero matrix positions and 324 coefficients. Their FEC definitions
are identical; independent receiver-codeword syndrome checks and actual
service routing remain unverified. Documentary matrix equality is not a
claim that every receiver-exported parity bit has been validated.
Do not infer source revision coverage from an ICD publication date.

## Progress

- [x] Enumerate signals covering the reviewed open-navigation input families.
- [x] Name legal family/format pairs for the checked layouts and distinguish
  documented CNAV-2 candidates from validated receiver mappings.
- [x] Verify L1C/B versus L1C, Galileo data components, and CNAV versus B-CNAV
  naming distinctions against primary sources.
- [x] Confirm shared B2b/PPP-B2b framing and FEC against both formal 1.0 ICDs,
  retaining distinct content semantics and prefix interpretations.
- [x] Permit explicit bounded unclassified D1/D2, B2b and L6 families when
  receiver identity does not establish the semantic subtype/service.
- [ ] Add further evidence-based subtype/service classification where useful.
- [ ] Validate reserved QZSS service mappings and Galileo QP extensions when
  relevant receiver output and adequate specifications are available.
- [x] Implement registry validation in importers, keeping source revision
  coverage outside CommonNEX reader requirements.

See [implemented coverage](raw-bits-importer.md) for current mappings and the
distinction between actual-recording checks and documentary/synthetic checks.

## Primary references

| Key | Document and relevant sections |
| --- | --- |
| G200 | [IS-GPS-200N](https://www.gps.gov/sites/default/files/2025-07/IS-GPS-200N.pdf), signal modulation and LNAV/CNAV sections 3, 20, 30 |
| G705 | [IS-GPS-705J](https://www.gps.gov/sites/default/files/2025-07/IS-GPS-705J.pdf), L5 I5 navigation and CNAV structure |
| G800 | [IS-GPS-800J](https://www.gps.gov/sites/default/files/2025-07/IS-GPS-800J.pdf), L1C and subframes 1/2/3 |
| GAL | [Galileo OS SIS ICD 2.2](https://www.gsc-europa.eu/sites/default/files/sites/all/files/Galileo_OS_SIS_ICD_v2.2.pdf), sections 2 and 4 |
| HAS | [Galileo HAS SIS ICD](https://www.gsc-europa.eu/sites/default/files/sites/all/files/Galileo_HAS_SIS_ICD_v1.0.pdf), E6-B C/NAV page |
| BLEG | [CSNO BDS open-service ICD, official CNSA copy](https://www.cnsa.gov.cn/n6758823/n6758839/c6796160/part/6776242.pdf), B1I/B2I D1/D2 |
| B3 | [B3I ICD](https://en.beidou.gov.cn/SYSTEMS/ICD/201806/P020180608516798097666.pdf), legacy navigation and BCH |
| B1I3 | [B1I ICD 3.0](http://en.beidou.gov.cn/SYSTEMS/ICD/201902/P020190227702348791891.pdf), section 4.3 ranging-code assignments and section 5.1.1 D1/D2 classification |
| B1C | [B1C ICD 1.0](https://en.beidou.gov.cn/SYSTEMS/ICD/201806/P020180608519640359959.pdf), section 6 |
| B2A | [B2a ICD 1.0](https://en.beidou.gov.cn/SYSTEMS/ICD/201806/P020180608518432765621.pdf), B-CNAV2 |
| B2B | CSNO, **BDS-SIS-ICD-B2b-1.0**, July 2020, formal Chinese edition, sections 6 and 7.2; supplied PDF `P020200803362056878157.pdf` |
| PPP | CSNO, **BDS-SIS-ICD-PPP-B2b-1.0**, July 2020, formal Chinese edition, sections 6.1-6.2.1; supplied PDF `P020200803362060731204.pdf` |
| QPNT | [IS-QZSS-PNT-006](https://qzss.go.jp/en/technical/download/pdf/ps-is-qzss/is-qzss-pnt-006.pdf), sections 3 and 4, GPS/QZSS semantic differences |
| QL1S | [QZSS SLAS signal specification overview](https://qzss.go.jp/technical/system/l1s.html) |
| QTV | [QZSS positioning technology verification signal overview](https://qzss.go.jp/technical/system/tv.html), standard/verification modes |
| QSERV | [QZSS interface specification catalog](https://qzss.go.jp/en/technical/ps-is-qzss/ps-is-qzss.html), separate SLAS/CLAS/MADOCA/DC/PTV services |
| SBAS | [ICAO DFMC SBAS SARPs Part B](https://www.icao.int/sites/default/files/sp-files/airnavigation/Documents/NSP5_Report%20on%20Agenda%20Item%202.APPENDIX%20A2%20-%20DFMC%20SBAS%20SARPS%20Part%20B.pdf), L1/L5 distinctions and L5 CRC |
| SBF | [Septentrio X5 reference guide](https://docs.sparkfun.com/SparkFun_GNSS_mosaic-X5/assets/component_documentation/firmware/mosaic-X5_Firmware_v4.15.0_Reference_Guide.pdf), raw-navigation block definitions |

Future signal reference: [Galileo G2 QP evolution note](https://www.gsc-europa.eu/sites/default/files/sites/all/files/G2_Evolution_of_Quasi-Pilot_Signals_and_Interface_Control_Information.pdf).
