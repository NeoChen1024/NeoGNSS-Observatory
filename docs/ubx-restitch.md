# UBX archive reconstruction

`ubx-restitch` reconstructs expanded UBX archives using a native C++ scanner,
exact overlap proofs, and a Python/Click orchestration layer. It does not
decompress inputs, decode navigation subframes, or modify source files.

Build the CMake applications first. The archive-index worker additionally
requires OpenSSL Crypto development files for source SHA-256 calculation.
Install the Python package to expose the `ubx-restitch` command.

```sh
ubx-restitch inventory --input-dir /path/to/ubx24h --state-dir /path/to/state \
  --indexer /path/to/cppubx2_archive_index
ubx-restitch run --input-dir /path/to/ubx24h --state-dir /path/to/state \
  --indexer /path/to/cppubx2_archive_index --output-dir /path/to/era-a --plan-only
ubx-restitch run --input-dir /path/to/ubx24h --state-dir /path/to/state \
  --indexer /path/to/cppubx2_archive_index --output-dir /path/to/era-a
```

Inventory caches are keyed by resolved source path, size, modification time,
and worker executable SHA-256. Each index records the full source SHA-256 and
is itself checksummed. Three native workers scan files concurrently; progress
uses tqdm on stderr. The input directory's `*.ubx` files are selected directly,
not recursively. State storage must be writable and separate from input data.

## Reconstruction policy

- Validate sync, declared length, and UBX checksum. After an invalid candidate,
  resume scanning one byte later instead of trusting its declared length.
  Record all non-UBX/corrupt spans, including gpsd text headers.
- Group valid frames through NAV-EOE while preserving original byte order.
  If EOE is absent, a known epoch transition closes the preceding run.
- NAV-PVT's valid UTC date/time anchors the integer-second timeline.
  RAWX timestamps are rounded to the nearest nominal second for grouping
  because receiver clock steering can straddle an integer-second boundary.
  Original RAWX bytes are unchanged. Index records retain RAWX GPS week when
  present and NAV iTOW, together with time-source flags.
- A known NAV timestamp may inherit UTC from a PVT anchor within 60 seconds.
  Unanchored time, conflicting timestamps, and leap-second labels are not
  silently converted into authoritative UTC.
- Sort sources by payload coverage, not filenames. A join needs two consecutive
  exact EOE intervals plus byte-for-byte verification of the entire discarded
  incoming head and outgoing tail. Fingerprints only locate candidates.
  Unverified or nested overlaps and time reversals stop planning before output.
- Split at UTC midnight or a missing NAV-bearing second. Same-second records
  are considered together, so a damaged individual frame is not automatically
  treated as a missing second. No samples or timestamps are fabricated.
- Name each timed segment `%FT%T%z.ubx`, with UTC `+0000` and the actual
  first available second. Filename collisions are errors, never overwrites.
- Untimed valid frames and intervals without NAV are preserved separately in
  `unassigned/`. For example, asynchronous frames before the first timestamp
  after a missing EOE cannot necessarily be attributed to that next epoch.
  Their original byte offsets and ordering remain available.

The EOE grouping convention does not claim that asynchronous SFRBX messages
have the same transmit time as the adjacent NAV solution. This first version
does not implement SBAS extraction or constellation-specific subframe parsing.

## Outputs and verification

- UTC-named UBX files: the reconstructed NAV-bearing segments.
- `unassigned/*.ubx`: valid frames requiring further timing review.
- `plan.jsonl`: source selections, exact overlap proofs, output byte spans,
  gap/anomaly events, and byte-accounting totals.
- `provenance/`: checksummed source indexes, a snapshot of the implementation,
  the native worker, build configuration, dependency versions, and Git revisions.
- `.artifacts/*.json`: per-output source spans, size, and verified SHA-256.
- `completed.json`: written only after all artifacts have passed verification.

Every selected byte is accounted for exactly once as output or a recorded
non-UBX/corrupt span. Discarded duplicate bytes require explicit overlap proof.
Non-UBX/corrupt bytes remain in their checksummed preservation source; they are
not inserted into UBX outputs.

Output requires an empty directory or a matching run manifest. Each artifact
is written exclusively to a temporary file, flushed, SHA-256 checked by reading
it back, then published without replacement. Completed artifacts can be reused
on a rerun only after checksum verification. Interrupted partial files remain
for inspection. An interrupted provenance initialization, or an existing final
file without completion metadata, requires explicit recovery rather than an
automatic overwrite. Do not modify source files between inventory and output;
size/mtime are checked before and after processing.
