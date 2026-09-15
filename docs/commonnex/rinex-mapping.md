# RINEX-specific observation information

Status: selected design decisions; adapters and complete header mappings remain
to be implemented. [Overview](overview.md) | [Core](core.md)

Support standard RINEX 3.x/4.x content for in-scope constellations. RINEX 2,
GLONASS and NavIC are excluded. Unknown observation codes are reported, not
guessed or retained through a generic vendor observation schema.

Preservation is subject to the [conditional import goal](overview.md#rinex-interoperability-and-strings).
Unmappable or insufficiently timed content may be discarded with diagnostics
and counts; do not silently substitute invented values. In particular, ION
header coefficients without transmission time or reliable acquisition context
are excluded, not copied into an untimed Parquet collection. This does not
discard usable observations or ephemerides from the same source.

## Source-only fields

Use `rinex_` for information whose meaning is specific to RINEX encoding or
source declarations. Only RINEX import populates these fields; UBX/SBF/RTCM3
import must not synthesize them. Optional source fields are null when absent.

| Field | Type | Location and meaning |
| --- | --- | --- |
| `rinex_ssi` | uint8? | Corresponding observable quality; 1-9, zero/blank becomes null |
| `rinex_lli` | uint8? | Phase tracking; original 0-7 bitmask, blank becomes null |
| `rinex_epoch_flag` | uint8? | Epoch-scoped Events for ordinary observation epochs, or the applicable special-event record |
| `rinex_version` | string | RINEX source metadata, e.g. `3.04`, never a float or repeated on every observation |
| `rinex_program` | string? | Source metadata: generating program |
| `rinex_run_by` | string? | Source metadata: file generator's run-by declaration |
| `rinex_comments` | list<string> | Ordered COMMENT contents; empty when none are present |

Each comment element preserves one COMMENT record's text. Remove fixed-column
padding, but do not join lines, deduplicate, reorder or otherwise rewrite the
content. List elements are non-null strings. Source comments do not overwrite
manually initialized Setup comments. Source metadata is not a provenance bundle
or an execution-environment inventory.

Header changes, external events and cycle-slip records require separate typed
event mappings, not fabricated ordinary C/L/D/S rows. If retained, parsed header
updates use `rinex_header_updates` with explicit applicability; the detailed
update/event payload schemas remain pending. Do not copy a full header per epoch.
Legitimate untimed special events retain their meaning.

## Common semantics, not RINEX-only fields

### Unsupported phase-shift declaration

`SYS / PHASE SHIFT` is not supported as CommonNEX metadata or as an import
correction mechanism. RINEX 4.02 section 5.2.12 and Table A2 mark this header
strongly deprecated and instruct decoders/encoders to ignore it.

For the project's RINEX 3.x/4.x import scope:

- Ignore this header record and its continuation lines, including in header
  updates. Do not create a `rinex_phase_shift` field, Event or correction state.
- Preserve the exported carrier-phase observations; do not apply or undo a
  phase shift based on this declaration.
- Do not reject a file solely because the header is present or contains a
  nonzero value. It describes shifts used when generating the observations,
  not an instruction to shift them again. Its omission from CommonNEX is an
  explicit limit of metadata preservation; the raw archive retains it.

This exclusion does not remove carrier-phase signal conventions or the
independent half-cycle/LLI fields. It is also separate from the rejection of
applied receiver clock-offset correction below.

### Supported interpretation

Satellite/signal identity, C/N0,
phase conventions, applied DCB/PCV metadata and station/receiver/antenna metadata
retain common names. A field does not become RINEX-specific merely because
RINEX is currently its only implemented source. Retain scientifically necessary
DCB/PCV declarations with explicit scope. Observation clock-offset correction
is excluded: reject `RCV CLOCK OFFS APPL=1`, accept zero, and interpret missing
headers using the supported version's default. Do not undo corrected input.
An optional `rinex_receiver_clock_offset_s: TimeDelta?` belongs to a separate
epoch-local auxiliary record; never apply it or fill it from NAV-CLOCK/PVT.
RINEX input remains unimplemented, so these are adapter requirements, not a
claim that the current UBX/SBF CLI parses or validates RINEX headers.

Decode storage scale factors into physical observable values. Do not propagate
ASCII widths, layout order or duplicate counts into the common observation
schema. Successfully mapped observation codes need no redundant per-row
`rinex_observation_code`. SSI is not C/N0 and must not generate a synthetic S
observable. Only DBHZ S observables are supported; an explicitly different unit
raises an unsupported-unit error. Missing headers follow supported-version rules.

The source archive preserves original encoding. These decisions preserve the
supported scientific interpretation, not byte-identical RINEX export.

Reference: [RINEX 4.02](https://files.igs.org/pub/data/format/rinex_4.02.pdf).
