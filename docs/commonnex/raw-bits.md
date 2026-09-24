# CommonNEX RawBits records

[Overview](overview.md) | [Formats](raw-bits-formats.md) | [Receiver mappings](receiver-mappings.md#rawbits-mapping)

## Scope

The record family is `RawBits`, an individual received occurrence is a
`RawBitsOccurrence`, and its ParquetNEX catalog is `raw-bits`. These names are
receiver-independent; standardized source names such as `UBX-RXM-SFRBX` and
the SBF `RawNavBits` group retain their original spelling.

RawBits is a first-class record family in the core data model, with
capability-dependent presence. It stores receiver-delivered navigation bits for all in-scope systems
(GPS, Galileo, BeiDou, QZSS, and SBAS), including navigation families that
the current scientific processors cannot decode. The GLONASS/NavIC exclusion
still applies; the record structure itself is not tied to a constellation.
A decoded ephemeris or correction record does not replace received raw bits.
RawBits-only sources and datasets are valid without raw observations. They use
the same Setup identity and navigation-time association as mixed datasets;
never require observations or fabricate C/L/D/S values. Navigation time is
nullable: retain unresolved records without inventing an epoch, using the
[shared placement policy](receiver-time.md). Storage requires canonical packing support, not
a solver or a decoded-navigation implementation.
Adapters normalize them directly, and ParquetNEX preserves them for later
decoding without reopening UBX/SBF input. Scientific message-decoder
availability does not gate storage; a supported receiver-packing mapping does.
Documentary and sample-validated coverage are distinguished in receiver mappings.

Here, "raw bits" means normalized GNSS broadcast-message bits exported by a
receiver, including navigation and augmentation/correction messages. It does
not mean RF/IQ samples, UBX/SBF envelopes, or guaranteed untouched over-the-air
bits; bits already discarded by receiver firmware cannot be recovered here.
The record unit follows the canonical navigation family's word, fragment,
page, subframe, or message definition. Input adapters split or assemble
receiver reports as required by that definition. Do not require complete
ephemeris assembly before publishing an independently defined navigation unit,
force all signals into a GPS subframe length, or infer layout from length alone.

## Common fields

| Field | Type | Meaning |
| --- | --- | --- |
| `setup_id` | `string` | Acquisition stream |
| `nav_epoch_gpst` | `GpstTimestamp?` | Associated navigation-context time in DECIMAL(38,12) GPST seconds; null without a usable anchor |
| `receiver_uptime_s` | `Duration?` | Available freshly associated receiver uptime |
| `uptime_basis` | enum? | ASSOCIATED, or null without uptime |
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
| `receiver_channel` | `uint16?` | Source tracking channel; never a permanent satellite/signal identity |
| `source_diagnostics` | struct? | Nullable `sbf_viterbi_count: uint8?` and `sbf_rs_corrected_symbols: uint8?`; meaningful only for applicable source blocks |
| `checks` | list of records | Scoped receiver or independently evaluated checks; empty when none are known |

Satellite identity uses the same RINEX `G/E/C/J/S` domain as Observation;
for example SBAS PRN 137 is `(S,37)`, and QZSS PRN 193 is `(J,1)`.
The satellite fields identify the broadcaster, not necessarily a satellite
described by the message (for example, an almanac entry). An importer unable
to normalize the broadcaster identity reports an unsupported mapping and does
not emit an identity-incomplete RawBits record. Raw archives retain the input;
source identifiers in raw archives do not replace normalized identity.

`bitstream_source` is a non-null list of non-null common signal identifiers,
not UBX/SBF numeric signal codes or receiver Setup/antenna/station identifiers.
The [broadcast-signal registry](raw-bits-formats.md#broadcast-signal-identifiers) supplies its vocabulary. A single known contributor
has one entry; known mixed contributors have multiple distinct entries, with
no ordering or per-half/page assignment implied. An empty list means unknown
contributors, not a signal-less broadcast. Do not infer contributors from
signals merely enabled or tracked by the receiver. A combined report must not
be presented as exclusively received on the receiver's nominal signal code.
`signal_composition` retains known combined/unknown status when the list alone
cannot express it. Receiver-side identity is resolved through station metadata,
independently of this list and without an epoch-table reference.

### Broadcast signal identifiers

`bitstream_source` uses globally unique uppercase identifiers enumerated in
[RawBits registry](raw-bits-formats.md). Its primary-source crosswalk and status
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
The catalog has no generic source-identity record. Retain only the explicitly
defined diagnostics; RS counts are symbols, not bit errors. Reserved or
inapplicable diagnostics are null, not reported zero errors.

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

## Canonical body and occurrence rules

`body` is MSB-first: bit zero occupies bit 7 of byte zero. Its byte length is
ceil(bit_length / 8); unused low bits of the last byte are zero. The declared
format defines included headers, CRC/parity/FEC regions and removed tails.
Neither receiver envelopes nor receiver word padding belong in the body.
Equivalent receiver mappings must preserve equivalent canonical content, not
claim untouched over-the-air bits or reconstruct bits omitted by firmware.

The [format registry](raw-bits-formats.md) defines all supported family/format
pairs, lengths, content kinds, units and check scopes. Currently every supported
body is `complete`; no partial/fragment layout is approved. A truncated transport
frame is not a legitimate RawBits fragment and must not be zero-filled.

Unknown content types within a supported format remain storable. Unknown receiver
packing does not: report the unsupported mapping and preserve the raw archive.
Failed navigation checks do not prevent storage when canonical content is
extractable. Downstream processors select the families and check results they
can use; their acceptance policy is not another stored validity flag.

`nav_epoch_gpst` is reception context, not transmission time or precise arrival
time. Use the [receiver-time policy](receiver-time.md); never substitute a native
SIS header timestamp or fabricate time from a directory name. Emit complete
records with null time when needed, without retroactive filling.

Keep distinct received occurrences, including identical bodies at equal times.
There is no payload deduplication or required epoch/occurrence foreign key.
File/day/batch boundaries do not reset scientific continuity. Decoded ephemerides
are not substitutes for received RawBits.
