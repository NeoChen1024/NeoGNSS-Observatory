# Era C RxTools SBF-to-RINEX experiment

The pilot artifacts recorded here were cleared during the single-GPST migration.
Results below are historical validation evidence, not reusable output paths.
New processing follows [the GPST policy](time-policy.md).

## Conversion policy

Use the separately installed Septentrio RxTools `sbf2rin` executable. It is not
vendored or relicensed by this project. The wrapper records its version and
SHA-256, the source hash, exact arguments, output hashes and converter log.
Retain the corresponding RxTools installation for reproducible processing.

```sh
sbf-rinex --tool /path/to/RxTools/bin/sbf2rin \
  --source /path/to/expanded/bx4a0910.25_ \
  --output work/rxtools-example --rinex-version 4.01 --workers 4
```

The default is RINEX 4.01 to preserve additional navigation message types.
Use `--rinex-version 3.04` for a separate compatibility conversion. This does
not change the existing project's canonical RINEX policy or make downstream
RTKLIB consumers automatically compatible with RINEX 4 navigation files.

The underlying command is:

```sh
sbf2rin -f /absolute/input.25_ -o copy -R401 -nOPBM -s -D -X -c -v
```

- Keep the native observation interval: no `-i`, start/end clipping, system
  exclusion or signal selection.
- Request observations, mixed navigation, SBAS broadcast and meteorology.
  A requested output is only produced when applicable data is available.
- Include `S` (signal strength), `D` (Doppler), and `X1` (channel number),
  alongside code and phase. Preserve converter-provided LLI/SSI and metadata.
- Enable SBF comments; do not suppress external-event comments. Do not force
  kinematic events or use `-U` to silently remove duplicate satellite records.
- Use the main antenna (RxTools default antenna 1); this is not a multi-antenna
  export policy.
- Each input receives a separate new output directory, avoiding filename
  collisions. Existing output directories are rejected. Repeated `--source`
  arguments run independent native converter processes, bounded by `--workers`.
- Inputs must be expanded `.25_` or `.sbf` files. No XZ decompression or writes
  to source directories occur.

## Verified one-day pilot

Tested `sbf2rin-15.10.3` with the expanded `bx4a0910.25_` from 2025-04-01.
The date below comes from payload/output timestamps, not solely the filename.

| Check | Result |
|---|---|
| Native SBF inventory | 86,400 MeasEpoch blocks; 86,400 Meas3Ranges blocks; zero CRC errors reported by `sbfblocks` |
| RINEX 4.01 and 3.04 observation epochs | 86,400 each, all 86,399 adjacent intervals exactly 1 second |
| First / last observation | 2025-04-01 00:00:00 / 23:59:59 GPST |
| Observation body comparison | Identical SHA-256 between the two new versions |
| Satellites with records | 137 across GPS, Galileo, GLONASS, BeiDou, QZSS and SBAS |
| BeiDou C59 | Present in all 86,400 epochs |
| Existing receiver RINEX reference | 2,880 epochs, 30-second spacing, the same 137 satellite identifiers |
| New observation header counts | G: 19, E: 21, S: 5, R: 17, C: 25, J: 13 observable types, including X1 |
| Additional RINEX 4 NAV content | 161 GPS CNAV records, 23 ION records, 18 STO records |
| Other products | Mixed NAV and about 21 MiB SBAS broadcast file; no meteorology file |

The older reference declares NavIC and SBAS L5 observation types but has no
NavIC satellite records on this day. Header declarations alone do not prove
actual observations. This pilot checks epoch coverage and new-version body
identity; a per-observable comparison against the receiver's 30-second file
and a raw-SBF-to-RINEX value-by-value proof remain future validation steps.

The RINEX 4 NAV ephemeris inventory is GPS LNAV 192 / CNAV 161, Galileo
INAV 986 / FNAV 955, BeiDou D1 444 / D2 198, QZSS LNAV 97, GLONASS FDMA 403,
and SBAS 1,412. RINEX 3.04 contains the corresponding legacy ephemerides but
not those 161 GPS CNAV records. RINEX 3 headers can carry some ionosphere/time
parameters; the RINEX 4 ION/STO record inventory should not be interpreted as
all parameters being entirely absent in version 3.

Local experiment products are under `work/era-c-rxtools-20250401-r401/` and
`work/era-c-rxtools-20250401-r304/`. Each `source-00000/conversion.json` records
conversion provenance; `observation-audit.json` in the former directory
compares the two new observation inventories with the original reference.

To reproduce an observation audit:

```sh
rinex-observation-audit \
  --obs work/rxtools-example/source-00000/bx4a0910.25O \
  --obs /path/to/reference/bx4a0910.25o \
  --output work/rinex-audit.json --workers 2
```

## Limits and next validation gates

RINEX is not a lossless container for SBF. This pilot's source includes
MeasExtra variances, Meas3PP proprietary flags, Meas3MP multipath corrections,
receiver quality/status, RFStatus, PVT and raw navigation pages. Preserve the
SBF and its block inventory; extract additional sidecars when an analysis
needs these fields. The SBAS broadcast export is not a replacement for every
raw GNSS navigation page. Do not assume every message family is converted
just because RINEX 4 can represent it.

The observed day boundaries match the project's GPST-day policy. Keep the
GPS time declared in the observation header; do not offset or relabel these
epochs as UTC. Other inputs still require payload coverage validation.

RxTools converts multiple inputs independently. This experiment does not yet
prove gapless behavior across file boundaries, preserve converter state across
inputs, or authorize filling missing epochs. Before a full canonical export,
test adjacent-file phase/LLI continuity and navigation carry-over, compare
common 30-second observations, and validate each downstream consumer's
supported observation codes and satellite ranges. In particular, the pinned
RTKLIB build's BeiDou PRN ceiling remains a downstream limitation even though
RxTools preserves C59 and higher PRNs in RINEX.
