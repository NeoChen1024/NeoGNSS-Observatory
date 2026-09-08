# RINEX conversion

RINEX OBS/NAV is an optional export and comparison format; the [STEC](stec.md)
and [PPP](ppp.md) pipelines read raw observations directly.
It is not a lossless copy of UBX or SBF: preserve the raw archive,
and use the separate [SBAS frame pipeline](subframes.md) for SBAS bodies.
All project observation axes use [GPST](time-policy.md).

## UBX with RTKLIB-EX

Build the project wrapper against the recorded `contrib/RTKLIB` revision:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DNEOGNSS_BUILD_RINEX_TOOLS=ON
cmake --build build --target neognss_convbin --parallel 4

build/native/neognss_convbin -r ubx -v 3.04 -od -os -oi -ot -ol \
  -o group.obs -n group.nav 'ordered-group/*.ubx'
```

This optional target uses `NFREQ=4`, `NEXOBS=3` and the constellation support
enabled by `native/CMakeLists.txt`. Upstream code is not modified.

Supply ordered, complete-frame, nonoverlapping files from one continuous
recording in one converter invocation. The quoted glob is expanded by convbin.
Do not assume packet fragments spanning physical input files are carried over
by this converter. Independent invocations can initialize phase/LLI and
navigation assembly differently; do not reset conversion merely at midnight.
The project does not currently provide an automatic gapless, daily RINEX
normalization pipeline. Restitch is optional repair for overlaps, not a
prerequisite for nonoverlapping UBX conversion.

Preserve actual fractional RAWX timestamps. Logger/reconstruction epoch labels
are not a substitute for observation time. When assigning output days, use
RINEX payload epochs, not the input file's date.

### Supported satellite and signal subset

The pinned RTKLIB source defines `MAXPRNCMP=50` in
[`rtklib.h`](../contrib/RTKLIB/src/rtklib.h). BeiDou C59 and other PRNs beyond
that ceiling are unsupported by this build. An OBS epoch can still be written
after unsupported measurements are skipped: equal epoch counts do not prove
complete observation retention. Treat these as converter exclusions, not
receiver outages. External orbit products cannot restore omitted observations.

RINEX observation codes, satellite ranges and navigation records must be
supported by the downstream decoder too. Supplying a newer RINEX version does
not automatically expand RTKLIB's capabilities. Converter-estimated approximate
station coordinates are not surveyed station metadata.

## SBF with RxTools

Use a separately installed Septentrio `sbf2rin`; the installation is not vendored.

```sh
ngo-sbf-rinex --tool /path/to/RxTools/bin/sbf2rin \
  --source /data/recording.25_ --output /data/rinex-export \
  --rinex-version 4.01 --workers 4
```

The wrapper accepts expanded `.25_` and `.sbf` files. Repeat `--source` for
independent conversions; `--workers` bounds concurrent native processes.
Available versions are 3.04, 3.05, 4.00 and 4.01, with 4.01 as the default.
Choose a version supported by the intended downstream consumer.

The wrapper invokes `sbf2rin -f INPUT -o copy -R401 -nOPBM -s -D -X -c -v`
for the default version. It requests OBS, mixed NAV, SBAS broadcast and
meteorology where applicable, plus signal strength, Doppler and channel number.
It applies no resampling, time clipping or signal filters and uses the main
antenna. Comments and external events are retained by the requested options.

Each input has a `source-NNNNN/` directory containing converter outputs,
`converter.log`, and `conversion.json` with tool version, arguments, input size,
output listing and observation summary. No source/binary/output hash inventory
is generated. Directory publication and `--overwrite` follow the research
output policy. The wrapper requires a generated OBS file declaring GPS time.

Inputs are converted independently. Cross-file phase/LLI and navigation
carry-over are not guaranteed, and the wrapper does not repartition them into
canonical GPST days. Additional receiver status, RF diagnostics, proprietary
measurement fields and raw navigation pages are not necessarily represented
by these exports. Requested auxiliary outputs need not exist if no applicable
data was recorded.

## Observation audit

```sh
ngo-rinex-observation-audit --obs /data/export.25O --obs /data/reference.25o \
  --output /data/observation-audit.json --workers 2
```

This reads RINEX 3/4 headers and inventories epoch flags, intervals, satellite
records and declared observables. It requires explicit GPS observation time.
It is not a per-observable equality check, navigation validity assessment,
gap repair, or proof that all raw measurements survived conversion.

Before interpreting a time series, check signal/PRN exclusions, fractional
epochs, adjacent-file phase/LLI behavior, and navigation availability. Compare
only common epochs when using a lower-rate reference.
