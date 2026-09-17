# Mosaic SBF mirror and FTPS forwarding

`ngo-mosaic-push` is a single-receiver companion daemon for a host such as a
Raspberry Pi 5 running Linux. The current network implementation uses Linux TCP
keepalive options and the archive lock uses `fcntl`. It downloads closed daily
SBF files over plain FTP on the receiver network, verifies local xz compression,
and forwards the archives to independently
enabled FTPS targets. It runs as an unprivileged account and never writes to or
deletes from the receiver. No native GNSS processing or SQLite database is involved.

## Configuration and invocation

Install the project into its repository-root uv environment as described in the
[README](../README.md). Install the system `xz` executable as well. All operational
settings and credentials live in one JSON file; `.netrc` is not used.
The companion itself uses only the Python standard library, Click and Rich; it
does not load the native GNSS extension. A dedicated receiver-host deployment can
use a uv-managed environment with those two dependencies and the package's
`mosaic_push` and shared `download_common` modules.

Start from [mosaic-push.example.json](../config/mosaic-push.example.json), replace
the endpoints and credentials, and choose an absolute `archive_root` on the SSD.
The daemon account needs write access to that directory. Limit configuration read
access to that account (for example mode `0600`, or root ownership and a dedicated
read-only group with mode `0640`). Passwords are not printed in normal error logs.

```sh
ngo-mosaic-push --config /etc/ngo-mosaic-push.json --check-config
ngo-mosaic-push --config /etc/ngo-mosaic-push.json --once
ngo-mosaic-push --config /etc/ngo-mosaic-push.json --once --xz-threads 3
ngo-mosaic-push --config /etc/ngo-mosaic-push.json
```

`--check-config` validates syntax, values and referenced local paths/groups without
connecting or creating files. It does not verify runtime write permissions,
available storage or server capabilities. `--once` performs a real single
scan, local recovery and forwarding cycle; it retains date eligibility rules and
returns nonzero on transfer/processing failures. Missing-date warnings alone do
not fail the cycle; a recorded unfinished source that is no longer downloadable
does fail it. Without either option the command runs continuously. A lock
on the archive root prevents overlapping instances.
`--xz-threads N` overrides JSON `compression.threads` for this invocation (1 to 64),
in both daemon and oneshot modes. It controls xz's internal threads, not the number
of files compressed concurrently. Without it the JSON value is used.

`destinations` is an object keyed by stable target names, for example:

```json
{
  "destinations": {
    "primary": {
      "enabled": true,
      "host": "archive.example.net",
      "port": 21,
      "username": "replace-me",
      "password": "replace-me",
      "tls": "explicit",
      "verify_tls": true,
      "ca_file": null,
      "path": "/archive/%Y/%m/%d"
    },
    "backup": {"enabled": false}
  }
}
```

Each target has its own boolean `enabled` (default `true`), credentials, TLS
settings and path template. A disabled target may omit connection fields, or
retain its full settings for later re-enabling. Names contain 1 to 64 ASCII
letters, digits, underscores, dots or hyphens, starting with a letter or digit.
They identify targets in logs and completion records; JSON ordering is not identity.
Unknown fields, duplicate names and non-boolean `enabled` values are rejected.
An enabled target requires `host`, `port`, `username`, `password` and `path`;
connection values on disabled targets are not validated until enabled.

For download/compression only, use `"destinations": {}`, omit the field, or disable
every target. No destination connection is attempted and stored completion records
are retained. Restart after changing configuration. Enabling or adding a target
forwards managed `.xz` files still present locally without redownloading or
recompressing them. Manually deleted local archives cannot be backfilled to a new
target unless they can still be mirrored from the receiver.

The previous singular `destination` configuration is no longer accepted: move its
object under a chosen name, such as `"destinations": {"primary": {...}}`. State
versions 1 and 2 migrate to version 3 on startup, preserving local metadata.
Version-1 receipts are assigned to enabled targets with matching normalized
endpoint signatures. Existing archives are not recompressed: missing SHA-512
values are computed by streaming decompression, local sidecars are created, and
each enabled target receives its checksum before gaining a current receipt.
Remote archives undergo SIZE checks and are reused when appropriate. Keep a
backup of both state and application before upgrading; older versions cannot
read version-3 state.

The source defaults to port 21, username `anonymous`, and an empty password; the
client performs anonymous FTP login without prompting or reading `.netrc`.

[mosaic-push-local.example.json](../config/mosaic-push-local.example.json) shows
local-only mirroring from `mosaic-x5` into `~mosaic/BEE0`. Home-directory notation
is expanded before checking that `archive_root` is absolute. When a systemd unit
uses an archive under `/home`, set `ProtectHome=read-only` and explicitly allow that
archive in `ReadWritePaths`; `ProtectHome=true` hides it entirely. Stop the daemon
before manually running `--once` against the same archive, then restart it.

