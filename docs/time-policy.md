# Single GPST processing policy

All project-owned observation timelines, partitions, query windows, plots and
derived metadata use GPST. There is no selectable UTC processing mode.

## Representation

`gpst`, `start_gpst`, `end_gpst` and `hour_gpst` denote continuous seconds since
1980-01-06 00:00:00 GPST. These are **not Unix timestamps**. Archive indexes use
integer `gpst_ms` milliseconds. `start_gpst_ms` / `end_gpst_ms` are exact
artifact bounds, while unsuffixed fields remain seconds. Receiver-clock
products use integer `gpst_ns`; NAV-CLOCK retains its millisecond iTOW resolution.
Raw measurement timestamps retain their original fractional precision.

GPST calendar rendering uses calendar arithmetic from that epoch, with no
timezone conversion. Assigned UBX files use
`GPST-%Y-%m-%d--%H-%M-%S-mmm.ubx`, with exactly three millisecond digits;
PNG labels/names also explicitly include GPST.
No GPST label ends in `Z`, `%z`, or `+0000`.

Days and hours are half-open GPST intervals: 86,400 and 3,600 seconds.
Continuous observation arcs survive file/day/hour boundaries; IPP display
values alone are rebased to each arc's first valid sample in the GPST hour.

## Input boundaries

- UBX archive timing is anchored by valid NAV-TIMEGPS or RAWX GPS week/TOW,
  never by NAV-PVT's UTC calendar. NAV/EOE defines millisecond navigation
  epochs without snapping to seconds. RAWX's steered measurement time is
  associated by the epoch's stream framing, not exact timestamp equality;
  week carry is handled explicitly and original bytes are retained. Empty
  RAWX reports cannot anchor or split epochs.
  Reconstruction splits at GPST midnight or a NAV-to-NAV interval exceeding
  50 seconds by default (`--gap-timeout`). This is not a sampling-rate or
  gapless-observation claim; downstream scientific QC remains independent.
- The logger requires valid week/TOW in NAV-TIMEGPS in every EOE interval and
  matching EOE iTOW. It buffers frames and routes the complete epoch at EOE.
  The first new-day epoch belongs entirely to the new day. Missing EOE at EOF,
  missing/invalid TIMEGPS, conflicting timestamps or non-increasing epochs
  fail explicitly. Subsecond fTOW is preserved, not used to shift nominal
  logger epochs to a neighboring second.
- RINEX observations must declare GPS time for project processing. Navigation
  records keep their standards-defined, constellation-specific native fields;
  the decoder converts them internally. Do not rewrite NAV fields as if they
  all had the same native time scale.
- SBF is immutable input. Receiver-time offsets, validity and GNSS timing
  system settings still matter; GPST does not make receiver clocks perfect.
- External product timestamps, download protocols and source metadata retain
  their specified semantics. Standard-format UTC fields are not relabeled.

## Migration and reproducibility

The archive index magic is `UBXIDX04` (integer milliseconds); reconstruction
rejects old second-based indexes and uses a format-specific inventory cache.
Reconstruction plans use schema 3 and `gpst_segment` artifacts. Existing
GPST schema-2 products remain readable downstream with explicit unit conversion
where needed; new plans require a new output directory, not relabeling.
Analysis uses
`hour_gpst` and GPST-labeled filenames. Old UTC products must be recomputed,
not reused or relabeled.

The user authorized deletion of prior Era A/B reconstruction outputs and
analysis products, caches and run logs under `work/`. Original archives,
downloaded external products, downloader plans/configuration and map assets
were preserved. Source SBF/UBX and native protocol fields remain unchanged.

This migration does not claim that full-archive reconstruction or RINEX
cross-file phase continuity has already been rerun and validated. Those are
new processing runs, with fresh GPST provenance and independent QC gates.
