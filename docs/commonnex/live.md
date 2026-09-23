# CommonNEX streaming infrastructure

The runtime is shared with historical importing; this is an implementation API,
not an extension of the CommonNEX scientific schema. It accepts receiver bytes
and delivers owned Arrow batches. See [native architecture](../native-architecture.md)
for component ownership and interop requirements.

## Batch delivery

`neognss_observatory.cnex_stream.CnexBatchGroup` contains a session-local
`sequence`, a mapping from catalog names to tuples of PyArrow RecordBatches,
and an optional transport `notice`. Missing catalogs have no rows to deliver.
Archive importing uses the same grouping helper, retaining empty schemas for
its daily writer. A group is not an epoch or an atomic scientific transaction.
Batch boundaries neither complete observations nor reset state.

`CnexStream` is a single-owner Python producer over the native engine:

```python
stream = CnexStream("sbf", "station-setup", period_seconds=1)
stream.feed(chunk)               # at most 64 KiB per live feed
group = stream.drain()           # None if there are no completed records
tail = stream.finish()          # report incomplete tail, never force completion
```

`feed()` preserves receiver frame, measurement, navigation and MeasExtra state.
`drain()` publishes already available records without forcing epoch closure. The output buffering
threshold is 4 MiB, plus the expansion of one bounded input chunk; drain when
the threshold is reached. Feeding again without draining raises instead of
growing indefinitely. Exported batches retain their buffers independently of
the engine. Do not mutate buffers shared with other consumers.

`discontinuity(reason)` drains preceding records with a boundary notice and
creates fresh framing, epoch and receiver-time context for subsequent input.
Consumers apply this notice **after** the group's records. It denotes possible
transport data loss, not a receiver reboot. `finish()` reports pending frame
bytes, measurement epoch and MeasExtra counts; it does not fabricate completion.
Both operations emit pending telemetry with `collection_complete=false` before
ending/resetting the engine. Ordinary file import instead checkpoints pending
telemetry unless `--finalize-telemetry` explicitly declares a true end.
Neither operation writes the transport notice into the CommonNEX Events table.
Consumers must handle notices explicitly; a future persistent live sink must
preserve relevant boundaries rather than silently discarding them.

Within a process, pass group objects directly (optionally through a bounded
thread queue). Never serialize Arrow just to call another Python/C++ function.
Consumers needing Events must inspect their applicability before processing
affected observations; catalog order is not chronological interleaving.

## TCP acquisition

`tcp_groups(host, port, stream, max_latency=5.0, duration=None)` connects once
and yields groups at approximately five-second intervals, or earlier at
the output capacity threshold. Acquisition buffering is bounded to 4 MiB plus
one receive buffer of up to 64 KiB. Empty socket polls do not terminate epochs.
Host monotonic time drives delivery only, never GPST or receiver restart logic.

Five seconds is a delivery target for completed records, not a hard real-time
guarantee: scientific completion, scheduling and a slow consumer can add delay.
At 1 Hz this normally groups about five measurement epochs. A slow consumer
eventually causes an explicit input-buffer-full error; raw bytes are never
silently dropped. TCP EOF/errors deliver a discontinuity notice, then raise.
There is no implicit reconnect or inference that a reconnect is lossless.
Callers may reconnect with the reset producer after handling the boundary.
An explicit duration ends normally; closing the generator stops acquisition.

```sh
ngo-cnex-live -p sbf --host RECEIVER_IPV6 --port 2006 \
  --setup /path/to/setup.json --max-latency 5 --duration 30 \
  --output /path/to/capture.cnex-stream
```

`--output` exclusively creates a framed IPC capture, **not ParquetNEX**.
Without it, the CLI prints JSON group summaries only. Summary row counts always
list all four catalogs in canonical order, including zero counts.
Setup metadata is supplied
separately and must describe the input station; it is not bundled into each
group. Observation input uses antenna 0 in this initial CLI. This command is
not a replacement for a durable raw receiver logger. Interrupt/process failure
may leave an incomplete IPC tail, which the reader detects.

## Cross-process and socket transport

`encode_group` / `decode_group` use this version-1 envelope:

1. Four-byte unsigned big-endian JSON-header byte length.
2. UTF-8 header: `version`, `sequence`, `notice`, and ordered
   `[catalog, payload_byte_length]` entries.
3. One self-contained Arrow IPC stream per listed catalog, including schema,
   any dictionaries and EOS. Each catalog may contain multiple batches.

Total message size is capped at 16 MiB. IPC compression is disabled; the Parquet
Zstd level-3 policy does not apply to this in-memory transport. Separate IPC
streams intentionally repeat schemas so groups can be decoded independently.
This trades modest metadata overhead for no persistent dictionary/session state.

- `send_group` / `receive_group`: dedicated multiprocessing Connection endpoints,
  using `send_bytes` / `recv_bytes`, not pickle.
- `write_group` / `read_group`: blocking binary files or `socket.makefile()`;
  prepend a four-byte unsigned big-endian total-message length. Handles short
  reads/writes. `read_group` returns None at clean EOF and raises on truncation.

Use one writer per channel. Callers track sequence within each connection/session
if needed; it is not an exactly-once ID across reconnects. A queue with several
readers distributes work, not broadcast. Fan-out requires one delivery path per
consumer. Slow paths must have bounded buffering or fail explicitly.

These adapters target trusted local processes or trusted network peers. They
are not an authenticated service: deploy TLS/authentication externally before
exposing them to untrusted clients. The frame cap is not a complete malicious
Arrow decompression/resource-exhaustion defense. Shared memory, Arrow Flight,
automatic replay/reconnect, live daily Parquet publication and streaming versions
of every downstream scientific processor are not implemented by this layer.