The [systemd example](../deploy/ngo-mosaic-push.service) uses a dedicated
`mosaic-push` user. Create that account and the archive directory, install the
configuration with suitable ownership, and adjust `ExecStart` and
`ReadWritePaths` before installing/enabling the unit. The sample intentionally
allows writes only to the archive root and a private temporary directory. It
requires the application and configuration to be accessible outside protected
home directories. Logs and coarse transfer progress go to stderr/the journal.
Interactive terminals use Rich formatting; redirected stderr and systemd use plain,
unwrapped log lines without a second timestamp or source-file column. At INFO level,
successful transfer phases emit one completion message; phase starts are DEBUG-only.
SIGTERM/SIGINT stop work, close tracked network sockets, and terminate xz.

### Local output permissions

Optional `output.group` and `output.mode` apply to newly produced local `.xz`
archives and `.sha512` sidecars on POSIX systems. For example:

```json
{"output": {"group": null, "mode": "0644", "dirmode": "0755"}}
```

`output.dirmode` sets permissions on newly created date subdirectories;
`output.group` also applies to those new directories. Pre-create the archive root
with the desired group and traversal permissions: the CLI lock creates a missing
root using umask before the output-permission code runs, so these settings do not
adjust the root itself.
Group accepts an existing group name or numeric GID; the service account must
have permission to assign it. Mode is a three- or four-digit octal string
(`"644"` or `"0644"`); special permission bits are not accepted. Omitted or null fields leave that attribute untouched,
using normal creation and umask behavior. Group is applied before mode, after
content verification and before the archive is published by rename. Permission
errors fail the affected stage. A failure before normal raw cleanup retains the
downloaded copy; a checksum error on an existing archive leaves that archive intact.
Existing archives (including adopted archives), existing directories, internal
state and remote FTPS permissions are not modified by these options. Re-created
checksum sidecars receive the current output settings. Parent directory traversal
permissions must be arranged separately if other users need archive access.

## Source selection and dates

With source prefix `/DSK1/SSN/GRB0051`, the daemon expects:

```text
/DSK1/SSN/GRB0051/26258/bee_2580.26_
```

`station` is the exact four-character lowercase IGS filename prefix, not a local
station-directory name. `year_base` explicitly resolves two-digit years:
`2000 + 26 = 2026`. It must be a century multiple; change it deliberately for a
new century. Only valid `YYDDD` directories and their expected daily SBF names
are considered. Dates are checked for leap years and filename consistency.

Every scan sorts the receiver's existing valid date directories and excludes
its **oldest two existing dates** from downloading, reducing collisions with
receiver `delete oldest` retention. A matching uppercase `.A` file marks active
logging and excludes that day's download, even if the final name also exists.
The receiver's listing and metadata are checked again around each download.
This cannot prevent source deletion during transfer; incomplete downloads never
become published archives and remain retryable.

The daily gate defaults to **00:10 UTC**. Before the gate, only dates through the
day before yesterday are eligible; afterwards yesterday is eligible too.
Receiver date labels are preserved rather than converted between GPST and UTC.
This copies receiver-named files; it does not claim full-day scientific coverage.
FTP `MDTM` timestamps are UTC protocol metadata, independent of filename dates.

Startup immediately scans the eligible backlog. Subsequent scans occur after
`retry_seconds` (600 by default) and at the next daily gate, without interrupting
an in-flight cycle. First startup follows the same policy and processes eligible
history in date order, subject to capacity and retry admission. Existing archive
upload jobs are scheduled before new downloads. Three independent workers handle downloading, compression and forwarding.
Each completed download is queued for compression immediately, and each verified
archive is queued for forwarding without waiting for the remaining downloads.
With a storage limit, admission is serialized across complete file pipelines to
keep the workspace bounded and avoid queue deadlocks; the three worker threads
remain separate.
`--once` waits for the planned work in all three stages to drain; it does not wait
through future retry deadlines. If a source changed while its older local version
was still awaiting delivery, that cycle delivers the old version first and a later
scan plans the replacement.
Existing downloaded work can still be compressed/uploaded when the receiver is
unreachable or no longer retains the original date.

Missing directories between the earliest retained date and the eligible cutoff,
missing closed files, and previously recorded unfinished sources that disappear
produce warnings, at most once per date/key per daemon UTC day. The daemon does
not infer missing history before the oldest retained date. Intentionally skipped
oldest dates, active files, and already archived dates are not missing-file alarms.

