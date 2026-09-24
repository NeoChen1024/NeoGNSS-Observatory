# NeoGNSS Observatory

GNSS research tools for experimenting with raw observations, satellite broadcast
bitstreams, and more, with budget receivers, at home!

This is a pre-Alpha research project. Interfaces, schemas and algorithms may
change without backward compatibility.

## CommonNEX and ParquetNEX

[CommonNEX](docs/commonnex/overview.md) is the project's common data model for
receiver-native GNSS data. It borrows many useful ideas from RINEX, including
satellite and signal identities, observation units and station metadata, but is
not a binary copy of RINEX or a RINEX import/export compatibility layer. Instead,
it captures the common scientific ground of u-blox UBX and Septentrio SBF without
making downstream tools interpret two separate vendor protocols.

| Data | What it preserves and enables |
| --- | --- |
| Observations | Code, carrier phase, Doppler and C/N₀, with quality, tracking and continuity information. |
| RawBits | Received satellite broadcast bits in canonical, receiver-independent layouts for ephemeris reconstruction, SBAS and navigation-message research. |
| Receiver telemetry | Receiver-reported clock estimates, clock-adjustment evidence, pulse timing, temperature and operating status. |
| Events | Observation/navigation epoch completion and receiver restart evidence for interpreting boundaries and discontinuities. |

RawBits is a first-class record family, not an afterthought or an ad hoc sidecar
to observations. Observations and RawBits can also exist independently, so a
receiver need not provide both. Station metadata describes the installation and
acquisition setup.

Clock analysis is not limited to estimating a receiver clock in a PPP solution.
CommonNEX retains the receiver's own navigation-clock bias and drift, together
with source-reported measurement-clock reset or adjustment evidence. This lets
analysis tools study clock behavior directly and correlate it with temperature
and uptime. Availability depends on the receiver; unknown values stay unknown,
and inferred adjustments remain distinct from explicitly reported ones.

[ParquetNEX](docs/commonnex/parquetnex.md) stores these records in compressed,
column-oriented Parquet catalogs partitioned by GPST day. CommonNEX also travels
as Arrow batches between native processing and Python, without requiring a file
round trip. Historical imports, daily incremental processing and live streams
therefore share a data model rather than separate scientific representations.
Project time coordinates use [GPST](docs/time-policy.md); time association and
missing-time rules remain explicit rather than inventing timestamps.

The acquisition inputs are UBX and SBF, including XZ-compressed recordings.
RINEX is a design reference, not a supported acquisition importer; RTCM3 import
is also out of scope. External RINEX navigation and precise GNSS products can
still be calculation dependencies. Original receiver archives remain the
preservation masters. See the [format documentation](docs/commonnex/overview.md)
and [importer guide](docs/commonnex/importer.md) for current coverage and limits.

## Tools

Python commands use the `ngo-` prefix. Each command provides `--help`; follow the
links for configuration, input requirements and scientific limitations.

| Command | Description |
| --- | --- |
| [neognsslogger](libcppgnss/README.md) | Record UBX/SBF streams and inspect decoded GNSS messages. |
| [ngo-mosaic-push](docs/mosaic-push.md) | Retrieve, compress and archive Septentrio receiver recordings. |
| [ngo-rx-msgratio](docs/dataset-qa.md) | Report receiver-message counts and their share of recording size. |
| [ngo-dataset-qa](docs/dataset-qa.md) | Inspect recording quality and reconstruct overlapping UBX archives. |
| [ngo-cnex-import](docs/commonnex/importer.md) | Import UBX/SBF recordings into daily CommonNEX/ParquetNEX datasets. |
| [ngo-cnex-live](docs/commonnex/live.md) | Produce CommonNEX batches from live receiver streams. |
| [ngo-stec](docs/stec.md) | Calculate phase-leveled STEC and GIM-constrained receiver bias estimates. |
| [ngo-stec-plot](docs/stec.md) | Render STEC trajectories with optional SBAS VTEC backgrounds. |
| [ngo-stec-realtime](docs/stec-realtime.md) | Emit realtime relative STEC and IPP coordinates as JSONL. |
| [ngo-stec-realtime-view](docs/stec-realtime-view.md) | Display the latest hour of relative STEC trajectories in a desktop GUI. |
| [ngo-sbas-grid-parquet](docs/subframes.md) | Decode SBAS RawBits into daily grid validity intervals in Parquet. |
| [ngo-sbas-grid-plot](docs/subframes.md) | Render composite VTEC maps from available SBAS providers. |
| [ngo-sbas-map-video](docs/subframes.md#hevcmp4-preview) | Encode map sequences as HEVC/MP4 preview videos. |
| [ngo-receiver-clock](docs/receiver-clock.md) | Analyze CommonNEX receiver telemetry and reconstruct clock trajectories. |
| [ngo-receiver-clock-reunwrap](docs/receiver-clock.md) | Recalculate clock unwrapping from existing derived Parquet data. |
| [ngo-receiver-clock-plot](docs/receiver-clock.md) | Plot receiver clock bias, drift, adjustments and temperature. |
| [ngo-ppp](docs/ppp.md) | Compute offline static GPS PPP Float solutions using local precise products. |
| [ngo-ppp-plot](docs/ppp.md) | Produce PPP diagnostic charts and a vector PDF report. |
| [ngo-cddis-download](docs/cddis-downloader.md) | Download and verify external GNSS products from CDDIS. |
| [ngo-sbf-rinex](docs/rinex-conversion.md) | Export SBF recordings to RINEX using an installed Septentrio RxTools converter. |
| [ngo-rinex-observation-audit](docs/rinex-conversion.md) | Inventory observations in exported RINEX files. |

## Installation

Requires Python 3.11+, uv, CMake 3.24+, a C++20 compiler and standard library
supporting `std::format`, and OpenSSL Crypto development files. From the cloned
repository:

```sh
git submodule update --init --recursive
uv venv .venv
uv pip install --python .venv/bin/python .
source .venv/bin/activate
ngo-cnex-import --help
```

Installation builds the native Python extension and installs the Python tools,
including the PySide6 viewer. The standalone `neognsslogger` is built separately;
see [libcppgnss](libcppgnss/README.md). Some tools need external software or data,
such as RxTools, FFmpeg or CDDIS products; their linked guides describe these
requirements.

## Native libraries and documentation

- [libcppgnss](libcppgnss/README.md): C++20 UBX, SBF, RTCM3 and NMEA parsers,
  mixed-protocol framing and the standalone logger. Protocol support does not
  imply CommonNEX import support.
- [libneognss-obs](libneognss-obs/README.md): Observatory-specific native
  processing and Python bindings, with Arrow batch exchange.
- [Documentation index](docs/README.md): format definitions and tool guides.
- [Processing overview](docs/processing-overview.md): supported workflows and
  scientific scope.
- [Native architecture](docs/native-architecture.md): library ownership and
  interop contracts.

## License

Original code and documentation use GNU GPL version 3 only (`GPL-3.0-only`);
see [LICENSE](LICENSE). Third-party components retain their own licenses.

## Credits

- **SEMU GEO Development Team** for
  [pyubx2](https://github.com/semuconsulting/pyubx2),
  [pysbf2](https://github.com/semuconsulting/pysbf2),
  [pyrtcm](https://github.com/semuconsulting/pyrtcm) and
  [pynmeagps](https://github.com/semuconsulting/pynmeagps), whose protocol
  definitions support this project's native parser generation.
- **[Natural Earth](https://www.naturalearthdata.com/)** for the public-domain
  1:10 million coastline data used in the maps. Made with Natural Earth.
