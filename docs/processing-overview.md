# Processing overview

NeoGNSS Observatory is a pre-Alpha offline GNSS research project. Current
tools extract receiver telemetry and SBAS messages, calculate SBAS grids and
relative carrier-phase dSTEC, and export scientific tables and plots.
GPS Float PPP is available as an initial static forward pipeline. Calibrated
absolute TEC and automated TID detection are not implemented project pipelines.

## Input preparation

Read expanded recordings directly. Raw archives are read-only; processing
does not decompress XZ or silently select compressed copies. Deployment paths
and local environment instructions belong in [AGENTS.md](../AGENTS.md).

`ngo-dataset-qa` offers an optional read-only scan. Its `restitch` profile is
for overlapping UBX inputs, notably Era A. Nonoverlapping Era B UBX and Era C
SBF can go directly to extraction. Extraction neither reruns full QA nor
requires a QA stamp or reconstruction manifest. Necessary framing, usable-time
and scientific-validity checks still apply.

Arrange input files in stream order. File names choose traversal, not GPST;
time comes from payloads. Gaps are not filled and time reversal is not silently
repaired. Physical file/day boundaries do not by themselves reset native
extraction state. See [dataset QA](dataset-qa.md) and [dataset notes](dataset-notes.md).

## Current processing paths

| Input | Processing | Products |
| --- | --- | --- |
| UBX or SBF | `ngo-sbas-frame-parquet` → `ngo-sbas-grid-parquet` → `ngo-sbas-grid-plot` | Daily SBAS body and IGP-interval Parquet; hourly VTEC maps |
| UBX NAV-CLOCK/MON-SYS | `ngo-receiver-clock` → `ngo-receiver-clock-plot` | Clock Parquet, events, telemetry and plots |
| Existing clock Parquet | `ngo-receiver-clock-reunwrap` | Recomputed clock arcs and bias corrections |
| UBX | RTKLIB-EX `neognss_convbin` | RINEX OBS/NAV supported by the pinned converter |
| SBF | `ngo-sbf-rinex` with installed RxTools | Native-rate RINEX and applicable auxiliary outputs |
| RINEX OBS/NAV and SBAS hourly cells | `ngo-tec-ipp-map` | Broadcast-orbit IPP tracks, arc-relative dSTEC and maps |
| CDDIS listings/products | `ngo-cddis-download` | Explicit product plans and integrity-checked downloads |
| UBX/SBF GPS L1/L2 and local precise products | `ngo-ppp` → `ngo-ppp-plot` | Static forward Float solutions, residual Parquet and daily plots |

RINEX conversion is not lossless preservation of raw protocols and does not
automatically prove cross-file continuity. Follow the [conversion guide](rinex-conversion.md).
The SBAS path retains its own 250-bit message bodies; it does not depend on
RINEX representing those messages.

## Intermediate products

All observation axes and daily/hourly partitions use [GPST](time-policy.md).
Intermediate formats retain the data and context needed by the consumer, not
copies of transport envelopes or execution environments. In particular, grid
calculation reads only SBAS frame Parquet, and plotting reads only grid products.

Python owns file I/O, Parquet, orchestration and rendering. CPU-heavy parsing
and state machines run in native batches. Parallelize independent receiver
streams or rendering jobs, not arbitrary cuts through shared parser state.
See [native architecture](native-architecture.md).

Outputs are research products outside Git. CLI schemas may change; no stable
compatibility layer is promised. Directory-producing research tools publish
by rename and support explicit replacement while retaining a backup. Downloader
recovery and reconstruction overlap proofs serve distinct integrity needs.

## Scientific limits and extension points

- Carrier-phase dSTEC is relative within a continuous arc, not absolute STEC.
  Absolute TEC requires ambiguity/bias treatment compatible with the actual
  observation codes; downloaded bias products are not automatically applied.
- Current IPP geometry uses broadcast NAV. Precise SP3/CLK consumption is an
  extension point, not an option already wired into `ngo-tec-ipp-map`.
- SBAS broadcast equivalent VTEC is an operational correction field, not a
  direct high-rate ionospheric measurement at the receiver.
- ROT/ROTI, detrending, automated TID detection, multi-GNSS PPP and absolute TEC need
  dedicated implementations and validation. A single station alone does not
  establish a disturbance's horizontal propagation velocity.
- CDDIS availability and successful decompression do not prove scientific
  coverage, compatible product families or solver support.
- [PPP Float](ppp.md) currently supports GPS L1/L2 only, with explicit receiver
  antenna calibration and a limited model set. It does not enable PPP-AR.
