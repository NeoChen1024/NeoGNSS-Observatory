# Mosaic SBF mirror and FTPS forwarding

`ngo-mosaic-push` is a single-receiver companion daemon for a host such as a
Raspberry Pi 5. It downloads closed daily SBF files over plain FTP on the receiver
network, verifies local xz compression, and forwards the archives to one FTPS
server when forwarding is enabled. It runs as an unprivileged account and never writes to or deletes from the
receiver. No native GNSS processing or SQLite database is involved.

## Configuration and invocation

Install the project into its repository-root uv environment as described in the
[README](../README.md). Install the system `xz` executable as well. All operational
settings and credentials live in one JSON file; `.netrc` is not used.
The companion itself uses only the Python standard library, Click and Rich; it
does not load the native GNSS extension. A dedicated receiver-host deployment can
use a uv-managed environment with those two dependencies and the package's
`mosaic_push`, `mosaic_push_config`, `mosaic_push_ftp`, and `download_common` modules.

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

`--check-config` does not connect or create files. `--once` performs a real single
scan, local recovery and forwarding cycle; it retains date eligibility rules and
returns nonzero on transfer/processing failures. Missing-date warnings alone do
not fail the cycle. Without either option the command runs continuously. A lock
on the archive root prevents overlapping instances.
`--xz-threads N` overrides JSON `compression.threads` for this invocation (1 to 64),
in both daemon and oneshot modes. It controls xz's internal threads, not the number
of files compressed concurrently. Without it the JSON value is used.

For download/compression only, set `"destination": {"enabled": false}`. Omitting
`destination` or setting it to `null` has the same effect. No destination credentials
are needed and no FTPS connection is attempted. An existing full destination object
may also be retained with `enabled: false`. Set `enabled: true` with complete
connection settings and restart to forward previously archived files. Older configs
with a destination object but no `enabled` field continue to enable forwarding.
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
SIGTERM/SIGINT stop work, close tracked network sockets, and terminate xz.

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
an in-flight cycle. First startup follows the same policy and mirrors all eligible
history. Three independent workers handle downloading, compression and forwarding.
Each completed download is queued for compression immediately, and each verified
archive is queued for forwarding without waiting for the remaining downloads.
`--once` waits for all three stages to drain before returning its exit status.
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
```

Source `SIZE` and `MDTM` are required. Source size and timestamp are compared with
saved metadata, not with compressed size. A difference causes a fresh download
and compression. Downloaded bytes must match the advertised size and the source
must remain unchanged and closed. xz output is decompressed and its SHA-256 is
compared with the downloaded SBF before publication. This validates lossless
storage, not the scientific content or completeness of SBF observations.

A small atomic JSON state file in `.mosaic-push/state.json` records source
metadata, local completion and destination checks. It contains no passwords.
Private temporary SBF copies live under `.mosaic-push/raw/` and are deleted only
after verified xz publication and a saved completion record. Failed compression
retains a complete raw copy for retry. Partial downloads are restarted, not resumed.
Keep the state with the managed archives. An existing `.xz` without state is adopted
only after a fresh receiver download and a streaming comparison of its decompressed
SHA-256 with the downloaded SBF. A matching archive is retained byte-for-byte, with
its original mtime. A mismatch leaves both the existing archive and private raw
download intact and reports an error. Pre-existing uncompressed SBF files outside
the tool's private raw directory are never removed or used as download scratch space.
Source configuration is
bound to the archive state to prevent accidentally mixing receivers.

Final `.xz` files are retained indefinitely; there is no automatic archive cleanup.
Users may remove old completed archives. If the receiver still has an eligible
copy, the mirror will download it again. Files outside the receiver's eligible
range are not reconstructed after manual cleanup. Pending work without a local
archive is warned about; source and remote files are never deleted as a cleanup
policy. Existing unrelated local archives are not overwritten.

## FTPS publication

`destination.tls` is `explicit` (AUTH TLS, usually port 21) or `implicit` (TLS from
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

Remote files are compared by compressed **size only**, never mtime. An equal-size
file is skipped, including an equal-size file with different content. A missing or
different-size file is uploaded to `<filename>.ngo-mosaic-push.part`, checked by
SIZE, renamed to the final name, and checked again. The destination must support
SIZE, directory creation, and RNFR/RNTO replacement of an existing final file.
No delete-before-rename fallback is used. A failed temporary upload can remain on
the server; the next attempt overwrites it. Only one writer should manage these
remote names.

Successful remote checks are cached until the next UTC day (or a local archive or
destination change); failed uploads are retried on the next cycle. This also
rechecks retained local archives daily if remote files have disappeared. Passwords
can be changed without invalidating source metadata; restart to reload config.

## Resource bounds and failures

There is one download worker, one compression worker, and one push worker. The
stages can overlap for different files; only one xz process runs at a time. The
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
every minute and at the end of each data transfer, including bytes and average
MiB/s since that transfer's data connection began (using a monotonic clock).
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
the completion reply. Logs identify TLS shutdown, completion reply, temporary SIZE,
rename, and final SIZE phases separately. An equal-sized remote temporary file alone
does not count as a successfully published transfer.

Compression defaults to preset 6, two threads and a 512 MiB memory limit. xz may
reduce threads/dictionary settings to satisfy that limit. `XZ_OPT` and
`XZ_DEFAULTS` are ignored so JSON settings and the explicit CLI override own compressor behavior. Output is
streamed through disk and bounded buffers; the whole SBF is never loaded in RAM.
Allow space for the current raw file, temporary compressed output and any previous
archive version. Disk-full or network failures leave completed archives intact and
are retried; no automatic deletion is used to make room.
