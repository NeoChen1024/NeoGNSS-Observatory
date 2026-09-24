# Realtime STEC viewer

`ngo-stec-realtime-view` displays the JSONL output of
[`ngo-stec-realtime`](stec-realtime.md) in a PySide6 desktop window. It performs
no GNSS decoding or STEC calculation and needs no network map service.

```sh
ngo-stec-realtime -p sbf --host RECEIVER_IPV6 --port 2006 \
  --setup /data/station/setup.json | ngo-stec-realtime-view

ngo-stec-realtime-view --input realtime.jsonl
```

PySide6 is installed with the project's Python dependencies. A desktop display
is required for interactive use. File input is consumed as quickly as the viewer
can process it, not replayed at acquisition speed. At EOF the last window stays
visible. Input errors appear in the status bar and on stderr.

## Display and controls

The map uses the bundled Natural Earth 10m coastline, with colored ionospheric
pierce-point trails and the latest available point for each satellite/signal pair.
These are IPPs, not satellite subpoints. Color represents arc-relative STEC,
not absolute STEC or VTEC; different arcs and pairs have independent zeros.
Tracks never connect across processor segments, arcs or signal pairs.

- Filter by constellation, satellite and exact signal pair.
- Select a satellite from the menu or click its latest map point to show its
  relative-STEC time series below the map.
- Use the Matplotlib toolbar to pan, zoom or save the figure. **Fit tracks**
  restores a viewport covering the retained data.
- Adjust the symmetric color range (default ±50 TECU), or set `--tec-limit`.
- Use `--extent WEST EAST SOUTH NORTH` for explicit initial map bounds.
- **Pause display** freezes rendering, not acquisition or data expiry. Resuming
  shows the current retained window.

## Retention and input limits

The viewer retains the latest 3,600 GPST seconds relative to the newest input
epoch, including empty epochs. It does not expire historical replay using the
host clock. EOF retains the final window; live input inactivity is marked stale
after ten seconds. A setup change or backward epoch timestamp clears the window.

An additional `--max-samples` limit defaults to 1,000,000 samples; at most 108,000
epochs are retained. Reaching either bound removes oldest whole epochs and
displays a capacity warning. Long tracks are thinned for drawing only; this does
not change the retained measurements. The viewer keeps no full-session history.

Input lines are limited to 1 MiB and 4,096 samples per epoch. Acquisition is
bounded; if the display cannot consume queued epochs for five seconds, input
stops with an error rather than silently dropping samples or growing memory.
Closing the window stops its reader; a piped producer may then receive a broken
pipe. Scientific JSONL remains the producer's responsibility.
