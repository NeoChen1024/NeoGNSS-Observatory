# CommonNEX DecodedNav extension

Status: v0 design draft; not an implemented format or API.

[Overview](overview.md)

## Scope

DecodedNav is optional and independent of RawNav. It represents decoded
broadcast navigation parameters supplied by an adapter or derived by a decoder.
A Core observation consumer may instead obtain navigation products externally.
Do not manufacture raw navigation occurrences from decoded ephemerides.

Records identify stream, navigation-record identity, satellite system/number,
navigation family, issue identifiers, health/validity and typed family-specific
parameters. Optional NavigationEpoch association provides acquisition context,
not the ephemeris reference epoch.

Unlike RawNav's epoch-only timing, decoded parameters retain the reference
times required by their scientific definitions, including native GNSS time
fields where necessary. Normalized processing axes use GPST. Do not replace
ephemeris reference times with the containing navigation epoch.
Canonical absolute reference times use Core `GpstTimestamp`. Normalized fit
intervals and ages use `Duration`; time-valued clock biases, group delays and
system-time offsets use `TimeDelta`. All three use `DECIMAL(38,12)` seconds.
Native week/TOW fields and model coefficients retain their standards-defined
units and meanings. Drift and drift-rate coefficients are not durations merely
because they describe clocks. Continuous non-time physical parameters may use
`float64`; issue identifiers, flags and counters retain integer types.

Use separate family schemas, not one universal GPS-like field set.
An optional reference to a RawNav occurrence is valid only where actual
derivation is established; external RINEX/RTCM3 input need not have such a record.

## Remaining definition work

- [ ] Enumerate the initial supported navigation families and source revisions.
- [ ] Define exact parameter names, units, types and validity intervals.
- [ ] Map source time scales and issue/health semantics per family.
- [ ] Define derivation references where a raw decoder supplies the records.

Current scope excludes GLONASS and NavIC. Recognizing an identifier does not imply a
working decoder. Report unsupported families without claiming navigation
coverage. This document establishes the boundary, not a finished field catalog.
