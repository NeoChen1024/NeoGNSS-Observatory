# Setup JSON metadata

Status: v0 with an implemented [single-station initializer](importer.md#initialize),
basic field validation, vendor-config copying and ANTEX selection. This is not
a complete RINEX importer or machine-readable JSON Schema.

[Overview](overview.md) | [Core identities](core.md#context-and-identity)

## Purpose and field groups

The ParquetNEX directory initializer imports `setup.json` and its referenced
vendor configuration file once. Daily UBX/SBF/RINEX inputs do not carry or
recopy these files. See [storage initialization](parquetnex.md#storage-initialization).
JSON describes one logical station with one receiver and one antenna, not every
piece of dataset metadata. Different receiver/antenna observation sources use
separate logical stations. Two recording paths from the same receiver/antenna
may feed the same station, subject to explicit overlap handling; no Stream
registry or named-antenna dictionary is required. Hardware/configuration changes
that affect interpretation require a new Setup rather than rewriting old metadata.

Scope is fixed base stations with upright antennas. Mobile operation, tilted
installations, vehicle body frames and center-of-mass metadata are out of scope.

## Strings and RINEX interoperability

This section applies the [CommonNEX-wide interoperability policy](overview.md#rinex-interoperability-and-strings)
to Setup JSON; Unicode support and asymmetric import/export guarantees are not
limited to this file.

Encode `setup.json` as UTF-8 JSON. String values
may contain Unicode and special characters, with normal JSON escaping. Do not
inherit RINEX fixed-column widths, ASCII-only constraints or arbitrary truncation.
Do not silently transliterate, normalize Unicode, case-fold or sanitize values.
JSON property names and
standardized external identifiers retain their specified spelling.

Preserve imported RINEX metadata content without loss; retain source-specific
header information outside Setup where appropriate. This does not require
reproducing fixed-column padding in normalized fields. Setup can express more
than RINEX, so lossless RINEX export is not guaranteed. A future exporter must
report unrepresentable values and require an explicit conversion policy rather
than silently truncating them. Permissive strings do not remove field semantics,
reference validation or the same-directory filename constraint on `vendor_config`.

The selected metadata groups are below. The complete RINEX header import mapping
remains a separate task; initialization does not read RINEX observations.

| Group | Contents |
| --- | --- |
| `setup_id` | Required nonempty free-form string identifying this Setup; not a filename or RINEX marker name |
| Marker | RINEX-like marker name, number and type; coordinates with frame, units and position basis |
| `observer`, `agency` | Optional observer name and responsible organization strings |
| `comment` | Optional free-text station notes, including station-information URLs |
| `receiver` | `vendor`, `model`, `rinex_name`, `serial_number`, `firmware_version` and optional `comment` |
| `antenna` | Required object for the single antenna; type, radome, serial, comment and optional calibration companion |
| Installation | `antenna.arp_offset_neu_m` and `antenna.azimuth_deg` |
| Tracking | Declared constellations and signals per constellation; receiver measurement rate where known |
| `epoch_period_s` | Required decimal string: strictly positive Duration in seconds |
| `antenna.feed_line` | Optional object with free-form `type` and finite nonnegative `length_m` |
| `vendor_config` | Optional filename of a configuration file beside `setup.json` |

Do not infer cable delay from type or length or add an independent cable-delay
field. Receiver compensation settings remain in the vendor configuration.
The specification does not prescribe that file's format or require the importer
to decode every vendor format. The pilot optionally interprets the Septentrio
tracking command as described below. It should allow the original receiver settings to be recovered,
or be directly applicable to a compatible receiver. Keep its contents out of
`setup.json`. No automatic configuration tracking, dump comparison or update
service is required; differences in transport/logging settings do not define
new Setups merely because the dump differs.

Unknown metadata remains absent/null, not an invented value. Declared signal
configuration is distinct from actual signal coverage. Consumers inspect it
before reading observations: for example, an L1/L2-only algorithm can reject a
known GPS L1/L5-only source immediately. Unknown configuration is not proof of
incompatibility, and enabled signals do not guarantee measurements at every
epoch. The tracking representation and unknown/disabled distinction are defined below.

RINEX source headers, comments, conversion details and observation events do
not all belong in Setup. Map them to source metadata or the appropriate record
family. RINEX-like observation coverage plus extra receiver setup information
is the intended superset; lossless RINEX import still requires explicit field,
correction and event mappings rather than merely retaining a JSON container.

## Setup identity and RINEX marker identity

`setup_id` is a nonempty free-form UTF-8 string. Spaces, punctuation, slashes
and Unicode are allowed; preserve the value exactly. It is an identity value,
not a filesystem path component. The initializer's output path is supplied
separately and must never be constructed by joining an untrusted `setup_id`.

RINEX-compatible marker metadata is separate: `marker.name`, `marker.number`
and `marker.type` correspond to `MARKER NAME`, `MARKER NUMBER` and `MARKER TYPE`.
Do not copy `setup_id` into these fields implicitly. Unknown marker metadata
may be absent/null. A station's marker can remain the same across different
receiver configurations/Setups. Marker strings follow the same permissive
string policy: compatibility means mapped semantics, not mandatory RINEX
column widths or guaranteed lossless export.

```json
{
  "setup_id": "Roof / mosaic-X5 - configuration A",
  "comment": "Fixed station installation notes.",
  "marker": {
    "name": "ROOF",
    "number": "0001",
    "type": "GEODETIC"
  }
}
```

## Tracking declaration and signal names

`tracking` is an optional object keyed by RINEX satellite-system identifiers:
`G` (GPS), `E` (Galileo), `C` (BeiDou), `J` (QZSS), and `S` (SBAS).
GLONASS and NavIC are intentionally out of scope.
Values are lists of system-specific, two-character RINEX band/attribute codes,
the same vocabulary used by Observation `signal`. Do not include the observable
prefix: `1C`, not `C1C`, `L1C`, `D1C` or `S1C`. The system is indispensable:
GPS `1C` and Galileo `1C` do not identify the same signal.

- Missing/null `tracking`: unknown configuration.
- Missing/null system entry: that system's configuration is unknown.
- `[]`: explicitly no enabled measurement signals for that system.
- A nonempty list: the complete set of possible measurement codes allowed by
  the declared tracking configuration; not an observation inventory. A receiver
  can switch between configured components without producing them simultaneously.
- Lists have no ordering significance and contain no duplicates. No extra
  completeness flag is required. Do not publish a partially understood config
  as a complete list or turn an unrecognized token into an empty list.

Example mosaic-X5 declaration (illustrative, not a promise of actual coverage):

```json
{
  "tracking": {
    "G": ["1C", "1W", "2L", "2W", "5Q"],
    "E": ["1C", "5Q", "6B", "6C", "7Q", "8Q"],
    "C": ["1P", "2I", "5P", "6I", "7D", "7I"],
    "J": ["1C", "1E", "2L", "5Q"],
    "S": ["1C", "5I"]
  }
}
```

### Septentrio vendor-config import

When `receiver.vendor` is `Septentrio` (case-insensitive recognition only;
stored strings are unchanged), the pilot
reads the referenced/copied UTF-8 config and recognizes `setSignalTracking`,
its `snt` alias, and a `SignalTracking` response line. It only reads these
comma-separated assignments; it does not execute commands, apply configuration,
or print unrelated config contents. The last complete assignment wins.
There is no model-name gate; `receiver.model` may be another Septentrio model
or unknown. Recognition depends on the documented command and signal tokens,
not an assertion that every receiver supports every signal in the table.
`setSignalUsage` is a navigation-solution selection, not a tracking declaration;
do not use it to populate `tracking`. Satellite selection and output-message
filters can further reduce coverage but are not this declaration.

The adapter follows the mosaic-X5 firmware 4.15.0 Reference Guide,
sections 2.2.1 (pilot/data), 3 (`setSignalTracking`) and 4.1.10 (RINEX codes):

| Config signal | System | Measurement code(s) | Meaning |
| --- | --- | --- | --- |
| `GPSL1CA` | G | `1C` | L1 C/A |
| `GPSL1PY` | G | `1W` | L1 P(Y), receiver measurement convention |
| `GPSL2PY` | G | `2W` | L2 P(Y), receiver measurement convention |
| `GPSL2C` | G | `2L` | L2C-L pilot |
| `GPSL5` | G | `5Q` | L5 Q pilot |
| `GALE1BC` | E | `1C` | E1 C pilot |
| `GALE6BC` | E | `6B`, `6C` | E6 B or C, selected by receiver |
| `GALE5a` | E | `5Q` | E5a Q pilot |
| `GALE5b` | E | `7Q` | E5b Q pilot |
| `GALE5` | E | `8Q` | E5 AltBOC Q |
| `GEOL1` | S | `1C` | SBAS L1 C/A |
| `GEOL5` | S | `5I` | SBAS L5 I |
| `BDSB1I` | C | `2I` | B1I, not B1C |
| `BDSB2I` | C | `7I` | B2I |
| `BDSB3I` | C | `6I` | B3I |
| `BDSB1C` | C | `1P` | B1C pilot |
| `BDSB2a` | C | `5P` | B2a pilot |
| `BDSB2b` | C | `7D` | B2b data |
| `QZSL1CA` | J | `1C` | L1 C/A |
| `QZSL2C` | J | `2L` | L2C-L pilot |
| `QZSL5` | J | `5Q` | L5 Q pilot |
| `QZSL1CB` | J | `1E` | L1 C/B, not L1C |

Source: [Septentrio mosaic-X5 firmware 4.15.0 Reference Guide, SparkFun mirror](https://docs.sparkfun.com/SparkFun_GNSS_mosaic-X5/assets/component_documentation/firmware/mosaic-X5_Firmware_v4.15.0_Reference_Guide.pdf).
These mappings describe this receiver's Measurements output, not all legal
RINEX signals or RawBits contributors. For example, `GALE1BC` does not promise
an E1 B observation; Galileo E6 can fall back from C to B when C is encrypted.
Actual Observation codes remain authoritative.

Constellation aliases and `all` depend on receiver/firmware capabilities and
are not expanded using a mosaic-X5 inventory. Supply an expanded signal list
or explicit Setup tracking instead; otherwise warn and do not infer tracking.
Known out-of-scope constellation tokens are ignored. GPS P(Y) dependencies
are applied: L2 P(Y) needs L1 C/A; L1 P(Y) needs both. The original dump stays
unchanged, including ineffective enabled options. Unknown tokens, incomplete
assignments, missing tracking commands or non-text dumps produce a warning
and no inferred declaration, rather than a partial result. Other vendors
remain opaque and can use manually supplied tracking. New firmware tokens need
an explicit adapter mapping; this is not a generic config interpreter.

Missing/null systems are filled from the recognized complete assignment.
Explicit system lists (including `[]`) take precedence; differences produce a
warning, never a silent overwrite. To rely wholly on vendor-config inference,
leave `tracking` absent/null in the input Setup. Otherwise the input Setup may
intentionally describe a narrower exported source. Enabled signals do not
assert receiver license availability, satellite visibility, successful tracking,
or output presence. Do not extract settings by parsing free-text comments.

## Nominal epoch period

Top-level `epoch_period_s` declares the nominal observation cadence expected
for this Setup, in seconds. Its logical type is Core `Duration`, represented
in JSON as a decimal string, not a JSON number. Examples are `"1.000000000000"`
for 1 Hz, `"0.100000000000"` for 10 Hz and `"30.000000000000"` for one observation
every 30 seconds. This field is required for every Setup, including RawBits-only
stations: provide the configured nominal receiver epoch period even when no
observations are exported. Missing/null values are rejected; do not infer a
default from input timestamps. It is not a telemetry/message transmission
interval, reference-epoch interval, or a guarantee of gapless observations.
Recording-source decimation may differ and must not rewrite receiver cadence.

Parse the string directly as decimal without a binary64 intermediate. Writers
emit ordinary decimal notation with exactly 12 fractional digits. Readers may
accept fewer fractional digits and pad zeros; reject exponent notation,
nonpositive values, out-of-range values and more than 12 fractional digits.

```json
{
  "epoch_period_s": "1.000000000000"
}
```

A true 30 Hz cadence has a period of 1/30 s, represented here as
`"0.033333333333"`, not 0.033 s. A decimal here is a
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
| `marker.position_xyz_m` | Object with finite `x`, `y`, `z`, ECEF meters; whole object null when unknown | Approximate marker position |
| `marker.reference_frame` | Optional string identifying the known coordinate frame/realization | Additional coordinate context |
| `marker.position_basis` | `approximate` or `surveyed`; absent/null if unknown | Additional position-quality context |
| `antenna.arp_offset_neu_m` | Object with finite `north`, `east`, `up` in meters, from marker to ARP in the marker's local frame | Reorder RINEX H/E/N to N/E/U |
| `antenna.azimuth_deg` | Optional finite number in `[0, 360)`, clockwise from true north to the antenna zero-direction mark | Antenna zero-direction azimuth |

Unknown fields remain absent/null, not zero coordinates or offsets. A receiver
position estimate is not a surveyed position. Do not guess the reference-frame
realization. Zero azimuth means the mark points north; absent/null means unknown.
There are no tilt angles or boresight vectors. Antenna type, radome and serial
remain in the single antenna object. Imported metadata conflicts
must not silently overwrite an initialized Setup.

## Receiver fields

`receiver` is a JSON object with the following string fields. Preserve unknown
values as null or absent; never invent equipment identity from a filename.
Serial numbers are strings, including when they contain only digits.

| Field | Meaning |
| --- | --- |
| `vendor` | Receiver manufacturer/vendor |
| `model` | Receiver model/type designation |
| `rinex_name` | Optional RINEX `REC # / TYPE / VERS` TYPE value, independent of vendor/model |
| `serial_number` | Receiver serial identifier |
| `firmware_version` | Reported internal software/firmware version |
| `comment` | Optional free-text receiver-specific notes, including multiline text |

```json
{
  "receiver": {
    "vendor": "Septentrio",
    "model": "mosaic-X5",
    "rinex_name": "SEPT MOSAIC-X5",
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
vendor field. Keep an imported TYPE intact as `rinex_name`; do not heuristically
split it to fabricate vendor/model, or concatenate vendor/model to guess a
standardized name. Vendor-config recognition uses `vendor`, not `rinex_name`.
No RINEX column-width restriction is imposed on JSON strings; exporting is a
separate representability check. Source metadata preserves the original header text.

## Single antenna and feed line

`antenna` is one object, never a list or named dictionary. Its string fields
`type`, `radome`, `serial_number`, `comment`, and `calibration_file` are optional;
unknown values remain absent/null. Native source-antenna selection belongs to
`run --source-antenna`, not an antenna identity registry. Import a different
physical antenna into a different logical station.

RINEX `ANT # / TYPE` has a serial field and a combined antenna/radome TYPE field.
The radome occupies positions 17-20 of TYPE, not a separate RINEX header record.
JSON separates `type` and `radome` for unambiguous catalog matching. Follow the
[IGS equipment naming convention](https://files.igs.org/pub/station/general/rcvr_ant.tab).
Unknown radome is not `NONE`; do not guess from an antenna's visible housing.
`BEIBT800S`, for example, can use a type-mean calibration without a matching
individual serial, once the applicable catalog radome is confirmed.

`antenna.feed_line.type` is free-form text, including manufacturer/model or
installation notes. `length_m` is a finite, nonnegative floating-point length
in meters, nullable when unknown. Do not compute delay from either field.

The [complete example](../../config/commonnex-setup.example.json) includes marker
XYZ, marker-to-ARP N/E/U, receiver RINEX name, feed line and optional companions.
Its coordinates, offsets and length are illustrative, not surveyed values.
Use null for an unknown vector rather than partial vectors or invented zeros.

## Antenna installation versus calibration

Store `azimuth_deg` in `antenna`, not at receiver scope.
The installation assumes upright antennas; unknown azimuth must not silently
become zero. Tilted or mobile installation metadata cannot be represented as
supported fixed-station geometry merely by discarding its additional components.

Marker-to-ARP offsets describe the physical installation and remain in Setup.
Phase-center offsets (PCO) and variations (PCV) are calibration data read from
the selected ANTEX companion by downstream processing, not expanded into
`setup.json`. ANTEX supplies frequency-dependent PCO and direction-dependent
PCV; see the [IGS antenna resources](https://igs.org/wg/antenna/) and
[NGS calibration FAQ](https://nweb.ngs.noaa.gov/ANTCAL/FAQ.xhtml).

Preserve antenna type, radome and serial identity for calibration selection.
Downstream selection must handle calibration coverage and applicable models
explicitly; an unmatched antenna/frequency is not a calibrated zero correction.
Installation orientation is still needed to interpret directional calibration.
Initializing with calibration does not apply corrections to observations. The
downstream engine must check time/frequency coverage and product-frame compatibility.

### ANTEX selection during initialization

Repeat `--antenna-catalog PATH` in preference order. A path may be an absolute
ANTEX 1.4 catalog (`.atx` or `.atx.gz`) or a one-antenna file. If no catalog is
specified and `antenna.calibration_file` names an input companion, read it by
the same rules. Without either input, initialization succeeds without calibration.
Explicit catalogs override the input calibration-file reference.

- Require exact standard `antenna.type` and four-character `antenna.radome`.
  Catalog selection cannot match arbitrary Unicode/nonstandard equipment names;
  the JSON remains permissive, but the requested selection fails explicitly.
- A mass-produced antenna normally uses a type-mean record (blank ANTEX serial),
  regardless of whether its hardware serial is known. A matching individual
  record takes precedence if supplied; another antenna's serial never matches.
- Catalog order breaks precedence within that class. Do not merge frequencies
  or calibration values between catalogs. A one-antenna input must still match.
- Preserve all complete selected records with disjoint validity intervals.
  Conflicting/overlapping alternatives in the chosen class are an error;
  byte-identical duplicate blocks are emitted once. Do not infer validity from
  filenames, and do not select by today's date during initialization.
- Copy the chosen catalog's complete original header and full
  `START OF ANTENNA` through `END OF ANTENNA` byte ranges into `antenna.atx`.
  Preserve every frequency, PCV grid, PCO, RMS field, comment and validity bound.
  Do not restrict frequencies using `tracking`, convert units or rewrite numbers.
- Reject unsupported ANTEX versions, relative/unknown calibration headers,
  truncated records, missing matches or ambiguous selection. Sources remain
  unchanged. Missing calibration/frequency never means zero correction.
- Store `"calibration_file": "antenna.atx"` in the output `antenna` object.
  Reserve this filename so the vendor config cannot overwrite it.

The selected record describes only the receiver antenna. Satellite antenna
calibrations and processing-time product selection remain engine inputs.

A phase-center declaration in imported RINEX remains source metadata rather
than a new Setup PCO field. Preserve it and any applied-correction declarations
so downstream processing can distinguish source information from its selected
ANTEX model and avoid applying a correction twice. This boundary does not
establish a fallback or precedence policy for conflicting calibration sources.

## RINEX coverage review

Compared with [RINEX 4.02](https://files.igs.org/pub/data/format/rinex_4.02.pdf),
sections 5.2 and 8.2, the following Setup decisions are settled:

- [x] Include observer/agency; store station-information links in `comment`.
- [x] Place orientation in the single antenna entry and obtain PCO/PCV from ANTEX.
- [x] Use true-north clockwise azimuth for upright antennas only.
- [x] Exclude mobile operation, tilt, body frames and center of mass.
- [x] Define marker identity, ECEF XYZ and named marker-to-ARP N/E/U offsets.
- [x] Allow Unicode without RINEX width restrictions; lossless export is not required.
- [x] Keep vendor configuration in an initialization-time companion file,
  and declare nominal period using decimal seconds.

Implementation status and remaining work:

- [x] Initialize a single station directory with Setup and optional companion files.
- [x] Preserve free-form Setup identity separately from RINEX marker identity.
- [x] Define tracking signal examples and model-independent Septentrio vendor-config inference.
- [x] Validate period, tracking shape, strings, vector components, azimuth and cable length.
- [x] Use single-station storage with `setup_id` on native records and no Stream file.
- [x] Select and preserve complete ANTEX receiver-calibration records during init.
- [ ] Implement RINEX metadata import and downstream use of the selected companion.

File metadata (producer, dates, comments, DOI/license), actual observation
inventory/interval/coverage, time interpretation, and applied DCB/PCV,
scale and phase corrections belong to source/observation mappings, not fixed
Setup fields. Their import remains to be specified; do not discard them or
infer them from declared tracking configuration. Observation clock-offset
correction declarations are instead used to reject corrected RINEX input;
they do not establish a supported correction mode. GLONASS mapping remains
outside v0 scope. This is not a complete lossless RINEX mapping claim.
