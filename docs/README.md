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
- [Implementation TODO](TODO.md): processing roadmap, deferred Unified SBAS design and unchecked implementation tasks, not current functionality.

## Design drafts

- [Ginan shim](ginan-shim.md): single-context native backend design,
  GIM/bias calibration investigation and implementation checklist.
- [CommonNEX and ParquetNEX](commonnex/overview.md): current pre-Alpha record
  contracts, receiver mappings, storage and batch/live APIs. Explicitly deferred
  format work is tracked in the [CommonNEX TODO](commonnex/TODO.md).

## Tools

- [CommonNEX live streaming](commonnex/live.md): native engine, five-second batch
  delivery, bounded TCP acquisition and Arrow IPC transport adapters.

- [Mosaic push](mosaic-push.md): resumable receiver FTP mirroring, verified xz/SHA-512
  archives, optional storage limits and multiple FTPS targets.
- [CommonNEX importer](commonnex/importer.md): native UBX/SBF observations,
  RawBits, receiver telemetry and Events to daily Parquet, tail parts and revisions.
- [Multi-GNSS STEC and receiver DCB](stec.md): CommonNEX input, incremental phase leveling,
  GIM-constrained absolute estimates and hourly plots.
- [Realtime relative STEC](stec-realtime.md): GPS/QZSS/Galileo/BeiDou broadcast geometry, CommonNEX Arrow API,
  per-epoch IPP and relative phase STEC JSONL.
- [Realtime STEC viewer](stec-realtime-view.md): PySide6 map and signal-pair traces
  for the latest hour of JSONL output.
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
