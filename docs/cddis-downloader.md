# CDDIS product downloader

This first version inventories and fetches an explicit product selection. It
does not convert observations, solve PPP, estimate TEC, or certify payload time
coverage. All original compressed bytes and filenames are preserved.

## Setup

Use Python 3.11 or newer on Linux (the root lock uses `flock`). Install with
`python -m pip install -e .`. `gzip` is additionally required for `.Z` inputs.
Runtime dependencies use minimum versions (`>=`) in `requirements.txt`, with
no lockfile. To upgrade, run `python -m pip install --upgrade -e .`. Each run
records actual installed dependency versions and source hashes in provenance.

Create or update `~/.netrc` locally, then set its permissions to `0600`:

```text
machine urs.earthdata.nasa.gov
  login YOUR_EARTHDATA_USERNAME
  password YOUR_EARTHDATA_PASSWORD
```

Visit the [CDDIS archive](https://cddis.nasa.gov/archive/) in a browser once and
approve the Earthdata application when prompted. Credentials are read only from
netrc and sent only to `urs.earthdata.nasa.gov` over HTTPS. The tool maintains a
`0600` cookie cache at `~/.cache/neognss-observatory/cookies.txt`; it has no cookie
import, token, password argument, or environment-variable authentication mode.
The standard `XDG_CACHE_HOME` environment variable can relocate the cache.
An invalid or expired login stops new work. For a redirect loop caused by stale
cookies, move this cache aside and retry. No credentials are stored in plans or
manifests. HTTP proxy environment variables are intentionally not consumed.

## Configure and run

Copy `config/products.example.toml` to a deployment-specific location, such as
`work/products.toml`. The example selects CODE MGEX Final ORB, CLK, ERP and OSB,
IGS combined broadcast NAV, CODE Final IONEX, and a named IGS20 ANTEX revision.
All of these downloads are required in this example. BLQ is reported separately
as a pending, optional station-specific input. Bias interpretation and ANTEX
compatibility with the solver and observation metadata must be verified before
scientific processing; downloading a matching filename does not establish them.

```sh
cddis-download plan --config work/products.toml --output work/plan.jsonl
cddis-download fetch --plan work/plan.jsonl --root work/products
cddis-download status --root work/products
cddis-download verify --root work/products
```

`--profile` aliases `--config`. `plan --start YYYY-MM-DD --end YYYY-MM-DD`
overrides configuration dates. CLI overrides are reflected in the plan's
resolved fields; the configuration hash identifies the original file.
`--netrc PATH` is accepted by `plan` and `fetch` and defaults to `~/.netrc`.

## Inventory interruption and resume

Each plan output has a separate checkpoint database beside it, for example
`work/plan.jsonl.inventory.sqlite`. Every successfully parsed directory listing
or numeric directory index is committed immediately. Only parsed file names,
sizes, directory numbers and evidence are stored; login HTML and credentials
are never cached. A failed or truncated response is not cached.

If planning fails, rerun the same command with the same configuration, resolved
date overrides and output path. Completed queries are replayed locally, with a
`cached` counter in the progress display, and only remaining queries need the
network. Original query timestamps/fingerprints are preserved in the plan.
The SQLite checkpoint is independent of the download manifest and protected
by a planner lock. A different output path has an independent checkpoint.

After the JSONL plan is atomically written, its checkpoint is marked complete.
The next plan run then starts a fresh inventory, allowing newly published
products to appear. Changed configuration also starts fresh. To explicitly
discard an unfinished snapshot, use `plan --refresh-inventory ...`. This clears
only that output's generated checkpoint, not its existing plan or downloads.
An interrupted run otherwise keeps its earlier view, including any cached empty
listings; refreshing is necessary to discover changes to those listings.

Directory response-body interruptions and invalid UTF-8 now get bounded
retries and backoff as well as the existing connection/status retries. A final
failure reports the affected archive path and error class without auth queries.
Static input headers and the local checksum scan are repeated on resume; only
directory queries are checkpointed. Existing plans made before this mechanism
remain usable and do not require re-inventory, but their old runs cannot gain
checkpoints retroactively.

`work` is ignored whether it is a directory or a symlink to another filesystem.
Paths in the manifest are relative to the download root. Existing preservation
masters under `/hdd` must never be selected as output locations.

## Planning and dates

Dates are inclusive UTC processing-date labels. `guard_days` adds explicit
adjacent candidate days (default one on either side). GPS week numbers group
these calendar labels for directory lookup and rough progress only; this is
not conversion of UTC observation epochs to GPST. Product nominal dates are
filename-derived, and every file is marked `coverage_status = unverified`.
Downstream processing must inspect payload epochs and preserve GNSS time and UTC.

Layouts are `week` (`base/<GPS-week>/`), `year` (`base/<year>/brdc/`), and `day`
(`base/<year>/<DOY>/`). Patterns accept `{yyyy}`, `{yy}`, `{doy}` and `{week}`.
Each dated product must match exactly one file per candidate day. Zero matches
are missing; multiple matches are ambiguous and none is silently chosen. This
version's example uses daily products; weekly/monthly validity expansion and
legacy naming changes require explicit future selectors, not broad wildcards.
`static` inputs currently allow explicit IGS station/general HTTPS URLs.

Without `end`, plan discovers numeric directories from live CDDIS indexes,
inventories the range, and freezes the actual latest selected nominal date.
Each product's latest selected date and latest directory bound are recorded.
Empty/missing slots are retained and the process does not stop at the first
gap. A new plan discovers subsequent publications; fetch never expands its
existing plan. Creating a plan queries listings and static response headers,
not product bodies. Listing footer counts must agree with parsed file counts.

During inventory, a Rich progress bar reports completed/total weeks, cumulative
selected files, files needing download, reusable local files and directories
queried. Non-terminal stderr receives the same counters once per completed week.
Pending counts exclude manifest-complete files whose identity and on-disk size
still match. This is a planning estimate, not a hash scan: fetch revalidates
the bytes. Counts are refreshed after static input identities are resolved.
Upstream snapshot checksum conflicts are advisory and do not increase the
pending count or invalidate otherwise reusable files.

Plan reports total weeks, inventoried weeks/directories, weeks requiring
transfers, weeks with missing required inputs, selected files and known bytes.
Different archive branches count toward the same calendar week. The partial
first/last weeks only cover requested and guard days, not seven complete days.
Local completion shown during planning is manifest-derived with an existence
and size check; status uses the recorded manifest state. `fetch`
revalidates before reuse and `verify` performs an explicit offline check.
The summary separately reports how far required inputs are available and
downloaded continuously from start, including guard-day and static-input
requirements. These are input-completeness measures, not payload coverage claims.

The plan is a JSON Lines (`.jsonl`) execution snapshot with one JSON object per
line: one `header` (schema 2), individual `file`, `slot` and `inventory` records,
then a `footer` containing record counts and SHA-512 of all preceding bytes.
The reader checks the footer before accepting the plan, rejecting truncation,
changed rows and extra trailing records. Writing is incremental and atomic;
reading parses lines individually rather than loading a giant JSON string.
The planner and executor still retain their working file/slot collections in
memory; JSON Lines does not make the entire pipeline constant-memory.

The snapshot includes listing fingerprints, tool source hashes, dependency versions, resolved
product configuration, slots, missing inputs, and candidate URLs. Keep it with
the run. Changed upstream files require a new plan; there is no silent fallback
to another center, solution class, or mirror.
Previously generated schema-1 `.json` plans remain readable by fetch/status;
new plans are always JSON Lines. Summary JSON on stdout and small `.part.json`
sidecars retain their existing formats.

## Optional upstream checksums

The archive-wide `gnss/products/SHA512SUMS` can be stale and around 1.6 GiB. It is
never downloaded automatically and is never an availability index. Configure
a local snapshot in the deployment configuration:

```toml
[checksum]
source_url = "https://cddis.nasa.gov/archive/gnss/products/SHA512SUMS"
local_file = "/path/to/SHA512SUMS"
algorithm = "sha512"
```

After selecting files from live listings, plan streams the snapshot once,
hashes it, and retains only selected basenames. It records source URL, snapshot
SHA-512, byte count, malformed-row count and expected hashes. Preserve the
original snapshot in deployment storage for independent reproduction. Memory
use depends on selected names, not the ten million rows of the source.

Matching is restricted to the source URL's directory tree. Multiple selected
paths with the same basename, or conflicting hashes for a name, are ambiguous.
The flat snapshot cannot prove uniqueness of unselected remote paths; matching
is a scoped candidate verification, not recovery of the lost directory paths.
Files absent from the snapshot are downloaded normally and marked unavailable
for upstream verification. MD5 snapshots are supported only when explicitly
configured with `algorithm = "md5"`; SHA-512 mismatch never triggers downgrade.

Every completed file has a local SHA-512. Upstream `verified`, `mismatch`,
`unavailable`, and `ambiguous` are distinct from transfer completion. The
snapshot is auxiliary evidence, not the source of truth: a mismatch records
the expected digest, algorithm, actual digest (`upstream_actual_digest`), and
snapshot provenance, and prints a warning. It does not alone fail a transfer,
quarantine bytes, or prevent reuse. Full compressed-stream integrity, size,
format-header checks and recorded local SHA-512 checks still apply. Successful
decompression does not prove scientific correctness or upstream authenticity.
No automatic per-directory checksum retrieval is performed in this version.

Existing plans remain usable with this policy; there is no need to re-inventory
just to relax snapshot verification. Re-run fetch to retry previously failed
transfers. Existing quarantine files are left untouched, not automatically
adopted or deleted. Verify also reports snapshot mismatches as warnings, while
changes relative to the recorded local SHA-512 remain errors.

## Transfers and recovery

The default limits are two workers and two request starts per second globally,
15-second connect timeout, 120-second read-idle timeout, and five retries.
HTTP 429/5xx and connection failures use exponential backoff, honoring
`Retry-After`. Interrupted response bodies have their own bounded retry loop.
404 is recorded without retry. Redirects are restricted to CDDIS, Earthdata
and IGS HTTPS origins, and authentication never follows a redirect to IGS.
CDDIS's known HTTP archive redirect after login is upgraded to HTTPS locally
before any request is sent; other plaintext redirects are rejected.

Data is written beside its destination as `.part`, with a small atomic JSON
sidecar. Resume requires a matching source identity and a strong ETag or
Last-Modified validator. It uses Range/If-Range and validates Content-Range.
An ignored Range restarts from zero; unverifiable partial bytes are quarantined.
Size, compressed-stream integrity, configured format signature, local SHA-512
are checked before an atomic rename. Applicable upstream checksums are compared
as advisory evidence, not required to match.
Format signatures are not full scientific payload validation.

Re-running fetch reuses valid files after checking them again. Damaged existing
files and conflicting transfers are retained with `.quarantine-<unique-id>`
suffixes. No existing product is silently discarded. A crash between rename
and database update can be recovered by validating and adopting the final file.
One writer may operate on a download root at a time; transfers inside it run
concurrently. Ctrl-C stops new work, retains partial files and exits 130.

`manifest.sqlite` stores plans, current file records and append-only events.
Records contain transfer attempts, sizes, hashes, validators where supplied,
tool identity, source/product provenance and errors. Plans and manifest data
are generated artifacts and stay outside Git. `verify` updates integrity state
offline; `status` opens SQLite read-only and reports recorded state.

Rich progress goes to stderr and refreshes at four frames per second. When
stderr is not a terminal, output is simple completion/error lines. Structured
command results are JSON on stdout.

Exit codes: 0 success; 1 failed/missing required downloads or verification
failure; 2 CLI/configuration errors; 3 authentication/authorization failure;
130 interruption. `plan` returns 0 when inventory succeeds even if it contains
gaps, so available files can still be fetched. Optional missing inputs generate
reported gaps without making fetch fail.

## Validation

```sh
python -m unittest discover -s tests -v
black --check python-src tests
isort --check-only python-src tests
```

The initial live validation on 2026-09-05 used Python 3.13 and the example's
2025-04-06 through 2025-04-12 interval with one guard day on either side:
55 files, 77,872,993 compressed bytes, all downloaded and verified. A second
fetch reused all 55 files with HTTP disabled, and offline verification passed.
The supplied archive-wide SHA512SUMS snapshot had no matching records for this
selection, so upstream verification correctly remained unavailable. Fresh
netrc login and open-ended recent-date discovery were also exercised. BLQ
remained an explicitly reported optional input, not a downloaded product.

References: [CDDIS archive access](https://www.earthdata.nasa.gov/centers/cddis-daac/archive-access),
[IGS MGEX products](https://igs.org/mgex/data-products/),
[IGS antenna models](https://igs.org/wg/antenna/).
