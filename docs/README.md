# Documentation

These documents describe the current implementation. Historical runs, superseded
designs and migration logs belong in Git history. Dataset facts are retained
only when they affect present interpretation or processing.

## Start here

- [Processing overview](processing-overview.md): supported paths and research boundaries.
- [GPST policy](time-policy.md): units, timestamps and partitions.
- [Dataset notes](dataset-notes.md): overlap and acquisition caveats that affect analysis.
- [Implementation TODO](TODO.md): agreed raw-observation STEC and offline PPP designs and unchecked implementation tasks, not current functionality.

## Tools

- [Offline PPP Float](ppp.md): raw GPS observations, local precise products and numerical/plot outputs.
- [Dataset QA and reconstruction](dataset-qa.md): optional scan and explicit restitch.
- [SBAS frames, grids and maps](subframes.md): source-independent Parquet pipeline.
- [Receiver clocks](receiver-clock.md): telemetry, unwrap and plots.
- [RINEX conversion](rinex-conversion.md): RTKLIB-EX and RxTools usage and limits.
- [IPP and relative dSTEC maps](tec-ipp-maps.md): broadcast geometry and hourly visualization.
- [CDDIS downloader](cddis-downloader.md): credentials, planning and transfer recovery.

## Libraries

- [Native architecture](native-architecture.md): protocol, processing and Python boundaries.
- [libcppgnss](../libcppgnss/README.md): protocol API and logger.
- [libneognss-obs](../libneognss-obs/README.md): native processing and bindings.
