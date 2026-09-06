# Era A RTKLIB-EX conversion experiment

Historical validation notes: the pilot artifacts below were removed during
the single-GPST migration. They are not reusable current products. New runs
follow [the GPST time policy](time-policy.md); native UTC fields mentioned in
the historical measurements below retain their original meaning.

## Scope and tool revision

This experiment checks whether source-file boundaries introduce observation or
navigation differences. It does not certify complete signal mapping, navigation
validity, or production readiness.

RTKLIB-EX revision: `06e8644287ff07efc4c53bbf3e7f9dafb0355605`.
The source submodule was not modified. Build settings were C99, `-O3`,
`ENAGLO`, `ENAQZS`, `ENAGAL`, `ENACMP`, `ENAIRN`, `NFREQ=4`, and `NEXOBS=3`.
A separate build with `TRACE` enabled was used to investigate decoder errors.

Conversion options:

```sh
convbin -r ubx -v 3.04 -od -os -oi -ot -ol \
  -o output.obs -n output.nav 'ordered-input/*.ubx'
```

The input glob is expanded by convbin, in one invocation. No observation
decimation or half-cycle correction option was requested.

## Source sample

Two reconstructed segments belong to the same continuous extraction group:

| Segment | Bytes | SHA-256 |
| --- | ---: | --- |
| `2023-05-19T23:57:14+0000.ubx` | 1348922 | `c73b65c4ae88406520ae4cce7d0872268a4a2dd381534e90dcbd343f9a17ea1a` |
| `2023-05-20T00:00:00+0000.ubx` | 1313804 | `cd3321dabaad2008a1c065238dafd33272f3fe085c02297385c6882227affbaa` |

Both source hashes and every UBX frame checksum were verified. Payload-derived
reconstruction coverage is 23:57:14 through 00:02:41 UTC, inclusive. There are
328 RAWX messages, with GPST observations from 23:57:31.999000 through
00:02:58.999000. The actual fractional timestamps are preserved in RINEX.

## Results

Comparisons below use exact OBS and NAV body bytes, excluding headers containing
input paths and generation times. OBS comparisons therefore include the written
measurements, epoch timestamps, LLI, and signal-strength indicators.

| Input arrangement | OBS epochs | NAV records | Equal to joined baseline |
| --- | ---: | ---: | --- |
| One concatenated input | 328 | 92 | Baseline |
| Original two complete-frame files, one invocation | 328 | 92 | OBS and NAV |
| 52 files split every 401 complete UBX frames, one invocation | 328 | 92 | OBS and NAV |
| Split inside a selected RAWX packet, one invocation | 327 | 92 | NAV only |

An additional arbitrary packet-interior cut happened not to change OBS/NAV;
that result does not establish safety for arbitrary byte cuts. The targeted
RAWX cut demonstrates actual observation loss.

Converting the original two files in independent invocations preserved the
epoch set but changed one OBS epoch: at GPST `2023-05-20 00:00:17.9990000`,
G09 L1C acquired LLI 1 after decoder initialization. Independent NAV outputs
contained 49 and 53 records. Their union had 102 unique body records versus
92 in the continuous run, with 20 baseline records absent and 30 additional
records. These are exact record differences, not a claim that 20 distinct
ephemerides are unavailable; semantic NAV comparison remains necessary.

## Decoder limitation discovered

The baseline reported 546 decoder errors. TRACE identified C59 as the rejected
satellite: this revision defines `MAXPRNCMP` as 50 in `src/rtklib.h`.
The two-pass trace contained 1,092 SFRBX satellite-number errors and 656 RAWX
satellite-number errors, all for BeiDou PRN 59. RAWX can still produce an OBS
epoch after skipping unsupported satellite measurements, so an equal epoch
count is not evidence that every satellite observation was retained.

Current policy: retain the pinned RTKLIB-EX revision without modifying upstream
sources. Accept its supported-satellite subset for exploratory processing and
record unsupported PRNs as converter exclusions, not receiver outages. C59 is
confirmed in this sample; further inputs require their own exclusion inventory.
External orbit or navigation products cannot restore observations omitted from
RINEX. Do not label these outputs as retaining all source observations.

The trace also records missing broadcast ephemerides and point-position
initialization failures. A short sample cannot establish navigation coverage
at its start. Receiver/antenna metadata must be supplied for production;
convbin's estimated approximate position is not surveyed station metadata.

## Consequences

- Feed ordered, complete UBX frames through one decoder session across
  continuous file boundaries. Do not reset the converter at GPST midnight.
- Reconstructed Era A files satisfy the complete-frame input requirement.
- Preserve actual RAWX timestamps when assigning GPST output days. Nominal
  integer-second reconstruction labels are not observation timestamps.
- Defer changes to the C59 PRN limit. Carry the converter exclusion into
  downstream QC and TEC provenance while using the supported subset.
- Next validation must compare per-satellite/per-signal RAWX values and validity
  flags with RINEX and compare NAV semantically, including transmission time.

Local experiment artifacts are kept outside Git under
`work/era-a-rinex-test/`: the Click experiment script, converter binaries,
`run-3/report.json`, input variants, OBS/NAV outputs, and decoder trace.
The report records source, converter, and experiment SHA-256 values and the
exact conversion commands.

## Downstream interface and satellite geometry

Use RINEX OBS as the primary input for receiver-derived TEC, with reconstruction
and conversion provenance supplying restart, gap, and exclusion information.
The SBAS MT18/26 broadcast-grid analysis remains a separate raw-message branch.
Begin with dual-frequency carrier geometry-free combinations and arc-relative
dSTEC; absolute STEC/VTEC additionally requires ambiguity/bias treatment.

Use the pinned RTKLIB-EX library for satellite geometry, wrapped by project-owned
tools without editing the submodule. Broadcast RINEX NAV is the initial geometry
source. Precise SP3 orbits and compatible CLK products are a later selectable
source; record the chosen source and any explicitly allowed fallback per result.
The relevant interfaces are `satposs`/`satpos`, `readsp3`, and `readrnxc`.

Satellite positions must correspond to signal transmission time. Apply the
appropriate propagation/Earth-rotation treatment when relating them to station
coordinates and observation reception time. Station coordinates, frame, orbit
validity, satellite health, and time systems must be explicit. Use geometry to
derive azimuth/elevation and the IPP on the configured thin shell (initially
350 km), then apply the chosen STEC-to-VTEC mapping. Precise clock products are
not a prerequisite for the basic dual-frequency geometry-free dSTEC combination.

References:

- [ESA measurement combinations](https://gssc.esa.int/navipedia/index.php?title=Combination_of_GNSS_Measurements)
- [IGS orbit and clock products](https://igs.org/products/)
