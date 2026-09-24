# RawBits formats and signal registry

[Record schema](raw-bits.md) | [Receiver mappings and coverage](receiver-mappings.md#rawbits-mapping)

This document owns canonical identifiers and body interpretation, not source
protocol support. All bit slices are zero-based, end-exclusive and refer to the
MSB-first canonical body. Retained FEC does not imply independent codeword validation.
No partial/fragment layouts are currently defined.

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

| `message_family` | `body_format` | Bits |
| --- | --- | ---: |
| `GPS_LNAV` | `LNAV_300_V1` | 300 |
| `QZS_LNAV` | `LNAV_300_V1` | 300 |
| `GPS_CNAV` | `CNAV_300_V1` | 300 |
| `QZS_CNAV` | `CNAV_300_V1` | 300 |
| `GAL_INAV` | `INAV_228_V1` | 228 |
| `GAL_FNAV` | `FNAV_238_V1` | 238 |
| `GAL_CNAV` | `CNAV_PAGE_486_V1` | 486 |
| `BDS_D1` | `D1D2_300_V1` | 300 |
| `BDS_D2` | `D1D2_300_V1` | 300 |
| `BDS_BCNAV1` | `BCNAV1_1800_V1` | 1800 |
| `BDS_BCNAV2` | `BCNAV2_576_V1` | 576 |
| `BDS_BCNAV3` | `B2B_984_V1` | 984 |
| `BDS_PPP_B2B` | `B2B_984_V1` | 984 |
| `SBAS_L1` | `SBAS_L1_250_V1` | 250 |
| `SBAS_L5` | `SBAS_L5_250_V1` | 250 |
| `GPS_CNAV2` | `CNAV2_1800_V1` | 1800 |
| `QZS_CNAV2` | `CNAV2_1800_V1` | 1800 |
| `BDS_D1D2_UNCLASSIFIED` | `D1D2_300_V1` | 300 |
| `BDS_B2B_UNCLASSIFIED` | `B2B_984_V1` | 984 |
| `QZS_L1S` | `QZS_L1S_250_V1` | 250 |
| `QZS_L5S` | `QZS_L5S_250_V1` | 250 |
| `QZS_L6_UNCLASSIFIED` | `L6_2000_V1` | 2000 |

`message_family` selects content semantics; `body_format` selects the canonical
unpacker. Shared layouts do not imply shared ephemeris constants or timescales.
`_V1` is a layout identifier, not receiver firmware or a daily-file revision.
Every table entry denotes a complete container, including documentary mappings
whose real-receiver validation is still pending in the coverage table.

LNAV and D1/D2 use `unit_kind=subframe`; Galileo pages use `page`; all other
listed formats use `message`. B-CNAV1/2, B2b, CNAV-2 and L6 use
`content_kind=binary_symbols`; the remaining formats use `navigation_bits`.

## Canonical layouts

### GPS and QZSS LNAV

The body concatenates ten 30-bit words, each containing receiver-deinverted
24 data bits and six normalized parity bits. Receiver container bits are absent.

Independent parity checks must use the normalized parity convention, not an
unmodified over-the-air parity checker. The first word needs separate handling
of unavailable preceding-word context; subsequent words can use the preceding
word's relevant parity-bit relationship. Record such a check as
`parity/normalized_word_chain`, distinct from receiver output-policy evidence.
Observed UBX chain exceptions and receiver-failed SBF frames that pass a
limited independent check prohibit equating these checks. Never repair bits
merely to make the chain pass.

### GPS and QZSS CNAV

The body has 300 contiguous bits. The last 24 body bits are CRC;
the check covers the preceding 276 bits. This is not a CNAV-2 layout.

QZSS L2C/L5 retain `QZS_CNAV`, including type 0 (test/default message),
type 60 (QZNMA, L5 only) and type 61 (regional ionosphere/clock/ISC).
These are content types within CNAV, not separate body formats or service
families. Preserve failed-check and reserved-type occurrences too; family
identification from the receiver container does not assert usable navigation.
IS-QZSS-PNT-006 sections 4.3.1/4.3.2 define these distinctions.

### Galileo pages

| Family | Canonical bits | Independent CRC scope |
| --- | ---: | --- |
| I/NAV nominal | 228 | `[0:196]` with CRC `[196:220]` |
| F/NAV | 238 | `[0:214]` with CRC `[214:238]` |
| C/NAV | 486 | `[0:462]` with CRC `[462:486]` |

I/NAV removes both six-bit tails and receiver padding. Preserve its final
eight non-tail bits even though they are outside this CRC scope. Preserve
combined E1/E5b contributor information; do not invent per-half signal identity.
The receiver's fixed even/odd pair is preserved even when page-type bits are
corrupt. This does not establish a genuine alert transmission. Pair CRC checks
do not assemble horizontal alert pages from separate received records.

F/NAV retains 214 information bits plus CRC. C/NAV retains a 14-bit header,
448-bit HAS field and CRC. SBF exports a further six-bit tail for each;
exclude it from canonical storage. HAS message assembly belongs to the
decoder; RawBits stores each page independently.

### BeiDou legacy

The 300-bit canonical body is already deinterleaved; do not deinterleave again.

Within the first 30-bit word, `[15:30]` forms a BCH(15,11) codeword. For each
subsequent word `w`, the two BCH codewords are `w[0:11] + w[22:26]` and
`w[11:22] + w[26:30]`. The generator is `x^4 + x + 1`.
Keep all 300 bits, not just the data portions. Independent BCH success does
not replace receiver validity or establish all header/time semantics.

### BeiDou coded regions

| Family | Canonical body | Information including CRC | Other retained content |
| --- | --- | --- | --- |
| B1C | `[0:1800]` | SF2 `[72:672]`; SF3 `[1272:1536]` | SF1 `[0:72]`; SF2 parity `[672:1272]`; SF3 parity `[1536:1800]` |
| B2a | `[0:576]` | `[0:288]` | Parity `[288:576]` |
| B2b | `[0:984]` | `[12:498]` | Prefix `[0:12]`; parity `[498:984]` |

Each information slice ends with its own 24-bit CRC. B1C requires separate
`crc/sf2` and `crc/sf3` results: their validity can differ. Independent SF1 BCH
and LDPC codeword checks are not yet validated. B2a/B2b preambles are excluded
by the receiver and must not be synthesized. B2b's 12-bit prefix is retained
but is not part of the checked 486-bit information-plus-CRC region.

### B2b open service versus PPP-B2b

The formal July 2020 ICDs, `BDS-SIS-ICD-B2b-1.0` sections 6 and 7.2
(printed pages 11-18), and `BDS-SIS-ICD-PPP-B2b-1.0` sections 6.1-6.2.1
(printed pages 9-12), establish a shared `B2B_984_V1` unpacker, not a shared
navigation-content decoder. See the registry's B2B and PPP references.

Both transmitted frames contain 1000 binary symbols over one second:
16 synchronization symbols (`0xEB90`, MSB first), 6 PRN symbols, 6 prefix
symbols and 972 coded symbols. The receiver omits the synchronization word;
it is not inserted into the canonical body.

| Canonical slice | B-CNAV3 open service | PPP-B2b |
| --- | --- | --- |
| `[0:6]` | Broadcasting PRN | Broadcasting PRN |
| `[6:12]` | Reserved | Service-status prefix: bit 6 is 1 for unavailable, 0 for available; bits `[7:12]` reserved |
| `[12:18]` | Message type | Message type |
| `[18:38]` | BDT SOW | Part of the type-dependent data, not a common SOW |
| `[38:474]` | 436 data bits | Remainder of the 456-bit type-dependent data starting at bit 18 |
| `[474:498]` | CRC-24 | CRC-24 |
| `[498:984]` | LDPC parity | LDPC parity |

For both families, CRC protects `[12:474]`, with the CRC-24Q polynomial
`x^24 + x^23 + x^18 + x^17 + x^14 + x^11 + x^10 + x^7 + x^6 + x^5 + x^4 + x^3 + x + 1`.
The prefix, including PRN and PPP availability, is outside both the navigation
CRC and the LDPC codeword. A passing check does not validate that prefix or
identify the service. Availability is interpreted only after identifying PPP-B2b.

Both ICDs define GF(2^6) LDPC(162,81), using `1 + x + x^6`, six bits per
field symbol, MSB-first vector representation, and 81 systematic symbols
followed by 81 parity symbols. Thus the dimensions describe 486 information
bits including CRC and 486 parity bits, not 162 binary bits. The ICDs define
identical parity-check matrices; independent receiver
codeword validation is not implied.

The open-service ICD covers MEO/IGSO B-CNAV3 and defines types 10, 30 and 40;
type 0 is invalid and all others are reserved (table 7-1). The PPP ICD
describes GEO broadcasts and defines types 1-7 and 63 (null information),
with 8-62 reserved (table 6-1); that table does not assign type 0.
See [receiver routing](receiver-mappings.md#unclassified-semantics) for the
bounded importer classifier. Canonical bodies and checks do not change when
a family is classified.

B-CNAV3 SOW refers to the start of its transmitted frame in BDT; it does not
replace the record's GPST navigation-epoch context. PPP correction epochs
are interpreted by message type. Correction usability also requires the
specified issue-of-data associations, not merely a valid RawBits CRC.

### SBAS L1 and L5

Both bodies contain exactly 250 bits, but use distinct family/layout identities:

| Family | Preamble | Message type | Data | CRC |
| --- | --- | --- | --- | --- |
| SBAS L1 | `[0:8]` | `[8:14]` | `[14:226]` | `[226:250]` |
| SBAS L5 | `[0:4]` | `[4:10]` | `[10:226]` | `[226:250]` |

CRC protects all of `[0:226]`, including the preamble.
The validated L5 check includes its first four bits. SBAS L5 is not accepted
by `SBAS_L1_250_V1` or the existing L1 grid decoder.


### GPS and QZSS CNAV-2

`CNAV2_1800_V1` denotes the receiver-normalized sequence of 52 SF1 symbols,
1200 SF2 symbols and 548 SF3 symbols. The latter two contain respectively
600 and 274 systematic bits including CRC. This is NOT
`BCNAV1_1800_V1` (72 + 1200 + 528). SF2's systematic region is `[52:652]`,
followed by parity `[652:1252]`; SF3's systematic region is `[1252:1526]`,
followed by parity `[1526:1800]`. Each systematic region ends with 24 CRC bits.
SF1 occupies `[0:52]`. Receiver deinterleaving/ordering belongs to the mapping;
equal dimensions alone do not establish correctness.

### QZSS augmentation/service containers

`QZS_L1S_250_V1` and `QZS_L5S_250_V1` retain 250 bits including their service
header and check region. They are not aliases of SBAS L1/L5. L1S independently
checks CRC over `[0:226]` against `[226:250]`; L5S currently retains receiver
check evidence only, without claiming a common independent CRC for all modes.

`L6_2000_V1` retains the complete 2000-bit message including RS parity.
L6D/L6E denote contributing signals, not an unconditional CLAS/MADOCA service
selection. Use `QZS_L6_UNCLASSIFIED` until service identity is established.
No independent L6 RS syndrome check or CNAV-2 SF1 BCH check is claimed.

## Extension boundary

Galileo QP/restricted signals and legitimate partial/alert assembly need their
own documented receiver mappings and canonical definitions before adoption.
Named RF signals do not automatically become supported RawBits contributors.
See the [remaining work](TODO.md); do not invent opaque fallback bodies.

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
