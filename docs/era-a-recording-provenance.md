# Era A recording provenance and continuity observations

## Historical acquisition path

The archive owner supplied the following original `ubx24h` (Era A) recording
command on 2026-09-06:

```sh
gpspipe -x 86460 -R | xz -e > /share/ubx24h/"$(date +%Y%m%dT%H%M%S%z)".ubx.xz
```

This is historical provenance, not a recommended command for new recordings.
The original wall-clock filename and numeric timezone suffix are retained here
verbatim; they do not change the current single-GPST policy for derived products.
Observation coverage must still come from UBX payload timestamps, not this
filename or the requested capture duration.

The recorded acquisition path was:

```text
Receiver -> RPi 4B PL011 UART0 -> gpsd -> gpspipe -> pipe -> xz -> SD card / USB 3.0 SSD
```

The old C++ UBX logger was **not** in this path. Its recording or rotation logic
therefore cannot explain missing epochs in these original Era A recordings.
The command alone does not establish the historical gpsd version, receiver
configuration, UART baud rate and buffering settings, process supervision, or scheduling
of successive captures.

## Hardware context supplied on 2026-09-07

The archive owner supplied these additional details about the Era A setup:

- The primary recording host was a Raspberry Pi 4B.
- Recordings initially went to an SD card and later to a USB 3.0 SSD. The owner
  confirmed that the SSD offered substantially higher write throughput and
  considered sustained storage throughput unlikely to explain the gaps, even
  for the earlier, relatively low-rate SD-card recording workload.
- Receiver input used the Pi's **PL011 UART0**, with `dtoverlay=disable-bt`
  enabled in `config.txt`. It was not a mini UART acquisition path.

These are operator-reported historical configuration details, not a new hardware
audit. No exact storage-change date, before/after gap statistics, or historical
UART error counters were supplied. In particular, this note does not claim a
measured comparison of gap rates between SD-card and SSD recording periods.

## Direct TCP comparison reported on 2026-09-06

The archive owner reported approximately four hours of acquisition using the
maintained C++ logger directly from TCP `DF32:2006`, serving a ZED-F9P in TIME
mode. Apart from one incident suspected to be timeout-related, the previously
observed pattern of frequent gaps did not recur during that observation window.

This is an operator-reported result. The exact start/end epochs, diagnostic
counts, incident cause, and log hashes were not independently audited for this
note. It is not a claim of a completed overnight test or zero data loss.

## Interpretation and processing implications

The comparison makes the historical gpsd/gpspipe acquisition path a stronger
suspect, and excludes the old C++ logger from that particular Era A path.
The supplied UART configuration excludes mini UART instability as an explanation
for this acquisition path. The storage history also makes a simple sustained
write-throughput shortage less persuasive; storage should not be treated as the
default explanation merely because the initial medium was an SD card.

It does **not** isolate gpsd itself as the proven cause. PL011 use does not by
itself establish loss-free UART acquisition, and high storage throughput does
not establish that the entire recording chain never stalled. Receiver behavior,
UART servicing/buffering, gpspipe, pipe/compression/output backpressure, and
capture lifecycle remain possible contributing factors rather than established
explanations. A later direct TCP run is not a simultaneous controlled comparison
against the original recording chain.

Working hypothesis: the frequent gaps may have originated in the historical
gpsd-based acquisition chain; the responsible component and mechanism remain
unknown. No specific gpsd defect or version is attributed by this observation.

Keep actual payload gaps and their reconstruction provenance intact. Do not
fill missing epochs, infer missing observations from the capture command, or
change reconstruction/analysis continuity thresholds merely to hide gaps.
The one suspected timeout in the direct TCP test must not be treated as a
confirmed explanation for the historical pattern.

See [UBX reconstruction](ubx-restitch.md) for gap handling and
[logger continuity diagnostics](../libcppgnss/README.md#overnight-continuity-diagnostics)
for the maintained logger's checks.
