# RawBits canonical layouts and validation status

Status: accepted v0 layouts; see [implemented adapters](raw-bits-importer.md)
for sample-validated versus documentary support. See [RawBits](raw-bits.md) for
record identity, navigation-epoch association, and validity semantics.

## Conventions

All slices are zero-based and end-exclusive. Decode receiver U4 values as
little-endian integers before interpreting their bits. `W` denotes their
concatenation, each integer most-significant bit first; it is not the original
wire byte string. The canonical byte body is MSB-first, with unused low bits
of its last byte zero. Per-word padding and continuous-stream padding are
different and must not be interchanged.

Preserve receiver-normalized content, not purported untouched over-the-air
bits. Do not repeat inversion, deinterleaving, or error correction already
performed by the receiver. Keep CRC/parity and retained FEC symbols; remove
only the tails and container padding explicitly excluded below. Do not
regenerate source-omitted bits. No partial/fragment layouts are approved here.

The LDPC-based B1C/B2a/B2b and future CNAV-2 representations retain the exported
normalized coded regions, rather than only systematic information bits.
Decoders can select the information slices without deleting the remaining
content from RawBits. Preservation of FEC symbols does not imply independent
validation of their codewords.

### Family versus unpacking format

`message_family` identifies navigation semantics; `body_format` identifies
canonical unpacking semantics. Both use uppercase identifiers, but format
names have no mandatory system/family prefix. The
[registry](raw-bits-registry.md#legal-family--unpacking-format-pairs) enumerates
legal pairs, globally unique names, and documented versus sample-checked status.
The shared LNAV format does not merge GPS/QZSS content semantics. The SBAS
formats remain distinct because their field boundaries differ. `_V1` tracks canonical layout and
unpacking semantics only, not firmware, source protocol or daily-file revision.
See [RawBits field definitions](raw-bits.md#orthogonal-family-and-layout-identifiers).

Complete further legal pairs from the documented navigation semantics and
validated layouts below. Share formats only when unpacking semantics match,
not merely length; leave unverified mappings TODO. UBX/SBF revision tables
belong to importer coverage documentation, not this canonical format registry.

## Validated mapping coverage

Validation used UBX archive samples and SBF archive samples, including a
complete BEE0 SBF recording for 2026-09-13 GPST. This establishes packing and
the specified check scopes, not exhaustive firmware support, full message
semantics, or simultaneous cross-receiver equality of a broadcast occurrence.
No receiver implementation is inferred from another receiver's support.

| Family | Canonical bits / bytes | Unit | Receiver mapping evidence |
| --- | --- | --- | --- |
| GPS LNAV | 300 / 38 | subframe | UBX and SBF parity-layout validation |
| QZSS LNAV | 300 / 38 | subframe | SBF parity-layout validation |
| GPS/QZSS CNAV | 300 / 38 | message | UBX/SBF CNAV checks; SBF L2C and L5 checked separately |
| Galileo I/NAV nominal | 228 / 29 | page | UBX and SBF nominal-page CRC validation |
| Galileo F/NAV | 238 / 30 | page | SBF CRC validation; UBX mapping is document-supported, not sample-validated |
| Galileo C/NAV | 486 / 61 | page | SBF CRC validation |
| BeiDou D1/D2 | 300 / 38 | subframe | UBX and SBF BCH-layout validation |
| BeiDou B1C (B-CNAV1) | 1800 / 225 | frame represented as `message` | SBF SF2/SF3 CRC slices validated |
| BeiDou B2a (B-CNAV2) | 576 / 72 | frame represented as `message` | SBF information-region CRC validated |
| BeiDou B2b | 984 / 123 | frame represented as `message` | SBF CRC slice validated; B-CNAV3 versus PPP-B2b service routing remains TODO |
| SBAS L1 | 250 / 32 | message | UBX and SBF CRC validation |
| SBAS L5 | 250 / 32 | message | Full-file SBF CRC validation |

GPS and QZSS retain distinct family identities even where packing is shared.
Signal contributors remain explicit: equal content on L2C/L5 is not proof of
one received occurrence. Galileo C/NAV is unrelated to GPS CNAV despite the
similar names. The coded BeiDou regions use `content_kind=binary_symbols`;
the remaining validated layouts use `navigation_bits`.

### GPS and QZSS LNAV

Concatenate the low 30 bits of each of ten exported words. Preserve the
receiver-deinverted 24 data bits and six normalized parity bits per word.
Exclude the receiver word's two high bits from the canonical body; they must
not be assumed to be universally zero or copied as navigation content.

Independent parity checks must use the normalized parity convention, not an
unmodified over-the-air parity checker. The first word needs separate handling
of unavailable preceding-word context; subsequent words can use the preceding
word's relevant parity-bit relationship. Record such a check as
`parity/normalized_word_chain`, distinct from receiver output-policy evidence.
Observed UBX chain exceptions and receiver-failed SBF frames that pass a
limited independent check prohibit equating these checks. Never repair bits
merely to make the chain pass.

### GPS and QZSS CNAV

Take `W[0:300]` from ten U4 words, discarding the final 20 container bits.
Unlike LNAV, do not take 30 bits from each word. The last 24 body bits are CRC;
the check covers the preceding 276 bits. This is not a CNAV-2 layout.

### Galileo pages

| Family | UBX slice | SBF slice | Independent CRC scope |
| --- | --- | --- | --- |
| I/NAV nominal | `W[0:114] + W[128:242]` | `W[0:228]` | Canonical `[0:196]` with CRC `[196:220]` |
| F/NAV | Candidate `W[0:238]` | `W[0:238]` | `[0:214]` with CRC `[214:238]` |
| C/NAV | No validated mapping | `W[0:486]` | `[0:462]` with CRC `[462:486]` |

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

UBX: concatenate the low 30 bits of ten words. SBF: take `W[0:300]`.
The output is already deinterleaved; do not deinterleave it again.

Within the first 30-bit word, `[15:30]` forms a BCH(15,11) codeword. For each
subsequent word `w`, the two BCH codewords are `w[0:11] + w[22:26]` and
`w[11:22] + w[26:30]`. The generator is `x^4 + x + 1`.
Keep all 300 bits, not just the data portions. Independent BCH success does
not replace receiver validity or establish all header/time semantics.

### BeiDou coded regions

| Family | Retained SBF slice | Information including CRC | Other retained content |
| --- | --- | --- | --- |
| B1C | `W[0:1800]` | SF2 `[72:672]`; SF3 `[1272:1536]` | SF1 `[0:72]`; SF2 parity `[672:1272]`; SF3 parity `[1536:1800]` |
| B2a | `W[0:576]` | `[0:288]` | Parity `[288:576]` |
| B2b | `W[0:984]` | `[12:498]` | Prefix `[0:12]`; parity `[498:984]` |

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
bits including CRC and 486 parity bits, not 162 binary bits. The complete
81-by-162 sparse parity-check matrices were compared: all 324 nonzero column
positions and all 324 coefficients match exactly. The comparison followed
the specified top-to-bottom, then left-to-right column-group ordering across
page breaks. This establishes identical normative FEC definitions; it does
not constitute a syndrome check of receiver-exported codewords.

The open-service ICD covers MEO/IGSO B-CNAV3 and defines types 10, 30 and 40;
type 0 is invalid and all others are reserved (table 7-1). The PPP ICD
describes GEO broadcasts and defines types 1-7 and 63 (null information),
with 8-62 reserved (table 6-1); that table does not assign type 0.
Known service context and a valid, defined message type provide evidence
for routing. Do not classify solely by a hard-coded PRN range, the availability
bit, body length, or CRC success. Reserved/corrupt types cannot independently
identify a family. Actual receiver service routing remains to be verified;
use `BDS_B2B_UNCLASSIFIED` for an unresolved record rather than choosing either
service. The shared format still retains the full coded envelope.

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

Take `W[0:250]`; CRC protects all of `[0:226]`, including the preamble.
The validated L5 check includes its first four bits. SBAS L5 is not accepted
by `SBAS_L1_250_V1` or the existing L1 grid decoder.

UBX SBAS samples can contain a ninth word outside this body. Its observed
zero value is not a universal guarantee. Exclude extras only when the known
body's boundaries and meaning are independently established, and report the
retention limit. QZSS L1S/L5S are not identified merely by a 250-bit length.

## Adapter constraints discovered during validation

- Pinned pysbf2 at `e0e044a` omits `RxChannel` in `GALRawCNAV`. The official
  payload prefix is 12 bytes; NAVBits starts after it. Resolve the schema
  defect before using generated fields for this mapping. Do not edit generated
  output or silently compensate by changing the canonical layout.
- Full-file transport CRC success does not imply navigation CRC success.
  Retain both receiver and independently calculated scoped results.
- Valid transport and parity also do not guarantee usable time. BDSRaw native
  timestamp outliers have been observed; require justified navigation-time
  association and store `nav_epoch_gpst` directly, rather than treating all raw
  TOW/WNc values as valid coverage. No epoch-table reference is required.
- The BEE0 full-file scan contained SBAS L5, but no GPS/QZSS RawL1C, QZSS
  RawL1S/RawL5S or raw L6 blocks. This is a dataset limitation, not proof of
  universal receiver incapability.

## Remaining work

Completed checkboxes describe research validation, not shipped importer support.

### Agreed design and verified scopes

- [x] Validate the packing/check scopes listed above using actual source data.
- [x] Confirm SBAS L5's 250-bit body and preamble-inclusive CRC on a whole file.
- [x] Retain normalized coded regions and FEC parity for modern BeiDou families.
- [x] Compare formal B2b/PPP-B2b 1.0 framing, prefix semantics, CRC scopes and
  all parity-check matrix entries; distinguish shared packing from service semantics.
- [x] Separate semantic family, unpacking format, broadcast satellite identity
  and contributing signals (`bitstream_source`).

### Remaining definitions

- [x] Enumerate the reviewed uppercase signal/family/format identifiers and
  legal pairs in raw-bits-registry.md, distinguishing pending receiver support.
- [ ] Finalize B-CNAV3 versus PPP-B2b service routing; CRC agreement alone is
  insufficient to identify the service.

### Deferred source validation

- [ ] Validate UBX F/NAV and other missing cross-receiver mappings when suitable
  receiver output becomes available.
- [ ] Define/validate I/NAV alert pages and any legitimate partial units using
  appropriate documentation and actual usable samples.
- [ ] Independently validate B1C SF1 BCH and retained LDPC codewords if needed;
  current evidence validates only the listed CRC slices.
- [ ] GPS CNAV-2: validate receiver packing for the distinct 1800-symbol
  `52 + 1200 + 548` layout. Do not reuse B1C's `72 + 1200 + 528` layout.
  QZSS L1C/CNAV-2 framing is document-cross-checked in the registry, but its
  receiver mapping still needs samples. SBF schema
  IDs 4221/4227 exist, but current recordings do not supply validation samples.
- [x] Validate UBX QZSS L1S's 250-bit body and CRC, including the nine-word
  receiver container; implement the separately documented SBF adapter.
- [x] Implement documentary QZSS L5S and L6D/L6E mappings using the G5 guide,
  with distinct L5S layout, full L6 parity retention and correct RS count units.
- [ ] Validate QZSS L5S/L6 and SBF L1S with actual receiver samples.

Missing receiver support or samples leave these items TODO; do not invent a
fallback payload or require unrelated acquisition upgrades. GLONASS and NavIC
remain out of scope.

### Implementation

- [x] Implement verified and documentary mappings in CommonNEX adapters, storing navigation
  time directly and publishing applicable Events without epoch-table references.
- [x] Read GALRawCNAV's official 12-byte prefix in the canonical adapter without
  using the defective generated fields or editing generated output.
- [ ] Resolve the upstream generic GALRawCNAV schema defect.

## References

- [u-blox ZED-F9P integration manual, section 3.15.1](https://content.u-blox.com/sites/default/files/ZED-F9P_IntegrationManual_UBX-18010802.pdf)
- [Septentrio mosaic-X5 4.15.0 reference guide, raw navigation blocks](https://docs.sparkfun.com/SparkFun_GNSS_mosaic-X5/assets/component_documentation/firmware/mosaic-X5_Firmware_v4.15.0_Reference_Guide.pdf)
- [Galileo HAS SIS ICD](https://www.gsc-europa.eu/sites/default/files/sites/all/files/Galileo_HAS_SIS_ICD_v1.0.pdf)
- [BeiDou B3I ICD, BCH definition](https://en.beidou.gov.cn/SYSTEMS/ICD/201806/P020180608516798097666.pdf)
- [GPS IS-GPS-800J, CNAV-2](https://www.gps.gov/sites/default/files/2025-07/IS-GPS-800J.pdf)
- [ICAO DFMC SBAS SARPs Part B, L5 message/CRC structure](https://www.icao.int/sites/default/files/sp-files/airnavigation/Documents/NSP5_Report%20on%20Agenda%20Item%202.APPENDIX%20A2%20-%20DFMC%20SBAS%20SARPS%20Part%20B.pdf)
- [Septentrio mosaic-G5 1.0.1 reference guide, L6D](https://docs.sparkfun.com/SparkFun_GNSS_mosaic-G5_P3/assets/component_documentation/firmware/v1.0.1/mosaic-G5%20Firmware%20v1.0.1%20Reference%20Guide.pdf)
