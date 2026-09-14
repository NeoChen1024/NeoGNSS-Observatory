# CommonNEX RawBits record family

Status: v0 design draft; not an implemented format or API.

[Overview](overview.md)

## Scope

The record family is `RawBits`, an individual received occurrence is a
`RawBitsOccurrence`, and its ParquetNEX catalog is `raw-bits`. These names are
receiver-independent; standardized source names such as `UBX-RXM-SFRBX` and
the SBF `RawNavBits` group retain their original spelling. `DecodedNav` remains
the separate family for decoded parameters.

RawBits is a first-class record family in the core data model, with
capability-dependent presence. It stores receiver-delivered navigation bits for all in-scope systems
(GPS, Galileo, BeiDou, QZSS, and SBAS), including navigation families that
the current scientific processors cannot decode. The GLONASS/NavIC exclusion
still applies; the record structure itself is not tied to a constellation.
A decoded ephemeris or correction record does not replace received raw bits.
RawBits-only sources and datasets are valid without raw observations. They use
the same Setup/Stream identity and navigation-time association as mixed datasets;
never require observations or fabricate C/L/D/S values. Valid navigation
time association is required; unresolved records are skipped and counted after
bounded association attempts, not stored under invented epochs. Storage requires canonical packing support, not
a solver or a decoded-navigation implementation.
Adapters normalize them directly, and ParquetNEX preserves them for later
decoding without reopening UBX/SBF input. Scientific message-decoder
availability does not gate storage; a validated receiver-packing mapping does.

Here, "raw bits" means normalized GNSS broadcast-message bits exported by a
receiver, including navigation and augmentation/correction messages. It does
not mean RF/IQ samples, UBX/SBF envelopes, or guaranteed untouched over-the-air
bits; bits already discarded by receiver firmware cannot be recovered here.
The record unit follows the canonical navigation family's word, fragment,
page, subframe, or message definition. Input adapters split or assemble
receiver reports as required by that definition. Do not require complete
ephemeris assembly before publishing an independently defined navigation unit,
force all signals into a GPS subframe length, or infer layout from length alone.

## Receiver mapping evidence

The following survey supplies protocol context. Accepted layouts, actual
validation coverage, and remaining work are recorded in
[RawBits layouts](raw-bits-layouts.md). Neither a schema entry nor a successful
build establishes scientific validation for every receiver revision.

