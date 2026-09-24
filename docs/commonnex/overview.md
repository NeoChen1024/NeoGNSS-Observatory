# CommonNEX and ParquetNEX

CommonNEX normalizes receiver-native observations, received broadcast bits and
receiver telemetry for NeoGNSS Observatory processing. ParquetNEX is its optional
daily Parquet persistence. This is a pre-Alpha format: schemas can change without
legacy readers or migration layers. Raw archives remain the preservation masters.

## Scope

UBX and SBF are supported acquisition inputs. RINEX and RTCM3 acquisition import
are intentionally out of scope, not pending adapters. RINEX supplies useful
identity, unit and station-metadata conventions, not a lossless I/O promise.
RTCM3 requires additional time context and does not preserve receiver-native
quality or received RawBits occurrences. Native RTCM3/NMEA protocol parsing and
mixed framing remain supported independently by libcppgnss.

GPS, Galileo, BeiDou, QZSS and SBAS are in scope; GLONASS and NavIC scientific
processing are excluded. Supported systems do not imply every signal, receiver
revision or scientific algorithm is implemented.

Observation and RawBits are independent first-class record families: either
may exist without the other. Telemetry and Events accompany available evidence.
No mandatory epoch table, row-reference graph or global occurrence ID exists.
Timestamps are coordinates, not unique identities. Decoded navigation is not a
CommonNEX catalog; processors can decode RawBits or consume external products,
including RINEX NAV, SP3 and bias products, as separate calculation dependencies.

## Document map

| Document | Primary responsibility |
| --- | --- |
| [Shared types](types.md) | Identity, decimal GPST/duration types, numerical and null semantics |
| [Observation](observations.md) | C/L/D/S, quality, tracking and receiver corrections |
| [RawBits](raw-bits.md) | Record schema, checks and occurrence semantics |
| [RawBits formats](raw-bits-formats.md) | Signal vocabulary, legal family/format pairs and canonical bit layouts |
| [Receiver telemetry](receiver-telemetry.md) | Common status, clock and pulse records |
| [Events](events.md) | Implemented completion/restart and explicitly planned cadence contracts |
| [Receiver time](receiver-time.md) | Missing time, anchor freshness, uptime and restart association |
| [Setup JSON](setup-json.md) | Station metadata and calibration/configuration companions |
| [Receiver profiles](receiver-profiles.md) | Which receiver messages to enable |
| [Receiver mappings](receiver-mappings.md) | UBX/SBF normalization, packing and validation coverage |
| [Import policy](import-policy.md) | Input ordering scope, reconciliation limits and consumer continuity |
| [ParquetNEX](parquetnex.md) | Disk encoding, metadata, date/part/revision layout and reader rules |
| [Batch importer](importer.md) | CLI initialization, input discovery, continuation and reconstruction |
| [Live API](live.md) | Streaming and Arrow IPC delivery interfaces |
| [Remaining work](TODO.md) | Concrete unimplemented or unverified items only |

## Naming and strings

Reuse standardized GNSS identifiers and RINEX conventions where appropriate,
without fixed-column widths or ASCII-only restrictions. Strings support Unicode;
do not silently truncate, transliterate, case-fold or normalize their contents.
Field-specific identity and companion-path constraints still apply.

## Processing pipeline

```text
UBX / SBF -> CommonNEX batches ----------------> processing
                    |                               ^
               ParquetNEX writer                    |
                    |                               |
               daily catalogs -> CommonNEX reader ---+
```

Direct and replay paths preserve the same logical quantities, missing values,
quality and time semantics. Observation uses its own measurement GPST; RawBits
and telemetry may retain null time. GPST uses DECIMAL(38,12) seconds, not Unix
time or floating absolute seconds. See [shared types](types.md).

Archive, daily incremental and live inputs share the model; a complete day,
known stream end, prior QA stamp or Parquet round trip is not required. File,
batch and day boundaries do not reset scientific state or make a daily solution.
Consumers declare required catalogs and context; a detached day need not be
sufficient to initialize a solver. Scheduling belongs to the implementation.

The native/Python Arrow boundary and library ownership are documented in
[native architecture](../native-architecture.md), not additional format rules.
The importer and mapping documents describe current support; TODO entries do
not imply working adapters. No existing datasets are rewritten by a spec update.

Project-owned specifications are GPL-3.0-only; see [LICENSE](../../LICENSE).
External standards retain their notices.
