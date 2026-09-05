# NeoGNSS Observatory

This project builds a reproducible offline ionospheric-observation and static
PPP pipeline from the existing GNSS raw archive. The repository stores code,
configuration, manifests, and compact result summaries. Raw archives under
`/hdd` remain read-only preservation masters, and large derived data stays out
of Git.

See [docs/offline-processing-plan.md](docs/offline-processing-plan.md) for the
verified data inventory and proposed implementation sequence.

The first deliverable is deliberately smaller than a complete two-year
reprocessing run:

1. Generate one UTC day of canonical RINEX from Era C's 1 Hz SBF data.
2. Cross-check it against the receiver-generated 30 s RINEX.
3. Produce availability, gap, cycle-slip, and signal-pair QC.
4. Produce carrier-phase relative dTEC, ROT/ROTI, and IPP time series.
5. Expand to all 147 Era C days only after the golden-day gate passes, then
   ingest Eras A and B.

All references to RTKLIB mean the RTKLIB-EX `main` branch from
[`rtklibexplorer/RTKLIB`](https://github.com/rtklibexplorer/RTKLIB), not the
upstream `tomojitakasu/RTKLIB` repository.
