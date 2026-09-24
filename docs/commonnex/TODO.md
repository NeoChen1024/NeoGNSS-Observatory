# CommonNEX remaining work

[Overview](overview.md). This list contains unimplemented or unverified work,
not completed decision history. Current contracts belong to their owning pages.
No RINEX/RTCM3 importer or DecodedNav catalog is planned.

## Observation and receiver mappings

- [ ] Complete the source-to-canonical observation signal/revision coverage
      table and verify remaining phase/Doppler conventions with independent evidence.
- [ ] Evaluate Meas3 only when explicitly needed; Measurements remains the
      implemented SBF observation input.

## RawBits validation and extensions

- [ ] Validate UBX F/NAV and missing cross-receiver mappings with suitable samples.
- [ ] Validate GPS/QZSS CNAV-2 receiver packing with real samples; documentary
      52 + 1200 + 548 symbol mapping is implemented, not sample-validated.
- [ ] Validate SBF QZSS L1S, QZSS L5S and L6 mappings with real samples.
- [ ] Define legitimate I/NAV alert/partial assembly before claiming its support.
- [ ] Independently validate B1C SF1 BCH and retained LDPC codewords where needed;
      existing CRC checks do not establish complete FEC validation.
- [ ] Add justified L6 service classification and further B2b subtype routing
      only when useful; preserve explicit unclassified families meanwhile.
- [ ] Study Galileo QP/restricted-signal exported representations before adding
      canonical identifiers or mappings.

## Events and downstream consumers

- [ ] Independently verify TIM-TP qErr polarity on appropriate hardware; the
      current mapping retains the documented external experiment's sign choice.
- [ ] Implement agreed cadence findings and their typed payloads, with explicit
      equal-time/conflict semantics. INTERVAL/STATE designs are not current output.
- [ ] Implement and validate required context-aware replay for new Event kinds;
      do not infer continuity from the absence of unsupported events.
- [ ] Add CommonNEX PPP input and integrate Setup calibration where needed by
      remaining consumers. STEC already uses the selected ANTEX companion.
- [ ] Extend the [realtime broadcast navigation consumer](../stec-realtime.md)
      beyond GPS/QZSS LNAV to Galileo I/F-NAV and BeiDou D1/D2.
- [ ] Join measurement-clock telemetry into realtime phase processing without
      treating a measurement adjustment as receiver reboot.
- [ ] Implement overlap reconciliation only on explicit request; the current
      input contract remains one continuous non-overlapping recording path.

## Deferred external sensor records

No relevant sensor hardware is installed yet; defer implementation.

- [ ] Investigate SBF ASCIIIn/raw NMEA for external temperature, humidity and
      pressure, including input-port, timestamp and fragmentation semantics.
- [ ] Define a separate `raw-txt` catalog associated with navigation epochs;
      preserve payload bytes and order without assuming UTF-8 or deduplicating.
- [ ] Keep sensor parsing, units and calibration downstream; unknown time must
      not be replaced with fabricated GPST.

## Streaming extensions

- [ ] Add durable live daily Parquet publication only with an explicit design
      for transport-discontinuity persistence and incomplete-tail recovery.
      The current live API delivers bounded batches/IPC, not a durable logger.
