# UBX archive reconstruction

`ubx-restitch` reconstructs expanded UBX archives using a native C++ scanner,
exact overlap proofs, and a Python/Click orchestration layer. It does not
decompress inputs, decode navigation subframes, or modify source files.

For the original Era A `gpspipe` recording command and the later direct TCP
continuity comparison, see [Era A recording provenance](era-a-recording-provenance.md).
The historical acquisition did not use the old C++ logger; the source of its
frequent gaps remains unconfirmed.

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
not recursively by default. Pass `--recursive` to either command to include
expanded `.ubx` files in subdirectories, such as Era B's monthly directories.
Compressed files are not selected. State storage must be writable and separate
from input data.

## Reconstruction policy

- Validate sync, declared length, and UBX checksum. After an invalid candidate,
  resume scanning one byte later instead of trusting its declared length.
  Record all non-UBX/corrupt spans, including gpsd text headers.
- Group valid frames through NAV-EOE while preserving original byte order.
  If EOE is absent, a known epoch transition closes the preceding run.
- NAV-TIMEGPS week and NAV/EOE iTOW anchor an integer-millisecond GPST
  timeline. Subsecond epochs remain distinct, including 10 Hz input; there is
  no nearest-second snapping. Original UBX bytes remain unchanged.
  RAWX carries a steered measurement time, not necessarily the NAV timestamp.
  Within an EOE interval, NAV time takes precedence; nonempty RAWX can supply
  the week when TIMEGPS is missing. A difference greater than 500 ms between
  the associated measurement and NAV time is a conflict, not a reason to
  round NAV time. RAWX-only runs retain their measurement time rounded to
  milliseconds and are excluded from NAV time-reversal checks. Empty RAWX
  reports cannot split or anchor epochs. Week carry is handled explicitly.
- A known NAV timestamp may inherit GPST from a GPS anchor within 60 seconds.
  Unanchored time and conflicting timestamps are not silently promoted to
  authoritative GPST. NAV-PVT calendar fields never anchor this time axis.
- Sort sources by payload coverage, not filenames. A join needs two consecutive
  exact EOE intervals plus byte-for-byte verification of the entire discarded
  incoming head and outgoing tail. Fingerprints only locate candidates.
  Unverified or nested overlaps and time reversals stop planning before output.
- A complementary boundary may instead join an outgoing partial RAWX-bearing
  interval without NAV/EOE to an incoming NAV-PVT/EOE interval without RAWX.
  Both intervals must touch their physical file boundaries and their times
  must differ by no more than 500 ms. The incoming NAV time is assigned to the
  joined interval; the original measurement time is recorded in the join.
  This complementary
  split epoch retains every byte and records a `split_epoch_continuation` join;
  it does not count as duplicate removal.
- Split at GPST midnight or when successive available NAV epochs are more
  than `--gap-timeout` seconds apart (default **50 seconds**). Exactly 50
  seconds remains in the same segment. This supports 30-second low-rate input
  without declaring every epoch a separate file. The CLI value is rounded to
  milliseconds. No sampling rate is inferred and no missing samples are filled.
  Timed intervals without NAV remain in `unassigned/`; they do not immediately
  split the surrounding segment if the next NAV arrives within the timeout.
  A segment is a timeout-bounded run, not a claim of gapless observations.
- Name each timed segment `GPST-%Y-%m-%d--%H-%M-%S-mmm.ubx`, where `mmm`
  is exactly three decimal digits from the first available epoch, including
  `000`. For example: `GPST-2024-01-23--06-20-38-500.ubx`.
  Filename collisions are errors, never overwrites.
- Untimed valid frames and intervals without NAV are preserved separately in
  `unassigned/`. For example, asynchronous frames before the first timestamp
  after a missing EOE cannot necessarily be attributed to that next epoch.
  Their original byte offsets and ordering remain available.

The EOE grouping convention does not claim that asynchronous SFRBX messages
have the same transmit time as the adjacent NAV solution. SBAS extraction is
a separate downstream stage.

The timeout is recorded in the plan and artifact metadata. To choose a
different threshold, pass `--gap-timeout 50` to `ubx-restitch run`.
Clock unwrapping and other scientific analyses retain their own gap/QC rules;
a reconstruction timeout does not authorize interpolation across outages.

## Outputs and verification

- GPST-named UBX files: the reconstructed NAV-bearing segments.
- `unassigned/*.ubx`: valid frames requiring further timing review.
- `plan.jsonl`: source selections, exact overlap proofs, output byte spans,
  gap/anomaly events, and byte-accounting totals.
- `provenance/indexes/`: source epoch indexes required by downstream time mapping.
  This directory name is retained, but no implementation, worker or environment
  snapshot is created.
- `.artifacts/*.json`: per-output source spans, size, and verified SHA-256.
- `completed.json`: written only after all artifacts have passed verification.

New indexes use `UBXIDX04` with integer `gpst_ms`; plans use schema 3.
Artifact bounds include exact `start_gpst_ms` / `end_gpst_ms`, `nav_epochs`,
and `max_observed_interval_ms`. The existing `start_gpst` / `end_gpst` fields
remain seconds, now possibly fractional, for downstream calendar calculations.
Rebuild the native indexer before inventory. The cache identity includes the
index format and worker hash, so old second-based indexes are not reused.
Old outputs are not renamed or overwritten: use a new output directory for
the new plan. Downstream readers can still inspect existing GPST products by
their explicitly identified format; no UTC compatibility mode is added.

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
