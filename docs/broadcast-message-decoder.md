# Broadcast message decoding and assembly

Selected design and remaining roadmap. A first GPS/QZSS LNAV implementation is
available through [the realtime decoder and Python API](broadcast-decode-realtime.md).
That guide owns current coverage and concrete fields; proposals below are not
claims of complete implementation for every family or validity model.

## Boundary and outputs

`BroadcastMessageDecoder` belongs to `libneognss-obs`. It consumes CommonNEX
RawBits batches, not receiver envelopes. It exposes decoded fields and completed
parameter assemblies independently of RTKLIB or any other calculation backend.
These are downstream derived records, not additions to the CommonNEX spec.
Persistence is optional and must not precede delivery to scientific consumers.

Two independent outputs share decoding state:

- `MessageOutput`: one occurrence whenever a single message or a multi-message
  set is fully received and decoded. Repeated complete content is retained.
- `Snapshot`: applicable parameter candidates and explicitly scoped statistics
  at the current processing time. Different sources/families/issues can coexist;
  the consumer, not the decoder, chooses the calculation input.

Snapshots cannot query past processing state. Historical queries require replay.
Periodic snapshots use the GPST grid and statistics contract in the
[processing roadmap](TODO.md#optional-periodic-snapshots). When navigation context
advances across a grid target, emit the previously known state before consuming
the new context's record. Do not backfill from a later cache. This is an
input-order view, not a claim of exact RF-time completeness: RawBits timestamps
are last effective navigation context, not SIS or packet arrival timestamps.
No `complete_through_gpst` watermark or all-messages-at-time barrier is required.
Batch boundaries must not affect ordering or trigger additional snapshots.

Each output kind has its own typed Arrow batch schema. Native typed records stay
independent of RTKLIB structures. Message and snapshot records can reuse parameter
structs, but receipt/completion time and snapshot evaluation time remain distinct.
No universal nullable union table or bulk JSON interface is proposed.

### Snapshot bundle

A snapshot groups typed batches for ephemerides, almanacs, clock/group-delay
parameters, system models, health/configuration and useful statistics. All
groups share `snapshot_gpst`; their schemas and applicability remain specific
to their contents. Clock or delay parameters published inseparably with an
ephemeris stay with that candidate rather than being independently recombined.
Missing categories produce no invented candidates. Statistics declare their
window, coverage and missingness as specified by the shared snapshot contract.

Only snapshots may aggregate common information across broadcasting satellites
within one GNSS system. MessageOutput assembly remains source-local. Aggregation
requires compatible model, region, reference epoch and contents; it does not
merge arbitrary signal formats or select a winner among conflicting candidates.
Keep per-item contributing broadcaster/signal identities and first/last receipt
context. Cross-source completion never emits a synthetic MessageOutput assembly.

For almanacs, an individually decoded entry with established reference time can
contribute immediately; no source-local full-set completion is required. Group
by system, almanac type and resolved reference epoch. Different epochs remain
separate; do not fill holes in a new set using old entries. Identical candidates
merge source evidence, while differing candidates for the same subject satellite
remain separate rows, not a combinatorial expansion of full sets.

Proposed `almanac_sets` fields are `snapshot_gpst`, `satellite_system`,
`almanac_type`, `reference_gpst`, `completeness` (COMPLETE/PARTIAL/UNKNOWN),
`received_satellite_count`, nullable `expected_satellite_count`, and
`conflict_present`. Completeness and conflict are independent. Counts exclude
dummy slots and count each meaningful subject satellite once, not each candidate.
Unknown expected membership cannot yield COMPLETE solely because no new entries
have arrived recently.

`almanac_entries` repeats the named snapshot/system/type/reference fields, plus
the subject `satellite_number`, typed `parameters` and per-candidate `sources`.
Sources retain broadcaster, `bitstream_source` and receipt-context range. There
are no row-ID foreign keys. MessageOutput reuses parameter structs with its own
receipt/completion context rather than a snapshot timestamp. Exact Arrow field
types for these proposed batches remain part of the schema definition work.

### Normalized orbit parameters

Share typed structs by physical model, not just by constellation. Use float64
for orbital quantities, meters for lengths and radians for angles; keep time
types and native/GPST reference semantics explicit. Normalize wire scale factors
and model-defined reference offsets in the decoder. In particular, GPS/QZSS
almanacs should expose full eccentricity/inclination rather than requiring the
consumer to restore constellation-specific reference offsets. Retain model and
family identity: equal field names do not prove equal propagation algorithms.

Simplified almanac, harmonic-corrected ephemeris, modern models with additional
rates, and Cartesian state vectors need different structs where their semantics
differ. Do not discard modern parameters to fit a legacy backend or create one
giant nullable orbit schema. Purely redundant wire values need not be copied;
CommonNEX RawBits remains the original-content record.

## Input and identity

Use `GPS_LNAV` / `QZS_LNAV` with `LNAV_300_V1`. Follow the existing
[canonical word and parity convention](commonnex/raw-bits-formats.md#gps-and-qzss-lnav):
the ten words already contain receiver-deinverted data. Do not deinvert twice or
apply an over-the-air parity checker to normalized parity bits.

Proposed common fields for decoded occurrences:

| Field | Type | Meaning |
| --- | --- | --- |
| `setup_id` | string | Input station identity |
| `satellite_system`, `satellite_number` | string, uint16 | Broadcasting satellite, not an almanac's subject satellite |
| `message_family` | string | Original CommonNEX family |
| `bitstream_source` | list<string> | Original canonical signal contributors, not UBX/SBF transport identity |
| `nav_epoch_gpst` | decimal128(38,12)? | Unmodified input navigation context |
| `subframe_id` | uint8 | Decoded subframe identifier |
| `data_id_raw`, `sv_id_raw` | uint8? | SF4/5 wire fields; not necessarily a satellite identity |
| `page_number` | uint8? | Only when unambiguously established; repeated SV IDs do not uniquely identify a page |
| `tow_count` | uint32 | Raw HOW count, not the receiver timestamp |
| `alert`, `antispoof` | bool | HOW indicators retained independently of integrity checks |
| `checks` | typed list | Preserve input check scope/result; character validity is not parity validity |

Meaningful TLM fields can be exposed, but reserved bits need not be duplicated.
HOW refers to a transmitted subframe boundary, not observation
time. Resolving a week must use trustworthy input context, never the host date.
Unknown time can still accompany an extracted message, but does not establish
time-applicable snapshot eligibility.

Assembled records carry broadcasting identity, family, signal contributors and
first/last contributing navigation-context times. Completion GPST is nullable
when chronology cannot be established. The initial assembly policy keeps
stations, broadcasters and distinct signal-source sets separate; do not merge
pages from different satellites/signals merely because their contents match.
No per-row references back into CommonNEX files are required.

## LNAV coverage and dispatch

The following inventory comes from G200 sections 20.3.3.1-5 and Table 20-V,
and QPNT section 4.1.2, especially Tables 4.1.2-2 and 4.1.2-16 (sources below).
All entries are definition targets, not implemented decoder claims.

| GPS location | SV ID | Output category |
| --- | --- | --- |
| SF1 | n/a | Clock, health, accuracy, group delay and control fields |
| SF2/3 | n/a | Ephemeris parts |
| SF4 pages 2-5, 7-10 | 25-32 | Individual almanacs |
| SF5 pages 1-24 | 1-24 | Individual almanacs |
| SF5 page 25 | 51 | Almanac epoch and health |
| SF4 page 25 | 63 | Configuration, anti-spoof and health |
| SF4 page 13 | 52 | NMCT |
| SF4 page 17 | 55 | Special message |
| SF4 page 18 | 56 | Ionosphere and UTC parameters |
| SF4 pages 1,6,11,16,21; 12,24; 14,15; 19,20,22,23 | 57; 62; 53,54; 58,59,60,61 | Reserved/system-use payloads |

G200 calls the normal Data ID "number two" but encodes it as binary `01`.
Keep the raw field value `1`; do not encode that prose label as numeric `2`.
Dummy SV ID zero does not identify an actual almanac satellite. Expanded-PRN
LNAV rules in G200 section 40 require a separate dispatch audit before claiming
coverage beyond this section-20 inventory.

QZSS SF1-3 share the basic clock/ephemeris layout. SF4/5 dispatch uses raw Data
ID `3` and the following SV IDs, not the GPS fixed page schedule:

| QZSS SV ID | Output category |
| --- | --- |
| 0 | Test-mode payload |
| 1-10 | Individual QZS almanacs |
| 51 | Almanac epoch and health |
| 55 | Special message |
| 56 | Wide-area ionosphere and UTC parameters |
| 60 | QZNMA payload; extraction does not imply authentication verification |
| 61 | Japan-area ionosphere and UTC parameters |

Keep QZSS L1C/A and L1C/B sources distinct. QPNT's health, TGD, fit rules,
almanac reference eccentricity/inclination, UTC realization and PRN mappings
are not interchangeable with GPS. In particular, a GPS TGD signal reference
must not be assigned to QZSS, and QZSS does not transmit GPS NMCT. See QPNT
4.1.2.7; later revisions need their own coverage review.

## First assembled type: LNAV ephemeris

Proposed parameter struct, shared only where field meanings agree:

| Fields | Type / unit | Rule |
| --- | --- | --- |
| `week_raw` | uint16 | Preserve the transmitted modulo week |
| `week_resolved` | int64? | Full system week only when resolvable |
| `toe_s`, `toc_s` | decimal128(38,12) seconds of native week | Preserve native reference values |
| `toe_gpst`, `toc_gpst` | decimal128(38,12)? | Separately resolved GPST coordinates |
| `iodc` | uint16 | Full clock issue field |
| `iode_sf2`, `iode_sf3` | uint8 | Retain both transmitted issue fields |
| `sqrt_a` | float64, sqrt(m) | Do not redundantly store semi-major axis |
| `eccentricity` | float64 | Dimensionless |
| `m0_rad`, `omega0_rad`, `i0_rad`, `omega_rad` | float64, rad | Convert semicircles explicitly |
| `delta_n_rad_s`, `omega_dot_rad_s`, `idot_rad_s` | float64, rad/s | Scaled broadcast rates |
| `cuc_rad`, `cus_rad`, `cic_rad`, `cis_rad` | float64, rad | Harmonic angle corrections |
| `crc_m`, `crs_m` | float64, m | Harmonic radius corrections |
| `af0_s`, `tgd_s` | decimal128(38,12), s | Round ties to even; this storage rounding is not measurement accuracy |
| `af1_s_s`, `af2_s_s2` | float64, s/s and s/s² | Clock polynomial coefficients, not timestamps |
| `ura_index`, `health_raw` | uint8 | Interpret with system-specific definitions |
| `code_on_l2_raw` | uint8 | Preserve wire value, including system-fixed values |
| `l2_p_data_flag`, `fit_interval_flag` | bool | Preserve wire flags, not guessed capability |
| `aodo_raw` | uint8 | Preserve sentinel values; not always a usable duration |

TGD must carry its system/signal reference semantics in the typed output
contract. This is not a generic DCB for arbitrary signal pairs. Preserve HOW
flags per contributing subframe rather than overwriting them with the last page.
The original RawBits remains the reference for omitted reserved bits; do not
copy them merely to recreate the entire wire payload in decoded form.

Assembly contract:

1. Require one fresh, accepted SF1, SF2 and SF3 from the same source assembly.
   Match both IODEs to the low eight bits of IODC. An issue match alone is not
   sufficient across unbounded time or a discontinuity.
2. Resolve reference times coherently around trustworthy GPST context and week
   rollover. Check family-specific ranges and time consistency without requiring
   TOE and TOC to be numerically identical.
3. Emit a complete parameter occurrence even if health makes it unsuitable for
   positioning; completeness, integrity, health and applicability are distinct.
   Failed integrity/default/error messages do not become usable candidates.
4. After emission, a new occurrence requires a new full set of contributing
   subframes. Repetitions of just one component do not complete another set.
5. Retain completed candidates independently of the assembly workspace. Snapshot
   eligibility uses documented clock/orbit applicability, availability by the
   target time and health semantics. Do not use a universal two-hour timeout or
   treat a backend's support flag as scientific validity.

Health and parameter applicability follow each system/message standard, not a
project-wide invented lifetime or blanket satellite-health boolean. Preserve
signal-specific scope. Undefined lifetimes must not become claimed validity.

Assembly timeout is a separate reception policy. For periodically transmitted
sets, use three complete-set transmission cycles as the default design rule;
where only a maximum transmission interval is specified, derive the collection
bound from the required components. This is project policy, not an ICD mandate.
Start at the first accepted fragment; duplicates do not extend the deadline.
Irregularly scheduled types require their own bound, not a fictional CNAV-wide
period. Single-message outputs require no multi-message assembly timeout.
Numeric limits, interval derivations, candidate caps and the precise standards
mapping remain pre-implementation work.

## Other message contracts

Individual almanac entries can be emitted before a complete constellation set.
A set needs coherent reference epoch, associated health/configuration evidence
and an explicit membership/completion rule. Never require "32 healthy satellites"
or infer QZSS membership from receiving ten arbitrary pages. Unknown membership
means no complete-set claim. The exact system-specific membership rules remain
open; single-entry output must not wait for that definition.

A source-local almanac MessageOutput requires every required page to have been
received at least once within the assembly timeout, with consistent reference
epoch/version evidence and no conflicting content. Emit once, clear that
assembly, and collect the next occurrence from scratch, even if its eventual
contents are identical. This does not clear snapshot candidates. An epoch change
or content conflict prevents combining the fragments into one completed set.
WNa/toa are reference identifiers, not a universally unique revision counter;
G200 20.3.3.5.2.2 explicitly permits changed content at upload cutover with the
same toa. Compare relevant payload fields, not changing HOW timestamps.

Dummy/unconfigured slots can establish that a required slot was received and
is empty, but are omitted from decoded satellite entries. Missing reception
does not prove an empty slot. Unhealthy configured satellites remain meaningful
entries, with their health retained independently of collection completeness.

QZSS individual almanacs are complete single-message outputs; do not wait for a
full source-local constellation collection. QPNT-006 gives a maximum almanac
transmission interval of 600 seconds, but no usable full-set boundary/membership
marker. Consequently no 1,800-second full-set assembly timer is needed. SV ID
zero is test mode and cannot count as a missing satellite's dummy page.
Cross-broadcaster QZSS almanac views belong only to snapshots.

GPS/QZSS ephemeris uses a 90-second assembly timeout. GPS section-20 complete
almanac collection uses 2,250 seconds (three 750-second cycles). These are
reception policies, not parameter validity periods.

Special messages expose the full 22-byte payload as binary, with optional safe
display text. For GPS, the payload is Word 3 bits 9-24, Words 4-9 data bits and
Word 10 bits 1-16. Map the ICD degree character `0xF8` explicitly; escape other
unrecognized bytes without truncating spaces/NULs or rejecting the occurrence.
Do not concatenate successive special messages or infer encryption semantics.

Decoded outputs prioritize useful semantic information and selected research
payloads, not full bitstream replication. Reserved bits, padding and dummy data
are normally omitted. Explicit research targets, such as special messages or
authentication payloads, may expose opaque bytes. Extraction is not
authentication verification, decryption or a time-valid correction claim.
Text-like messages need not appear in the current-parameter snapshot.

Input discontinuity clears incomplete assemblies. Complete parameter candidates
follow their own expiry/update rules, not file or midnight boundaries. A new
station requires a new decoder state. Checkpointing incomplete messages is not
planned. Output and assembly buffers must be bounded with explicit overflow
behavior, not silent loss of completed MessageOutput records.

## Definition and implementation checklist

- [x] Establish decoder ownership, separate MessageOutput/Snapshot semantics,
  source preservation and backend-independent typed outputs.
- [x] Inventory standard GPS section-20 and QZSS LNAV dispatch differences.
- [x] Draft common occurrence fields and the assembled ephemeris parameter set.
- [x] Define source-local fresh-set MessageOutput cycles, semantic-only outputs,
  snapshot-only cross-broadcaster aggregation and model-based normalization.
- [x] Define snapshot bundle categories and almanac candidate/conflict semantics.
- [x] Select standards-based health/applicability and the three-cycle assembly
  policy for periodic sets, distinct from parameter validity.
- [ ] Finalize subframe field schemas, check acceptance, exact time-resolution
  rules, health/fit applicability, buffer bounds and assembly time limits.
- [ ] Finalize almanac Arrow schemas, expected membership, rollover and numeric
  collection deadlines for each supported schedule.
- [ ] Define ionosphere/UTC, NMCT, health/configuration, special-message and
  QZNMA extraction schemas; review expanded-PRN and historical ICD variants.
- [x] Replace the proposed completeness watermark with navigation-context,
  input-order snapshot boundaries; expose the first typed batch/JSONL API.
- [x] Implement source-local SF1-3 assemblies, GPS almanac cycles, QZSS single
  almanac entries and snapshot-only cross-broadcaster almanac aggregation.
- [ ] Complete scientific applicability and normalized signal-health mapping
  for all retained parameter categories; UNKNOWN is not valid coverage.
- [ ] Implement against canonical RawBits, reusing existing decoding mechanisms
  where correct; retire duplicate processing paths after parity verification.
- [ ] Validate GPS/QZSS recorded samples against independent field references,
  including issue transitions, repeated sets, week rollover and interruptions.
  No automated test-suite expansion is implied by this checklist.

## Sources

- **G200:** [IS-GPS-200N](https://archive.gps.gov/technical/icwg/IS-GPS-200N.pdf),
  sections 20.3.3.1-5, Tables 20-I/III/V, and section 40 for expanded PRNs.
- **QPNT:** [IS-QZSS-PNT-006](https://qzss.go.jp/en/technical/download/pdf/ps-is-qzss/is-qzss-pnt-006.pdf),
  sections 4.1 and 5.6-5.7. This is the reviewed edition, not a claim that future
  revisions or all receiver firmware have been verified.
