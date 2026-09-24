# Documentation

Tool documents describe usage, observable behavior, data interpretation and
known limitations. Keep internal scheduling, cache structures and optimization
walkthroughs in code rather than duplicating them here. Public API contracts and
scientific/format definitions still belong in their owning documentation.
Design documents explicitly distinguish selected contracts from implemented
subsets. Historical runs, superseded
designs and migration logs belong in Git history. Dataset facts are retained
only when they affect present interpretation or processing.

## Start here

- [Processing overview](processing-overview.md): supported paths and research boundaries.
- [GPST policy](time-policy.md): units, timestamps and partitions.
- [Dataset notes](dataset-notes.md): overlap and acquisition caveats that affect analysis.
- [Implementation TODO](TODO.md): agreed raw-observation STEC and offline PPP designs and unchecked implementation tasks, not current functionality.

## Design drafts

- [Ginan shim](ginan-shim.md): single-context native backend design,
  GIM/bias calibration investigation and implementation checklist.
- [CommonNEX and ParquetNEX v0](commonnex/overview.md): Core observations and RawBits,
  optional DecodedNav/auxiliary schemas, receiver profiles, Parquet persistence
  and batch/incremental/live processing contracts; broader than the implemented pilot.

## Tools

- [CommonNEX live streaming](commonnex/live.md): native engine, five-second batch
  delivery, bounded TCP acquisition and Arrow IPC transport adapters.

- [Mosaic push](mosaic-push.md): resumable receiver FTP mirroring, verified xz/SHA-512
  archives, optional storage limits and multiple FTPS targets.
- [CommonNEX importer pilot](commonnex/importer.md): native UBX/SBF/RTCM3 observations,
  RawBits, receiver telemetry and Events to daily Parquet, tail parts and revisions.
- [RTCM3 observation adapter](commonnex/rtcm3.md): MSM4-7, explicit GPST week resolution and station selection.
- [Multi-GNSS STEC and receiver DCB](stec.md): CommonNEX input, incremental phase leveling,
  GIM-constrained absolute estimates and hourly plots.
- [Receiver antenna calibration](antenna.md): shared PCO/PCV, same-frequency use, bounded interpolation and nearby-frequency substitution.
- [Offline PPP Float](ppp.md): raw GPS observations, local precise products and numerical/plot outputs.
- [Dataset QA and reconstruction](dataset-qa.md): optional scan and explicit restitch.
- [SBAS frames, grids and maps](subframes.md): source-independent Parquet pipeline.
- [Receiver clocks](receiver-clock.md): telemetry, unwrap and plots.
- [RINEX conversion](rinex-conversion.md): RTKLIB-EX and RxTools usage and limits.
- [CDDIS downloader](cddis-downloader.md): credentials, planning and transfer recovery.

## Libraries

- [Native architecture](native-architecture.md): protocol, processing and Python boundaries.
- [libcppgnss](../libcppgnss/README.md): protocol API and logger.
- [libneognss-obs](../libneognss-obs/README.md): native processing and bindings.
