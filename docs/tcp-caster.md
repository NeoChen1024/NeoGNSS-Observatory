# TCP caster

`ngo-tcp-caster` broadcasts one POSIX serial port or one TCP upstream to multiple
receive-only TCP clients. It forwards unchanged bytes without GNSS parsing,
framing, protocol conversion, timestamps or replay. TCP read/write boundaries
are not message boundaries; a new subscriber may start inside a receiver message.
The core supports generic Linux/UNIX systems with POSIX serial descriptors and
asyncio fd readiness; systemd is optional and Linux-specific.

## Usage

Use the repository-root uv environment and the normal project installation:

```sh
uv pip install --python .venv/bin/python -e .
.venv/bin/ngo-tcp-caster --config deploy/ngo-tcp-caster-tcp.json
.venv/bin/ngo-tcp-caster --config deploy/ngo-tcp-caster-serial.json
```

Run `uv venv .venv` first only if the root environment does not exist. Copy the
appropriate example and adjust its source and listening addresses. Diagnostics
use stderr; the TCP connections contain source bytes only. `--log-level DEBUG`
also reports rejected subscriptions. No source bytes or discarded GUI commands
are logged.

The examples listen on loopback. Set `server.host` to the desired interface
address for remote subscribers (`0.0.0.0` for all IPv4 interfaces, `::` for IPv6;
dual-stack behavior depends on the OS). This is a raw TCP service without
authentication, TLS or RFC 2217 control negotiation.

## Connection and buffer contract

- Each subscriber has an independent sender and an application pending-data
  limit of 1 MiB by default. If accepting the next chunk would exceed the limit,
  that subscriber is immediately disconnected; the remaining subscribers and
  source acquisition continue. No partial chunk is queued and no bytes are
  silently skipped within a surviving connection.
- The limit counts queued bytes, excluding at most one 16 KiB chunk being sent.
  Transport buffering has a 64 KiB high watermark and a 16 KiB low watermark;
  each sender waits for its own transport before sending the next chunk. The
  socket send buffer is requested as 64 KiB, with OS-dependent adjustment and
  accounting. TCP receive buffers and object overhead are additional resources.
  The application limit is not a total process-memory limit.
- At 18 KiB/s, 1 MiB provides about 57 seconds of application backlog capacity.
  This is not a precise disconnect deadline or acknowledgement that a client's
  application consumed the data.
- The default maximum is 32 simultaneous subscribers. Excess connections are
  immediately closed. Clients must retry themselves after rejection/disconnect.
- Bytes sent by subscribers are read in bounded chunks and discarded. GUI tools
  may send queries without losing their subscription, but these queries cannot
  change receiver settings or produce a requested response through the caster.
  EOF, including a client write-half-close, closes that subscription.
- The source remains open and is drained even with no subscribers. New clients
  receive only subsequent source data, without a banner or historical replay.
- Source EOF, error or enabled idle timeout disconnects all subscribers and
  clears their pending data. A reconnected source is a new stream; pre- and
  post-reconnection data are never joined in a surviving TCP subscription.
  While the source is unavailable, the listener remains open but immediately
  closes new subscriptions. Connection acceptance means a source connection is
  open, not that valid GNSS messages have been received.
- SIGINT/SIGTERM stop retries and abort outstanding subscriptions. Slow-client
  eviction and shutdown do not wait for blocked socket buffers to flush.

## JSON configuration

Unknown keys, incompatible source options and invalid values are rejected before
opening the source or listener. `source` is required; `server` may be omitted to
use its defaults. One source is selected per invocation.

| Server field | Default | Meaning |
|---|---|---|
| `host` | `127.0.0.1` | Listening address |
| `port` | `3002` | TCP port, 1–65535 |
| `max_clients` | `32` | Positive integer subscription limit |
| `client_buffer_bytes` | `1048576` | Positive integer per-client pending byte limit |

Both source types accept `idle_timeout_seconds` and `reconnect`:

| Common source field | Default | Meaning |
|---|---|---|
| `idle_timeout_seconds` | `null` | Positive seconds since the last nonempty read; `null` disables it |
| `reconnect.initial_delay_seconds` | `1` | Initial retry delay in seconds |
| `reconnect.max_delay_seconds` | `30` | Maximum base retry delay; at least the initial delay |
| `reconnect.stable_connection_seconds` | `60` | Connection uptime needed to reset retry backoff |

Initial source failures and later disconnects retry indefinitely. Retry base
delay doubles to its maximum, with each actual delay randomized to 80–100% of
that base. A sufficiently stable connection resets the base when it fails.
Idle timing uses a monotonic clock, starts when the source opens, and resets
only on received bytes. The TCP example enables a **10-second** idle timeout;
the serial example disables it. Disable or increase it for legitimately silent
sources. An idle timeout closes/reopens a serial port as well as a TCP source.

For `source.type: "tcp"`:

| Field | Default | Meaning |
|---|---|---|
| `host` | Required | Upstream hostname or unbracketed IP address |
| `port` | Required | Upstream TCP port, 1–65535 |
| `connect_timeout_seconds` | `10` | Positive timeout for establishing the connection |

The upstream is receive-only: the caster sends no subscription command, query or
keepalive payload. OS TCP keepalive is enabled with platform defaults; it is
independent of the configurable data-idle timeout.

For `source.type: "serial"`:

| Field | Default | Meaning |
|---|---|---|
| `device` | Required | Serial device path |
| `baudrate` | `115200` | Positive integer baud rate |
| `bytesize` | `8` | 5, 6, 7 or 8 data bits |
| `parity` | `N` | `N`, `E`, `O`, `M` or `S` |
| `stopbits` | `1` | 1, 1.5 or 2 stop bits |
| `rtscts`, `xonxoff`, `dsrdtr` | `false` | Hardware/software flow-control settings |
| `dtr`, `rts` | `true` | Requested initial control-line states |

The device is opened for exclusive access. No subscriber data is written to it.
Serial framing, flow control and control lines still apply; opening/reopening can
reset some devices, and OS/drivers may briefly toggle RTS/DTR during open.
Configure these fields for the receiver. Use a persistent device path when the
platform provides one; no Linux-specific path layout is required. Missing or
unplugged devices retry using the same reconnect policy. There is no separate
serial-open timeout; device opening uses the platform's local serial driver.
At conventional 8N1 framing, 115200 baud carries at most about 11.25 KiB/s;
choose a higher baud rate if an actual UART must sustain 18 KiB/s. USB CDC devices
may interpret the configured baud rate differently.

## systemd example

[deploy/ngo-tcp-caster.service](../deploy/ngo-tcp-caster.service) runs the installed
CLI in a repository-root uv environment. It does not install or start itself.

1. Install the project under the intended service path, such as
   `/opt/NeoGNSS-Observatory`, and adjust `ExecStart` if needed.
2. Create the dedicated `tcp-caster` user/group. For serial sources, grant that
   user access to the device using the distribution's actual device group or
   device-access rule. `dialout` in the unit comment is only an example.
3. Copy one JSON example to `/etc/ngo-tcp-caster.json` and configure the addresses
   or serial device. Keep the config readable by the service user.
4. Copy the unit to `/etc/systemd/system/ngo-tcp-caster.service`, then run:

```sh
sudo systemctl daemon-reload
sudo systemctl enable --now ngo-tcp-caster.service
journalctl -u ngo-tcp-caster.service -f
```

The unit's `ProtectHome=true` requires an installation outside protected home
directories. Do not enable `PrivateDevices` for a serial source. Source failures
are retried inside the process; `Restart=on-failure` handles process failures.
The caster does not write output files or alter receiver configuration through
subscriber commands.