## Archive and transfer state

Local output is fixed, with no station directory:

```text
<archive_root>/2026/09/15/bee_2580.26_.xz
<archive_root>/2026/09/15/bee_2580.26_.sha512
```

Source `SIZE` and `MDTM` are required. Source size and timestamp are compared with
saved metadata, not with compressed size. A difference causes a fresh download
and compression. Downloaded bytes must match the advertised size and the source
must remain unchanged and closed. xz output is decompressed and its SHA-512 is
compared with the downloaded SBF before publication. This validates lossless
storage, not the scientific content or completeness of SBF observations.

A small atomic JSON state file in `.mosaic-push/state.json` records source
metadata, local completion and per-target destination checks in each file's `uploads`
map. It contains no passwords. Disabled or removed targets' receipts remain in this
map. Receipts retain the archive identity and hash they covered; old receipts no
longer satisfy completion after a local revision. Renaming a target
is treated as adding a target, while reordering targets preserves their receipts.
Private temporary SBF copies live under `.mosaic-push/raw/`. Normal cleanup
follows verified xz and checksum publication with saved state. Copies belonging to
a superseded source version are discarded before downloading its replacement. Failed compression
retains a complete raw copy for retry. Partial downloads are preserved for REST
resume when source SIZE/MDTM still match their saved identity.
Keep the state with the managed archives. An existing `.xz` without state is adopted
only after a fresh receiver download and a streaming comparison of its decompressed
SHA-512 with the downloaded SBF. A matching archive is retained byte-for-byte, with
its original mtime. A mismatch leaves both the existing archive and private raw
download intact and reports an error. Pre-existing uncompressed SBF files outside
the tool's private raw directory are never removed or used as download scratch space.
Source configuration is
bound to the archive state to prevent accidentally mixing receivers.

Final `.xz` files are retained indefinitely unless `storage.limit_gib` is set.
Users may remove old completed archives. If the receiver still has an eligible
copy, the mirror will download it again. Files outside the receiver's eligible
range are not reconstructed after manual cleanup. Pending work without a local
archive is warned about; source and remote files are never deleted as a cleanup
policy. Existing unrelated local archives are not overwritten.

## FTPS publication

Each `destinations.<name>.tls` is `explicit` (AUTH TLS, usually port 21) or `implicit` (TLS from
connection establishment, usually port 990). Set `port` explicitly, including for
nonstandard ports. `verify_tls` defaults to `true`: TLS certificates and hostnames
are verified in both modes. `ca_file` may name an absolute custom CA bundle;
`null` or omission uses the system trust store. An empty string is not a valid CA
file path. Set `"verify_tls": false` to disable both certificate-chain and hostname
verification for the destination's control and data connections. TLS encryption
remains enabled, but the server's identity is not authenticated. In this mode
`ca_file` is ignored. Restart the daemon after changing these settings.
The client offers the control connection's TLS session for data-channel reuse.

