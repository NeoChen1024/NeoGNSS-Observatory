# Setup JSON metadata

Status: v0 design draft; selected mappings are documented below, not a complete
machine-readable JSON Schema or an implemented initializer.

[Overview](overview.md) | [Core identities](core.md#context-and-identity)

## Purpose and field groups

The ParquetNEX directory initializer imports `setup.json` and its referenced
vendor configuration file once. Daily UBX/SBF/RINEX inputs do not carry or
recopy these files. See [storage initialization](parquetnex.md#storage-initialization).
JSON describes the station setup, not every piece of dataset metadata.

Scope is fixed base stations with upright antennas. Mobile operation, tilted
installations, vehicle body frames and center-of-mass metadata are out of scope.

## Strings and RINEX interoperability

This section applies the [CommonNEX-wide interoperability policy](overview.md#rinex-interoperability-and-strings)
to Setup JSON; Unicode support and asymmetric import/export guarantees are not
limited to this file.

Encode `setup.json` as UTF-8 JSON. String values and user-defined antenna names
may contain Unicode and special characters, with normal JSON escaping. Do not
inherit RINEX fixed-column widths, ASCII-only constraints or arbitrary truncation.
Do not silently transliterate, normalize Unicode, case-fold or sanitize values.
Antenna references match their dictionary keys exactly. JSON property names and
standardized external identifiers retain their specified spelling.

Preserve imported RINEX metadata content without loss; retain source-specific
header information outside Setup where appropriate. This does not require
reproducing fixed-column padding in normalized fields. Setup can express more
than RINEX, so lossless RINEX export is not guaranteed. A future exporter must
report unrepresentable values and require an explicit conversion policy rather
than silently truncating them. Permissive strings do not remove field semantics,
reference validation or the same-directory filename constraint on `vendor_config`.

The selected metadata groups are below. The named-antenna mapping is defined
below; remaining nested keys and the complete RINEX header mapping remain
implementation review items.

| Group | Contents |
| --- | --- |
| Marker | RINEX-like marker name, number and type; coordinates with frame, units and position basis |
| `observer`, `agency` | Optional observer name and responsible organization strings |
| `comment` | Optional free-text station notes, including station-information URLs |
| `receiver` | Explicit `vendor`, `model`, `serial_number`, `firmware_version` and optional `comment` fields |
| `antennas` | Dictionary of named antenna entries with type/model, radome, serial number and receiver input association |
| Installation (per antenna) | Marker-to-ARP offset and antenna orientation with explicit frames and units |
| Tracking | Declared constellations and signals per constellation; receiver measurement rate where known |
| `epoch_period_ms` | Optional positive finite JSON number: nominal observation epoch period in milliseconds |
| Feed lines (per antenna) | Optional cable type and length with units for that antenna's receiver connection |
| `vendor_config` | Optional filename of a configuration file beside `setup.json` |

Do not infer cable delay from type or length or add an independent cable-delay
field. Receiver compensation settings remain in the vendor configuration.
The specification does not prescribe that file's format or require the importer
to decode it. It should allow the original receiver settings to be recovered,
or be directly applicable to a compatible receiver. Keep its contents out of
`setup.json`. No automatic configuration tracking, dump comparison or update
service is required; differences in transport/logging settings do not define
new Setups merely because the dump differs.

Unknown metadata remains absent/null, not an invented value. Declared signal
configuration is distinct from actual signal coverage. Consumers inspect it
before reading observations: for example, an L1/L2-only algorithm can reject a
known GPS L1/L5-only source immediately. Unknown configuration is not proof of
incompatibility, and enabled signals do not guarantee measurements at every
epoch. Exact constellation/signal identifiers and configuration completeness
must be defined so omission is not silently interpreted as disabled.

RINEX source headers, comments, conversion details and observation events do
not all belong in Setup. Map them to source metadata or the appropriate record
family. RINEX-like observation coverage plus extra receiver setup information
is the intended superset; lossless RINEX import still requires explicit field,
correction and event mappings rather than merely retaining a JSON container.

## Nominal epoch period

Top-level `epoch_period_ms` declares the nominal observation cadence expected
for this Setup, in milliseconds. Examples are `1000` for 1 Hz, `100` for 10 Hz
and `30000` for one observation every 30 seconds. Unknown or non-periodic
cadence is absent/null, not zero. It is not a telemetry/message transmission
interval, reference-epoch interval, or a guarantee of gapless observations.
Recording-source decimation may differ and must not rewrite receiver cadence.

Allow a positive finite JSON number rather than requiring integer milliseconds.
A true 30 Hz cadence has a period of 1000/30 ms, not 33 ms. A decimal here is a
nominal approximation, not an exact rational timing representation. This avoids
restricting the format to the rates of currently used receivers. Actual epoch
timestamps remain authoritative: never generate, snap or accumulate timestamps
from this metadata. CommonNEX defines a per-epoch interval tolerance of +/-20%
in the [cadence classification rules](import-policy.md#epoch-interval-classification).
This is a diagnostic threshold, not permission to round timestamps. Preserve an
imported source interval outside Setup as well when its precision or meaning
cannot be represented here exactly.

## Observer, agency and comments

Top-level `observer` and `agency` are optional strings identifying the observer
and responsible organization. Unknown values may be absent or null. `comment`
is an optional free-text string, including multiline notes and station-information
URLs. Do not add a dedicated station URL field or require URL parsing/fetching.
These are station notes, not a container for every imported file's comments.

```json
{
  "observer": "Station operator",
  "agency": "Operating organization",
  "comment": "Station information: https://example.org/station"
}
```

## Marker and installation fields

`marker` describes the fixed station marker, not an antenna phase center.
The selected mapping is:

| JSON field | Type and meaning | RINEX counterpart |
| --- | --- | --- |
| `marker.name` | String station/marker name | Marker name |
| `marker.number` | String marker identifier; preserve leading zeros | Marker number |
| `marker.type` | String marker category | Marker type |
| `marker.position_xyz_m` | Three finite numbers, ECEF X/Y/Z in meters | Approximate marker position |
| `marker.reference_frame` | Optional string identifying the known coordinate frame/realization | Additional coordinate context |
| `marker.position_basis` | `approximate` or `surveyed`; absent/null if unknown | Additional position-quality context |
| `antennas.<name>.arp_offset_hen_m` | Three finite numbers: height/up, east, north in meters, from marker to ARP | Antenna H/E/N offset |
| `antennas.<name>.azimuth_deg` | Optional finite number in `[0, 360)`, clockwise from true north to the antenna zero-direction mark | Antenna zero-direction azimuth |

Unknown fields remain absent/null, not zero coordinates or offsets. A receiver
position estimate is not a surveyed position. Do not guess the reference-frame
realization. Zero azimuth means the mark points north; absent/null means unknown.
There are no tilt angles or boresight vectors. Antenna type, radome, serial and
receiver input remain in each named antenna entry. Imported metadata conflicts
must not silently overwrite an initialized Setup.

## Receiver fields

`receiver` is a JSON object with the following string fields. Preserve unknown
values as null or absent; never invent equipment identity from a filename.
Serial numbers are strings, including when they contain only digits.

| Field | Meaning |
| --- | --- |
| `vendor` | Receiver manufacturer/vendor |
| `model` | Receiver model/type designation |
| `serial_number` | Receiver serial identifier |
| `firmware_version` | Reported internal software/firmware version |
| `comment` | Optional free-text receiver-specific notes, including multiline text |

```json
{
  "receiver": {
    "vendor": "Septentrio",
    "model": "mosaic-X5",
    "serial_number": null,
    "firmware_version": null,
    "comment": "Additional receiver hardware and timing configuration notes."
  }
}
```

Use `receiver.comment` for relevant details not covered by standard fields,
such as an external rubidium (Rb) frequency reference connected to a receiver
that supports it. The example does not assert external-reference support for
the illustrated receiver model. Top-level `comment` describes the station;
`receiver.comment` describes the receiver and its associated configuration.
Comments are descriptive, not machine-readable correction settings: downstream
tools must not infer or apply clock corrections by parsing this free text.

RINEX `REC # / TYPE / VERS` carries number, type and version, not a separate
vendor field. Keep an imported type intact as `model`; do not heuristically
split it to fabricate a vendor. Additional independently known vendor metadata
can populate `vendor`. Source metadata preserves the original header text.

## Named antennas and Stream references

`setup.json` stores `antennas` as a JSON object (dictionary), not a list. Each
key is an `antenna_name`, unique within that Setup. A Stream's string
`antenna_name` references exactly that key in its parent Setup. There is no
additional `antenna_id`, positional index, or repeated `antenna_name` field
inside an antenna entry.

Example antenna mapping (a Setup metadata fragment, not a complete Setup):

```json
{
  "antennas": {
    "main": {
      "type": "ANTENNA_MODEL",
      "serial_number": "12345",
      "receiver_input": "ANT1"
    },
    "aux": {
      "type": "ANTENNA_MODEL",
      "serial_number": "67890",
      "receiver_input": "ANT2"
    }
  }
}
```

The corresponding logical Stream record is:

```json
{
  "stream_id": "main-observations",
  "setup_id": "station-setup",
  "antenna_name": "main"
}
```

- Names are user-defined stable references, not just display labels. Once
  referenced, do not rename or remove the key within the same Setup.
- Dictionary order has no meaning and may change without changing references.
- `receiver_input` preserves the receiver's physical input name or identifier;
  it need not equal the user-defined antenna name.
- Each entry owns its antenna metadata, Marker-to-ARP offset, orientation and
  optional feed line information. These are not shared across all inputs.
- A single-antenna Setup uses the same dictionary representation, for example
  one entry named `main`; no special list or integer-ID representation exists.
- Validate each Stream reference against its parent Setup. A missing name is
  an error, not a reason to select the first antenna or another Setup's entry.

These examples define the antenna mapping and reference names, not the final
serialization location of Stream declarations or all installation subfields.

## Antenna installation versus calibration

Store `azimuth_deg` in the corresponding `antennas` entry, not at receiver scope.
The installation assumes upright antennas; unknown azimuth must not silently
become zero. Tilted or mobile installation metadata cannot be represented as
supported fixed-station geometry merely by discarding its additional components.

Marker-to-ARP offsets describe the physical installation and remain in Setup.
Phase-center offsets (PCO) and variations (PCV) are calibration data read from
external ANTEX products by downstream processing, not manually copied into
`setup.json`. ANTEX supplies frequency-dependent PCO and direction-dependent
PCV; see the [IGS antenna resources](https://igs.org/wg/antenna/) and
[NGS calibration FAQ](https://nweb.ngs.noaa.gov/ANTCAL/FAQ.xhtml).

Preserve antenna type, radome and serial identity for calibration selection.
Downstream selection must handle calibration coverage and applicable models
explicitly; an unmatched antenna/frequency is not a calibrated zero correction.
Installation orientation is still needed to interpret directional calibration.
ANTEX selection belongs to processing configuration, not a change of Setup.

A phase-center declaration in imported RINEX remains source metadata rather
than a new Setup PCO field. Preserve it and any applied-correction declarations
so downstream processing can distinguish source information from its selected
ANTEX model and avoid applying a correction twice. This boundary does not
establish a fallback or precedence policy for conflicting calibration sources.

## RINEX coverage review

Compared with [RINEX 4.02](https://files.igs.org/pub/data/format/rinex_4.02.pdf),
sections 5.2 and 8.2, the following Setup decisions are settled:

- [x] Include observer/agency; store station-information links in `comment`.
- [x] Place orientation in each antenna entry and obtain PCO/PCV from ANTEX.
- [x] Use true-north clockwise azimuth for upright antennas only.
- [x] Exclude mobile operation, tilt, body frames and center of mass.
- [x] Define marker identity, position context and per-antenna H/E/N offsets.
- [x] Allow Unicode without RINEX width restrictions; lossless export is not required.

File metadata (producer, dates, comments, DOI/license), actual observation
inventory/interval/coverage, time interpretation, and applied clock/DCB/PCV,
scale and phase corrections belong to source/observation mappings, not fixed
Setup fields. Their import remains to be specified; do not discard them or
infer them from declared tracking configuration. GLONASS mapping remains
outside v0 scope. This is not a complete lossless RINEX mapping claim.