UBX reference: F9 HPG 1.51 Interface description, protocol 27.50,
UBXDOC-963802114-13124 R01, section 3.17.9, page 201.
`UBX-RXM-SFRBX` (`0x02 0x13`) has an eight-byte prefix and
`numWords` little-endian `U4` values (`dwrd`). Prefix fields are `gnssId`,
`svId`, `sigId`, `freqId`, `numWords`, `chn`, `version`, and `reserved0`.
There is no payload timestamp or per-record navigation CRC flag. `freqId`
is GLONASS-specific; message version is distinct from protocol version.
[u-blox interface description](https://content.u-blox.com/sites/default/files/documents/u-blox-F9-HPG-1.51_InterfaceDescription_UBXDOC-963802114-13124.pdf#page=201).

The ZED-F9P Integration manual, UBX-18010802 R16, section 3.15.1,
pages 74-82, describes complete, parity-checked output and receiver-side
inversion handling. GPS LNAV and BeiDou use ten words with per-word padding;
Galileo uses signal-dependent page layouts. Its SBAS diagram describes eight
words, while its summary lists nine. These are not grounds to discard an extra
word or assert a universal packing rule.
[u-blox integration manual](https://content.u-blox.com/sites/default/files/ZED-F9P_IntegrationManual_UBX-18010802.pdf?hash=undefined#page=74).

SBF reference: mosaic-X5 firmware 4.15.0 Reference Guide, sections 4.1.3
and 4.2.2, pages 257 and 276-292. Navigation blocks use `NAVBits` words;
common fields include `TOW`, `WNc`, `SVID`, `Source`, `RxChannel`, and
block-dependent checks/diagnostics. SIS timestamps mark transmission-end of
the last contributing bit, not receiver arrival. Representative payload sizes:

| SBF blocks | Exported content length |
| --- | --- |
| `GPSRawCA`, `GPSRawL2C`, `GPSRawL5` | 300 bits |
| `QZSRawL1CA`, `QZSRawL2C`, `QZSRawL5` | 300 bits |
| `GALRawFNAV`, `GALRawINAV`, `GALRawCNAV` | 244, 234, 492 bits |
| `GEORawL1`, `GEORawL5` | 250 bits |
| `BDSRaw` | 300 bits |
| `BDSRawB1C`, `BDSRawB2a`, `BDSRawB2b` | 1800, 576, 984 binary symbols |

`GALRawINAV.Source` can indicate combined E1/E5b sub-pages; its layout removes
the even-page tail. `BDSRawB1C` has separate `CRCSF2`/`CRCSF3` checks.
`CRCPassed` and `ViterbiCnt` are not universal. GPSRawCA's parity treatment is
specified separately. Transport CRC is distinct from navigation validity.
[Septentrio reference guide](https://docs.sparkfun.com/SparkFun_GNSS_mosaic-X5/assets/component_documentation/firmware/mosaic-X5_Firmware_v4.15.0_Reference_Guide.pdf#page=257).

Design consequence: validate a canonical mapping for each signal and receiver
revision before emitting CommonNEX records. Equal message families and equal
lengths do not establish equal bit sequences. Receiver-export words can be
internal importer state or diagnostics while a mapping is investigated, but
are not a compliant CommonNEX payload. Navigation symbols after receiver error
correction also need a family-defined representation; do not label all payloads
as unmodified transmitted data bits.

## Common fields

| Field | Type | Meaning |
| --- | --- | --- |
| `stream_id` | `string` | Acquisition stream |
| `nav_epoch_gpst` | `GpstTimestamp` | Required associated navigation-context time, directly stored as DECIMAL(38,12) GPST seconds |
| `satellite_system`, `satellite_number` | `string`, `uint16` | Required normalized identity of the broadcasting satellite |
| `bitstream_source` | `list<string>` | Common signal identifiers for known contributing broadcast signals; empty if unknown |
| `signal_composition` | enum | `single`, `combined`, or `unknown`; does not imply an ordering of contributing signals |
| `message_family` | `string` | Navigation-message semantics selecting the content decoder; not an observable code or receiver message ID |
| `body_format` | `string` | Globally unique canonical layout/version selecting the bit unpacker; may be shared by compatible families |
| `content_kind` | enum | `navigation_bits` or `binary_symbols`, as fixed by the canonical family definition |
| `bit_length` | `uint32` | Number of meaningful bits under that layout |
| `body` | `bytes` | Canonical navigation content; parsing does not require a UBX/SBF packing decoder |
| `unit_kind` | enum | `word`, `fragment`, `page`, `subframe`, or `message`, as defined by the family |
| `completeness` | enum | Body completeness, not epoch completion: `complete` or `partial`; partial requires a family-defined fragment layout |
| `source_identity` | typed record? | Native satellite/signal identifiers and source message ID/revision where scientifically relevant; not instructions for unpacking the body |
| `receiver_channel` | `uint16?` | Source tracking channel; never a permanent satellite/signal identity |
| `source_diagnostics` | typed record? | Applicable receiver-specific diagnostics with their native definitions |
| `checks` | list of records | Scoped receiver or independently evaluated checks; empty when none are known |

Satellite identity uses the same RINEX `G/E/C/J/S` domain as Observation;
for example SBAS PRN 137 is `(S,37)`, and QZSS PRN 193 is `(J,1)`.
The satellite fields identify the broadcaster, not necessarily a satellite
described by the message (for example, an almanac entry). An importer unable
to normalize the broadcaster identity reports an unsupported mapping and does
not emit an identity-incomplete RawBits record. Raw archives retain the input;
optional native identifiers do not replace the required normalized identity.

`bitstream_source` is a non-null list of non-null common signal identifiers,
not UBX/SBF numeric signal codes or receiver Setup/antenna/Stream identifiers.
The [broadcast-signal registry](raw-bits-registry.md#broadcast-signal-identifiers) supplies its vocabulary. A single known contributor
has one entry; known mixed contributors have multiple distinct entries, with
no ordering or per-half/page assignment implied. An empty list means unknown
contributors, not a signal-less broadcast. Do not infer contributors from
signals merely enabled or tracked by the receiver. A combined report must not
be presented as exclusively received on the receiver's nominal signal code.
`signal_composition` retains known combined/unknown status when the list alone
cannot express it. Receiver-side identity is resolved through Stream metadata,
independently of this list and without an epoch-table reference.

### Broadcast signal identifiers

`bitstream_source` uses globally unique uppercase identifiers enumerated in
[RawBits registry](raw-bits-registry.md). Its primary-source crosswalk and status
tables distinguish named signals from enabled receiver mappings. Do not turn
a coarse source indication into an inferred component.
A justified combined Galileo report uses `["GAL_E1_B", "GAL_E5B_I"]`, without
assigning individual half-pages to those contributors.

Observation's `signal` retains its system-specific RINEX band/attribute
representation; this broadcast-source vocabulary does not replace it.
Observation-code crosswalks do not prove reception of navigation bits.

### Orthogonal family and layout identifiers

Keep `message_family` and `body_format` separate. The former selects navigation
content semantics; the latter specifies canonical bit segmentation, ordering,
and CRC/parity/FEC retention and selects the unpacker. Here unpacking means
CommonNEX body to navigation fields/regions, not UBX/SBF wire decoding. Source
endianness and receiver padding have already been normalized by the importer.

Use uppercase C-enum-style identifiers. Prefer established navigation-standard
terminology, including RINEX names where applicable; use signal ICDs and SBF
as references for families not represented by RINEX. UBX availability is not
a prerequisite. Do not turn SBF block names into protocol-dependent canonical
formats.

`body_format` names are globally unique, but need no mandatory satellite-system
or family prefix. Different families can share a format only when their entire
layout and unpacking semantics agree, not just their bit lengths. Code reuse
alone is not evidence of equivalence. For example, `GPS_LNAV` and `QZS_LNAV`
both pair with `LNAV_300_V1`. Family-specific semantic interpretation remains
in their content decoders.

The format registry lists legal family/format pairs; reject contradictory
combinations. A family can acquire another format version when the canonical
layout or unpacking semantics change. Merely implementing another message-type
decoder does not change that version. `_V1` is a CommonNEX layout version,
independent of firmware/protocol revisions and ParquetNEX file revisions.

Source protocol revision handling belongs to the importer, not the CommonNEX
storage contract. Readers use the CommonNEX schema version, semantic family
and canonical body format, never UBX protocol/SFRBX versions or SBF block
revisions to decode canonical content. Importers select and validate source
mappings, considering firmware only when relevant to an actual documented
difference. Do not copy that selection machinery into mandatory row fields.
Retain source metadata only where separately justified for scientific
interpretation, not as an alternate canonical unpacker selector.

Each `checks` entry has `origin` (`receiver` or `independent`), `kind`
(`crc`, `parity`, `bch`, `ldpc`, `reed_solomon`, or `unknown`), `scope: string`,
`result` (`pass`, `fail`, `unknown`, or `not_applicable`), and `evidence`
(`source_field`, `documented_output_policy`, or `computed`). The optional
`source_field: string?` identifies a native field when applicable. Scope names are defined
by `body_format`, for example a whole navigation unit or a numbered subframe.
The schema does not collapse multiple checks into a single successful boolean.
Receiver output policy is evidence distinct from an explicit per-record flag.

Processor acceptance is derived policy, not a property of the received bits;
it is omitted from this general record. The SBAS grid adapter derives acceptance
from the scoped checks without persisting an extra acceptance field. Fragment/assembly context remains an
extension point, not a mandatory generic sequence model: do not invent sequence
numbers absent from the source. Transport truncation is not automatically a
legitimate navigation fragment.

## Adapter decisions

The following are proposed mappings based on the survey, not implemented APIs.

| Parameter group | Proposed retention |
| --- | --- |
| Identity | Require normalized broadcaster identity; map known contributing signals to `bitstream_source`; native identifiers are supplementary, not fallback satellite identity |
| Time | Store associated navigation time directly in `nav_epoch_gpst`; no epoch-table reference or separate transmission/message time |
| Layout | Select a legal semantic-family/canonical-format pair; source message/block revisions select the importer mapping, not a protocol-specific downstream unpacker |
| Channel | Map tracking-channel identity to `receiver_channel`; unknown remains null |
| Checks | Map each applicable check separately, with its scope and evidence; never equate transport validation with navigation validation |
| Diagnostics | Preserve defined, applicable source metrics; do not manufacture a shared quality score |
| Counts | Derive word counts from preserved word arrays when exact; retain separately only if the normalized representation no longer expresses the source count |
| Reserved fields | Remove documented padding; retain unexplained words in the raw archive or importer diagnostics, and report their exclusion from canonical content |

For UBX, an associated epoch supplies context time, not a measured arrival time.
Map `chn` to channel identity. Source identity and mapping selection use the exact `gnssId`,
`svId`, `sigId`, and `version` where required; protocol/firmware belongs in
stream metadata. A receiver-policy check may be recorded only for a documented
applicable version. Independently checking a normalized body requires the
right parity convention, not simply replaying a transport checksum.

For SBF, retain the complete meaningful interpretation of `Source`, including
combination flags, rather than masking it down to a nominal signal index.
Keep `CRCSF2` and `CRCSF3` as separate scoped results. Preserve applicable
`ViterbiCnt` in the `sbf` diagnostics namespace; not-applicable fields become
absent, not measurements of zero errors. Resolve the containing navigation epoch using an explicit adapter rule.
Native SIS timing may inform association internally; do not copy it as a second
RawBits time axis or silently equate it with an observation timestamp.

Names in the installed/generated schema can differ from the manual's display
names. The current SBF adapter accesses `NavBits` and decoded `SigIdx`, whereas
this document survey uses `NAVBits` and `Source`. Implementation must reconcile
these explicitly and retain defined flag bits, not assume a field-name match
or that an existing SBAS-only extraction covers every navigation block.
See [current SBF extraction](../../libcppgnss/src/sbf.cpp) and
[current UBX subframe decoding](../../libcppgnss/src/ubx_subframe.cpp).

## Canonical payloads

Canonical normalization and the validated layouts in
[RawBits layouts](raw-bits-layouts.md) are agreed design decisions. The
[importer coverage](raw-bits-importer.md) distinguishes implemented UBX/SBF
mappings with receiver samples from documentary mappings without samples.
For canonical bit layouts,
bit order is MSB-first: body bit zero occupies bit 7 of byte zero. Unused low
bits in the final byte are zero. Each `body_format` specifies included bits,
parity/FEC/deinterleaving or other receiver transformations when known, and the
meaning of the check fields. Do not claim original over-the-air bits when the
receiver exports a transformed representation.

The logical `body` is a canonical bit/symbol sequence.
The byte convention describes that field's content, not a CommonNEX transport
envelope or mandatory in-memory container. A transport may carry typed words
instead of bytes if it preserves the declared logical value exactly.

Define canonical payloads by navigation family, for example GPS LNAV/CNAV,
Galileo I/NAV/F/NAV, the individual BeiDou navigation families, and SBAS L1.
Different families may have different sizes and structures; different receiver
protocols must not create alternative payload definitions for the same family
and canonical revision.

Every family definition must specify:

- The independently stored unit and any permitted fragment representation.
- Exact bit count/order and placement of headers, data, CRC/parity, and tails.
- Treatment of inversion, interleaving, FEC, and receiver transformations.
- How source-omitted or irrecoverable information is represented, including
  any required availability fields; never invent missing bits.
- The scopes of validity checks; epoch association is an adapter rule.

For equivalent navigation content, UBX and SBF adapters must produce equivalent
canonical payloads and explicitly represented availability, even when source
quality diagnostics or acquisition times differ. Source-only information needed
for interpretation belongs in typed auxiliary fields, not a second opaque
payload that scientific consumers must unpack. Resolving differences in parity,
tails, and receiver-added information is part of defining each family mapping;
neither discarding them without review nor assuming the source words match is
acceptable.

An unknown navigation message type inside a known canonical structure remains
storable without decoding its ephemeris/correction fields. An unknown receiver
packing or unresolved canonical mapping is unsupported input for this family:
report it explicitly and retain the original archive. Importer staging or
diagnostics may hold exported words, but neither CommonNEX nor ParquetNEX treats
them as a compliant fallback. Checksums alone do not establish a valid mapping.

Preserve failed navigation checks and family-defined partial units when their
canonical content is extractable. Do not manufacture fragments from truncated
transport frames. Report normalization exclusions separately from downstream
message-decoder limitations and successfully retained records.

For `SBAS_L1_250_V1`, persist exactly the 250-bit SBAS L1 message body, including
its preamble, message type, data, and CRC, in 32 bytes with six zero padding
bits. Remove UBX/SBF envelopes and receiver word-storage padding. This layout
does not accept SBAS L5 bodies. Native signal identifiers remain available in
raw archives/decoder APIs; a generic extension mechanism does not require
persisting them in the SBAS body product.

If an input has unexplained extra navigation words, retain them in the original
archive or importer diagnostics and report the canonical mapping's retention
limit. Emit the known 250-bit body only if its boundaries and interpretation
are established independently of those extra words. That record does not claim
complete preservation of every exported receiver word. If the extra information
affects interpretation, resolve the mapping before emitting a canonical record.

The receiver's CRC result and an independent body check remain distinct. An
unperformed check is absent or explicitly unknown, not success. A partial SBAS
payload is retained only under a defined canonical fragment layout, never
zero-filled into `SBAS_L1_250_V1` or stored as opaque receiver words.
Downstream processors select layouts, completeness, and validity they support;
failed or unknown checks do not prevent general raw-bit preservation.

RawBits stores the associated navigation epoch time in `nav_epoch_gpst` directly.
It is not precise transmission or receiver arrival time, nor an observation
measurement timestamp. No epoch table, `nav_epoch_id`, or mandatory occurrence
counter is required. Importers may buffer records while
waiting for a justified anchor. If association remains unresolved at the bounded
buffer limit or finalization, skip the record and count/report the exclusion.
Do not snap to nearby epochs or guess dates from filenames. There is no
unassociated RawBits table or partition; raw archives remain available for future
reconstruction. Navigation validity checks and time association are separate:
a failed navigation CRC does not itself invalidate an otherwise usable epoch.

Navigation context may exist without observations. Multiple occurrences within
one epoch remain separate rows, including repeated equal payloads. Timestamps
are not unique keys. Payload equality is never sufficient deduplication
proof. Physical file/day boundaries do not create new occurrences or reset
assembly. SBAS aging uses the associated epoch time; high-precision air-interface
timing is outside this family's scope.

Retain generic continuity/end events alongside bodies. Neither daily partition
boundaries nor replay batches reset SBAS masks, message aging, or signal state.
RINEX input that does not contain these bodies cannot populate this family;
report that source limitation rather than manufacturing bits from decoded NAV.