The remote `path` follows the substitutions documented for mosaic-X5
`setFTPPushSBF` in the
[v4.14.10 reference guide, page 236](https://www.ardusimple.com/wp-content/uploads/2024/10/mosaic-X5-Firmware-v4.14.10-Reference-Guide.pdf#page=236):

| Sequence | Meaning | Example for 2026-09-15 |
| --- | --- | --- |
| `%Y` | Four-digit year | `2026` |
| `%y` | Two-digit year | `26` |
| `%m` | Two-digit month | `09` |
| `%d` | Two-digit day of month | `15` |
| `%j` | Three-digit day of year, starting at 001 | `258` |
| `%%` | Literal percent | `%` |

These use the **source file's date**, never the current upload date. Unknown
sequences are rejected. `/archive/%Y/%m/%d` and `/archive/%y%j` are examples.
Relative remote paths are relative to the login directory; absolute paths are
relative to the FTP server root. Parent traversal is rejected. The companion does
not impose the receiver's 80-character expanded-path limit. Missing directories
are created and the original filename plus `.xz` is appended.

Remote archive existence is checked by compressed **size**, never mtime. A known
local revision replaces an older version even when compressed sizes match. For a
previously unknown equal-size remote file, SIZE alone is not content verification.
Data is uploaded to `<filename>.ngo-mosaic-push.part`, checked by SIZE, renamed to
the final name, and checked again. The destination must support SIZE, directory
creation, and RNFR/RNTO replacement. No delete-before-rename fallback is used.
Only one writer should manage these remote names.

An owned partial upload may resume with REST/STOR when its saved target and local
archive identity still match. Unknown, oversized or obsolete partials restart at
zero. A full owned partial can be published after a lost completion reply without
retransmitting it. Servers rejecting REST with an unsupported-command response
fall back to a full transfer on the next attempt; this is logged. Completed local
raw files and retained download partials similarly avoid repeat downloads.

Each `.xz` has a sibling `<original-name>.sha512`, for example
`bee_2600.26_.sha512`. Its standard checksum line contains the SHA-512 of the
**uncompressed SBF**, two spaces, and the original filename without `.xz`.
Both files receive the configured output permissions. Existing verified archives
are streamed through decompression to produce missing hashes and sidecars without
recompression. This verifies preserved local content, not a receiver-supplied hash.

The archive is published first, then its checksum via a separate temporary-file
rename. A target is complete only after both publications succeed. These two
renames are not one atomic transaction; after interruption the checksum is repaired
on retry without resending an already published archive. A sidecar supports later
content verification; its presence or matching length alone does not validate
remote archive bytes. It is not used as proof that compressed prefixes match.

One push worker visits enabled targets in JSON order for each file; uploads to
different targets do not run concurrently or compete for the LTE link. A target
failure is logged with its name and the worker continues to the next target.
Only that file/target pair remains pending; successful targets keep their receipts.
An in-flight failed connection can still delay subsequent work until its configured
timeout. `--once` drains the remaining work and returns nonzero if any enabled target
fails or is deferred by backoff. Per-file download and per-target push failures
use persisted exponential backoff: `min(3600, retry_seconds * 2^(n-1))` seconds,
with the failure counter capped at 10. The actual attempt waits for a subsequent
scan. Download source changes and push archive/endpoint changes reset the relevant
retry identity. Password-only changes do not reset it. Compression and local
checksum failures are reconsidered on subsequent scans without this network backoff.
Storage admission failures are reconsidered every cycle after existing uploads.

Successful remote checks are cached separately per target until the next UTC day
(or a local archive or that destination's settings change). This also
rechecks remote archive SIZE for retained local archives daily and republishes
the checksum, even when the archive itself is skipped. Cached hashes are reused;
archives are not decompressed afresh every day. Passwords
can be changed without invalidating source metadata; restart to reload config.

## Storage limit and eviction

```json
{"storage": {"limit_gib": 8}}
```

Omitting `storage`, omitting `limit_gib`, or setting `limit_gib` to null keeps
unlimited retention. The `storage` section itself must be an object. A finite
limit must be a number from 8 to 1048576 GiB, inclusive (1 GiB = 1024^3 bytes);
fractional GiB values are accepted. The limit includes logical file lengths beneath
the archive root: archives, checksums, raw files, partials, state and unrelated
files. Sparse files count by logical length; directory entries and symlinks are
not included in this logical-byte total. Unrelated files count toward usage
but are never selected for deletion. Available filesystem space is checked
independently; external writers may still cause ENOSPC, which preserves pending
work and is reported as a failure.

Before downloading or resuming compression, reserve raw SIZE `R`, compressed workspace
`R + max(64 MiB, ceil(R / 100))`, and 4 KiB for the checksum. Keep a shared 256 MiB
margin for state updates and filesystem activity. Already retained job data is
counted once. Compressed output is copied from xz's pipe with a size check before
each write; exceeding its bound aborts compression while retaining raw. A raw file
whose complete workspace cannot fit is rejected before evicting any archive.
Thus 8 GiB supports the expected daily sizes, not arbitrary multi-GiB inputs.

If admission lacks room, remove oldest managed archives whose current content and
checksum were delivered to **all currently enabled targets**. With no enabled
targets, completed local archives may be evicted. Pending work, changed/unmanaged
archives, and active jobs are protected. Files waiting for a failed destination
can fill the budget: new downloads are deferred, not allowed to evict pending
uploads. Source revisions also wait for the older local version to be delivered
before replacement. Reducing the configured limit may leave protected data above
the new limit; cleanup reports this instead of deleting it. End-of-cycle cleanup
also evicts eligible archives when needed to restore the 256 MiB margin. With no
configured limit, the free-space admission guard still applies but does not
automatically evict archives.

Eviction removes the local `.xz` and its `.sha512`, then empty date directories;
it never removes remote files. A durable eviction intent permits recovery after
interruption. The source version remains in state as a tombstone so the next scan
does not redownload it merely because the receiver still retains it. A changed
SIZE/MDTM is a new version. Enabling a new target does not resurrect tombstones;
only retained local archives are backfilled.

Tombstones are collected after two successful complete receiver inventories show
the original file absent, with no intervening successful presence observation. Presence checks include the oldest excluded directories
and active `.A` names. Failed/incomplete inventories cannot advance GC. Entries
with local archives or unfinished local data remain. After GC, reappearance of the
same path is a new discovery.

## Resource bounds and failures

There is one download worker, one compression worker, and one push worker. The
stages can overlap for different files when storage is unlimited; only one xz
process runs at a time. A finite limit admits one full file pipeline at a time. The
compression queue holds at most two waiting files. When it fills, the downloader
waits before starting another download. Newly downloaded raw backlog is bounded
by the active compression, two queued files, and the current download; previously
retained recovery files may already occupy additional disk space. Queues contain
paths and metadata, not file contents. Per-file ownership passes between stages,
and atomic JSON state updates are serialized under a lock.

The main thread monitors download and upload independently and shuts down only
the stalled transfer's tracked sockets after
`stall_timeout_seconds` (300 by default) without progress. Socket timeouts enforce
the same idle bound; connection/control operations use
`connect_timeout_seconds` (30 by default). There is no ten-minute whole-file
limit and no overlapping retries for a given file. Progress is reported approximately
every minute and at the end of each data transfer, for example:

```text
INFO Upload [primary] bee_2590.26_.xz.ngo-mosaic-push.part: 66.75MiB transferred, avg 0.69 MiB/s, progress 4.7%
```

Transferred MiB and progress include any resumed prefix; progress uses the known
file size (an empty file reports 100%). Average
MiB/s uses only the bytes transferred since the previous progress report, divided
by the actual elapsed interval on a monotonic clock. The first report starts at
data-connection establishment, and the final report covers the remaining partial
interval. Each transfer starts its speed counters at its resume offset, so bytes
from an earlier attempt do not inflate the current speed.
Upload progress means bytes accepted by the client socket; completion additionally
requires the server's final FTP reply.

Control sockets use TCP keepalive even while the data connection is busy. The Linux
settings default to `tcp_keepalive_idle_seconds: 60`,
`tcp_keepalive_interval_seconds: 30`, and `tcp_keepalive_probes: 5`. This is TCP
probing, not concurrent FTP NOOP commands that could consume transfer replies.
The separate `completion_timeout_seconds` (default 300) bounds TLS data-channel
shutdown and then waiting for the FTP transfer-complete reply. During these phases
the byte-progress watchdog is suspended; socket timeouts and shutdown cancellation
still apply. Ordinary control commands return to `connect_timeout_seconds` after
the completion reply. INFO logs identify the initial remote archive SIZE check,
transfer progress, TLS shutdown and FTP completion reply. Temporary/final SIZE
checks and rename are performed but do not each emit a separate success message.
An equal-sized remote temporary file alone does not count as a successfully
published transfer.

Compression defaults to preset 6, two threads and a 512 MiB memory limit. xz may
reduce threads/dictionary settings to satisfy that limit. `XZ_OPT` and
`XZ_DEFAULTS` are ignored so JSON settings and the explicit CLI override own
compressor behavior. Output is streamed through disk and bounded buffers; the
whole SBF is never loaded in RAM.
Allow space for the current raw file, temporary compressed output and any previous
archive version. Network and write failures leave pending data for recovery;
space admission can remove completed, eligible archives only under the configured
storage policy described above.

## Local vsftpd integration checks

The opt-in harness uses real plain FTP and explicit FTPS with session reuse,
throwaway certificates and fixtures. Run it inside private Linux user/network
namespaces, using the repository uv environment and an installed or unpacked
vsftpd executable (plus `openssl`, `ip`, and `xz`):

```sh
unshare --user --map-root-user --net sh -c 'ip link set lo up; exec "$@"' sh \
  .venv/bin/python tests/mosaic_push_vsftpd.py --vsftpd /usr/bin/vsftpd --suite storage
unshare --user --map-root-user --net sh -c 'ip link set lo up; exec "$@"' sh \
  .venv/bin/python tests/mosaic_push_vsftpd.py --vsftpd /usr/bin/vsftpd --suite resume
```

No system service, receiver or production archive is contacted. Fixtures and server
processes are removed on exit. The storage suite uses 64 MiB zero-filled and
OS-random files with the real 8 GiB minimum and a 7.25 GiB sparse unmanaged filler
to exercise quota pressure without transferring gigabytes per daily file. It checks
observed usage, protected failed uploads, recovery, eviction/redownload prevention,
GC, physical free-space checks and oversized inputs. The resume suite injects
interruptions into real transfers, verifies REST and decoded remote SHA-512,
checksum-only retry, lost completion replies, restart, changed source partials,
state-write ENOSPC, compression bounds, local-only operation with 8/16 GiB and
unlimited retention, and same-size revisions.
