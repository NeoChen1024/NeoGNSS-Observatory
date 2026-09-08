# Dataset QA and optional UBX reconstruction

`ngo-dataset-qa` defaults to read-only dataset QA. Its explicit `restitch`
profile reconstructs overlapping UBX archives using a native C++ scanner,
exact overlap proofs, and a Python/Click orchestration layer. Neither profile
decompresses inputs, decodes navigation subframes, or modifies source files.

See [dataset notes](dataset-notes.md) for Era A's overlap and gap constraints.

Install the Python package with its native extension. OpenSSL Crypto development
files are required when building the native source-integrity calculation.

```sh
ngo-dataset-qa --input-dir /path/to/ubx
ngo-dataset-qa -p sbf --input-dir /path/to/sbf
ngo-dataset-qa --profile restitch --input-dir /path/to/ubx24h --state-dir /path/to/state \
  --output-dir /path/to/era-a --plan-only
ngo-dataset-qa --profile restitch --input-dir /path/to/ubx24h --state-dir /path/to/state \
  --output-dir /path/to/era-a
```

Inputs are selected recursively by default. Pass `--no-recursive` to restrict
traversal. Era B's
nonoverlapping monthly directories can be scanned or extracted directly.
Compressed files are not selected. Progress uses tqdm on stderr.

## Default scan profile

Omitting `--profile` selects `scan`. It reads expanded recordings without
writing indexes, fingerprints, QA stamps, manifests or reconstructed files.
A JSON summary goes to stdout; progress and warnings go to stderr.
State/output/plan options are rejected unless reconstruction is explicitly selected.
A successful scan is not a certification or prerequisite checked by extraction.

Scan carries framing and epoch state across file boundaries. UBX QA reports
checksum/noise, unanchored/conflicting epochs, partial epochs, duplicate completed
epochs, time reversals and gaps greater than `--gap-timeout` (default 50 s).
Different NAV messages within one epoch are not duplicate epochs. A partial
epoch or long sampling interval is reported without automatically proving loss.
SBF validates framing and generated payload schemas, checks native TOW/WNc,
and reports time reversals per block type. MeasEpoch (4027) and PVTGeodetic
(4007) supply cadence/duplicate-epoch-block checks; repeated raw-navigation blocks
at the same time can belong to different satellites and are not duplicates.
Other SBF block types do not imply a sampling cadence. Unsupported schemas are
not silently claimed as fully validated.

Corruption, invalid/unknown time, reversal, duplicate or truncated-tail findings
return exit status 1 after the summary; gaps/partial epochs alone are informational.
I/O or fatal parsing errors return a nonzero error. Inputs must be stable and
ordered; scan does not sort by payload by building a hidden index, and does not
repair data.

Only Era A's overlaps motivate reconstruction. Nonoverlapping UBX/SBF can go
directly to extraction whether or not scan has been run. Extraction performs
necessary parsing and scientific validity checks, not another diagnostic pass.
UBX epoch association is shared by scan, reconstruction and SBAS extraction;
fingerprints and overlap indexes are enabled only for reconstruction.

## Reconstruction policy

The restitch profile selects expanded `*.ubx` files. Its inventory caches are
keyed by resolved source path, size, modification time, and native index-policy
identifier. Each index records the full source SHA-256 and is itself checksummed.
Three Python threads call the GIL-released native scanner concurrently.
State storage must be writable and separate from input data; the default scan
profile does not create this cache.

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
different threshold, pass `--gap-timeout 50` to `ngo-dataset-qa --profile restitch`.
Clock unwrapping and other scientific analyses retain their own gap/QC rules;
a reconstruction timeout does not authorize interpolation across outages.

## Outputs and verification

- GPST-named UBX files: the reconstructed NAV-bearing segments.
- `unassigned/*.ubx`: valid frames requiring further timing review.
- `plan.jsonl`: source selections, exact overlap proofs, output byte spans,
  gap/anomaly events, and byte-accounting totals.
- `provenance/indexes/`: source epoch indexes for reconstruction investigation, not downstream requirements.
  These are integrity/overlap artifacts, not extraction prerequisites.
- `.artifacts/*.json`: per-output source spans, size, and verified SHA-256.
- `completed.json`: written only after all artifacts have passed verification.

Indexes use `UBXIDX04` with integer `gpst_ms`; plans use schema 3.
Artifact bounds include exact `start_gpst_ms` / `end_gpst_ms`, `nav_epochs`,
and `max_observed_interval_ms`. Unsuffixed `start_gpst` / `end_gpst` fields
are seconds and may be fractional, for downstream calendar calculations.
Rebuild/reinstall the native extension after changing indexing policy. The cache
identity includes the index format and native policy identifier. Native code
makes GPST segmentation and quarantine decisions; Python retains overlap byte I/O, publication and coverage
accounting checks.
Use a new output directory for a different plan. Extraction reads the resulting
UBX files directly without consuming the reconstruction manifests or indexes.

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
