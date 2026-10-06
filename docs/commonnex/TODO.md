# CommonNEX remaining work

[Overview](overview.md). This list contains remaining implementation or
independent validation, not completed history. Current mappings and evidence
limits belong in [receiver mappings](receiver-mappings.md). RINEX/RTCM3
acquisition import and a DecodedNav catalog remain outside the selected scope.

## Observation and receiver validation

- [ ] Complete the observation signal/revision coverage table and independently
  verify remaining phase/Doppler conventions for supported UBX/SBF mappings.

## RawBits validation and extensions

- [ ] Validate documentary UBX F/NAV packing against real receiver samples.
- [ ] Validate GPS/QZSS CNAV-2 and SBF QZSS L1S/L5S/L6 packing against real
  receiver samples. Constructed containers do not establish RF validation.
- [ ] Define I/NAV alert/horizontal-page normalization and assembly with
  suitable receiver evidence; the retained nominal pair does not establish it.
- [ ] Independently validate retained LDPC codewords where required; CRC success
  is not full FEC validation. B-CNAV1 SF1 BCH decoding/validation is owned by the
  [BeiDou checklist](../broadcast-beidou.md#remaining-scope).
- [ ] Establish justified L6 service classification when independent service
  evidence is available; retain unclassified identity meanwhile.

## Consumer integration

- [ ] Add CommonNEX Observation input to PPP and use Setup station/antenna
  context. Preserve the supported GPS Float contract while replacing its
  separate receiver-normalization input path.
- [ ] Join measurement-clock telemetry into phase processing with explicit
  observation-time association. Distinguish receiver clock adjustments from
  restart Events; tracking and restart handling alone do not supply this join.

## Deferred acquisition and mapping work

Meas3, Galileo QP/restricted-signal mappings and overlap reconciliation require
an explicit use case before implementation. The current acquisition contract is
one ordered, continuous, non-overlapping recording path; unknown signal mappings
remain counted rather than guessed.

External temperature/humidity/pressure ingestion is deferred until concrete
hardware and inputs are available. Evaluate SBF ASCIIIn/raw NMEA, source time,
fragmentation, units and calibration before selecting a record schema.

- [ ] Design durable live daily Parquet publication before implementing it,
  including discontinuity persistence and incomplete-tail handling. The current
  bounded batch/IPC API is not a durable logger.
