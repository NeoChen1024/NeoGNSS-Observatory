# CommonNEX RawNav extension

Status: v0 design draft; not an implemented format or API.

[Overview](overview.md)

## Scope

The optional RawNav extension stores receiver-delivered navigation bits for all in-scope systems
(GPS, Galileo, BeiDou, QZSS, NavIC, and SBAS), including navigation families that
the current scientific processors cannot decode. The earlier GLONASS exclusion
still applies; the record structure itself is not tied to a constellation.
A decoded ephemeris or correction record does not replace received raw bits.
Adapters normalize them directly, and ParquetNEX preserves them for later
decoding without reopening UBX/SBF input. Scientific message-decoder
availability does not gate storage; a validated receiver-packing mapping does.

Here, "raw bits" means the digital navigation content exported by a receiver,
not RF/IQ samples or recovery of bits already discarded by receiver firmware.
The record unit follows the canonical navigation family's word, fragment,
page, subframe, or message definition. Input adapters split or assemble
receiver reports as required by that definition. Do not require complete
ephemeris assembly before publishing an independently defined navigation unit,
force all signals into a GPS subframe length, or infer layout from length alone.

## Receiver mapping evidence

The following survey informs the proposed schema. It is not a claim of tested
firmware coverage or a complete bit-mapping specification.

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
| `NAVICRaw` | 292 bits |

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
| `occurrence_id` | `uint64` | Record identity within the stream, retained across batches and parts |
| `nav_epoch_id` | `uint64?` | Associated NavigationEpoch within the stream; null when unresolved |
| `satellite_system`, `satellite_number` | `string?`, `uint16?` | RINEX satellite identity where mapping is known; otherwise retain source identity |
| `signal_sources` | list of records | Known contributing signals, using system-specific RINEX band/attribute or native identity; may be empty if unknown |
| `signal_composition` | enum | `single`, `combined`, or `unknown`; does not imply an ordering of contributing signals |
| `message_family` | `string` | Standards-defined navigation family; not an observable code or receiver message ID |
| `body_format` | `string` | Canonical layout and revision defined for the navigation family, independent of receiver protocol |
| `content_kind` | enum | `navigation_bits` or `binary_symbols`, as fixed by the canonical family definition |
| `bit_length` | `uint32` | Number of meaningful bits under that layout |
| `body` | `bytes` | Canonical navigation content; parsing does not require a UBX/SBF packing decoder |
| `unit_kind` | enum | `word`, `fragment`, `page`, `subframe`, or `message`, as defined by the family |
| `completeness` | enum | `complete` or `partial`; partial records require a family-defined fragment representation |
| `source_identity` | typed record? | Native satellite/signal identifiers and source message ID/revision where scientifically relevant; not instructions for unpacking the body |
| `receiver_channel` | `uint16?` | Source tracking channel; never a permanent satellite/signal identity |
| `source_diagnostics` | typed record? | Applicable receiver-specific diagnostics with their native definitions |
| `checks` | list of records | Scoped receiver or independently evaluated checks; empty when none are known |

Each `signal_sources` entry contains nullable `signal: string`, optional typed
`source_identity`, and `component_scope: string?`. A combined report must not be
presented as exclusively received on the receiver's nominal signal code.
Do not infer which constituent page came from which signal without evidence.
An empty list means unknown contributors, not a signal-less broadcast.

Each `checks` entry has `origin` (`receiver` or `independent`), `kind`
(`crc`, `parity`, `bch`, or `unknown`), `scope: string`,
`result` (`pass`, `fail`, `unknown`, or `not_applicable`), and `evidence`
(`source_field`, `documented_output_policy`, or `computed`). The optional
`source_field: string?` identifies a native field when applicable. Scope names are defined
by `body_format`, for example a whole navigation unit or a numbered subframe.
The schema does not collapse multiple checks into a single successful boolean.
Receiver output policy is evidence distinct from an explicit per-record flag.

Processor acceptance is derived policy, not a property of the received bits;
it is omitted from this general record. The existing SBAS processing product
may retain its own acceptance field. Fragment/assembly context remains an
extension point, not a mandatory generic sequence model: do not invent sequence
numbers absent from the source. Transport truncation is not automatically a
legitimate navigation fragment.

## Adapter decisions

The following are proposed mappings based on the survey, not implemented APIs.

| Parameter group | Proposed retention |
| --- | --- |
| Identity | Normalize justified satellite/signal mappings; retain typed native identifiers for unresolved or representation-specific cases |
| Time | Associate with NavigationEpoch; no separate transmission/message timestamps in RawNav |
| Layout | Use a canonical family layout/revision; source message/block revisions select the importer mapping, not the downstream payload decoder |
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
RawNav time axis or silently equate it with an observation timestamp.

Names in the installed/generated schema can differ from the manual's display
names. The current SBF adapter accesses `NavBits` and decoded `SigIdx`, whereas
this document survey uses `NAVBits` and `Source`. Implementation must reconcile
these explicitly and retain defined flag bits, not assume a field-name match
or that an existing SBAS-only extraction covers every navigation block.
See [current SBF extraction](../../libcppgnss/src/sbf.cpp) and
[current UBX subframe decoding](../../libcppgnss/src/ubx_subframe.cpp).

## Canonical payloads

Canonical normalization is an agreed requirement. Field names, enum spellings,
and individual family layouts remain proposals. For canonical bit layouts,
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

For `sbas_l1_250`, persist exactly the 250-bit SBAS L1 message body, including
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
zero-filled into `sbas_l1_250` or stored as opaque receiver words.
Downstream processors select layouts, completeness, and validity they support;
failed or unknown checks do not prevent general raw-bit preservation.

RawNav timing is solely its NavigationEpoch association. It does not represent
precise transmission or receiver arrival time. There is no RawNav time_role,
time_reference, time_basis or independent gpst_ns field. Null nav_epoch_id means
unassociated; do not snap to a nearby epoch to make it timed. Timed consumers
must explicitly exclude or reject unassociated records. Store these separately
from GPST-day partitions, never under a date guessed from filenames.

NavigationEpoch may exist without observations. Multiple occurrences within
one epoch retain distinct IDs, and identical payloads broadcast in different
epochs remain distinct. Payload equality is never sufficient deduplication
proof. Physical file/day boundaries do not create new occurrences or reset
assembly. SBAS aging uses the associated epoch time; high-precision air-interface
timing is outside this extension's scope.

Retain generic continuity/end events alongside bodies. Neither daily partition
boundaries nor replay batches reset SBAS masks, message aging, or signal state.
RINEX input that does not contain these bodies cannot populate this family;
report that source limitation rather than manufacturing bits from decoded NAV.
