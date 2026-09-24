# SPDX-License-Identifier: GPL-3.0-only
"""PySide6 viewer for ngo-stec-realtime JSONL; no GNSS processing."""

import queue
import sys
import time
from datetime import datetime, timedelta
from pathlib import Path

import click
import numpy as np
from matplotlib.backends.backend_qtagg import FigureCanvasQTAgg, NavigationToolbar2QT
from matplotlib.cm import ScalarMappable
from matplotlib.collections import LineCollection
from matplotlib.colors import Normalize
from matplotlib.figure import Figure
from PySide6 import QtCore, QtWidgets

from .map_assets import coastline_parts, with_coastline
from .stec_view_data import JsonlReader, Window


class Viewer(QtWidgets.QMainWindow):
    def __init__(self, coastline, *, extent=None, tec_limit=50.0, max_samples=1_000_000):
        super().__init__()
        self.setWindowTitle("NeoGNSS — Realtime relative STEC")
        self.resize(1400, 950)
        self.model = Window(max_samples)
        self.reader = None
        self.last_received = None
        self.dirty = True
        self.coast_dirty = True
        self.auto_fit = extent is None
        self.coast = [np.asarray(p, dtype=float) for p in coastline_parts(coastline)]
        self.annotations = []
        self.series = {}
        self.pick_satellites = []
        self.last_error = None
        self.figure = Figure(figsize=(11, 8), layout="constrained")
        self.canvas = FigureCanvasQTAgg(self.figure)
        self.map_ax, self.time_ax = self.figure.subplots(2, 1, gridspec_kw={"height_ratios": [3, 1]})
        self.map_ax.set(title="Ionospheric pierce points — last hour", xlabel="Longitude (°E)", ylabel="Latitude (°N)")
        self.map_ax.grid(alpha=0.2)
        self.coast_artist = LineCollection([], colors="black", linewidths=0.45, zorder=1)
        self.map_ax.add_collection(self.coast_artist)
        self.norm = Normalize(-tec_limit, tec_limit)
        self.trails = LineCollection([], cmap="coolwarm", norm=self.norm, linewidths=1.4, zorder=2)
        self.map_ax.add_collection(self.trails)
        self.points = self.map_ax.scatter([], [], c=[], cmap="coolwarm", norm=self.norm, s=30, zorder=3, picker=6)
        self.colorbar = self.figure.colorbar(ScalarMappable(norm=self.norm, cmap="coolwarm"), ax=self.map_ax, pad=0.015)
        self.colorbar.set_label("Relative STEC (TECU)")
        self.time_ax.set(
            xlim=(-60, 0), ylim=(-tec_limit, tec_limit), xlabel="Minutes before latest GPST", ylabel="Relative STEC (TECU)"
        )
        self.time_ax.grid(alpha=0.2)
        self.time_ax.set_title("Select a satellite or click its latest IPP")
        self.canvas.mpl_connect("pick_event", self.pick)
        self.map_ax.callbacks.connect("xlim_changed", self.map_changed)
        self.map_ax.callbacks.connect("ylim_changed", self.map_changed)

        root = QtWidgets.QWidget()
        self.setCentralWidget(root)
        layout = QtWidgets.QHBoxLayout(root)
        plots = QtWidgets.QVBoxLayout()
        plots.addWidget(NavigationToolbar2QT(self.canvas, self))
        plots.addWidget(self.canvas, 1)
        layout.addLayout(plots, 1)
        controls = QtWidgets.QVBoxLayout()
        panel = QtWidgets.QWidget()
        panel.setLayout(controls)
        panel.setFixedWidth(220)
        layout.addWidget(panel)
        title = QtWidgets.QLabel("DISPLAY")
        title.setStyleSheet("font-weight: bold; font-size: 16px")
        controls.addWidget(title)
        self.systems = {}
        for key, label in [("G", "GPS"), ("J", "QZSS"), ("E", "Galileo"), ("C", "BeiDou")]:
            box = QtWidgets.QCheckBox(label)
            box.setChecked(True)
            box.toggled.connect(self.changed)
            controls.addWidget(box)
            self.systems[key] = box
        controls.addWidget(QtWidgets.QLabel("Satellite"))
        self.satellite = QtWidgets.QComboBox()
        self.satellite.addItem("All")
        self.satellite.currentTextChanged.connect(self.changed)
        controls.addWidget(self.satellite)
        controls.addWidget(QtWidgets.QLabel("Signal pair"))
        self.pair = QtWidgets.QComboBox()
        self.pair.addItem("All")
        self.pair.currentTextChanged.connect(self.changed)
        controls.addWidget(self.pair)
        controls.addWidget(QtWidgets.QLabel("Symmetric color range (± TECU)"))
        self.limit = QtWidgets.QDoubleSpinBox()
        self.limit.setRange(0.1, 10000.0)
        self.limit.setDecimals(1)
        self.limit.setValue(tec_limit)
        self.limit.valueChanged.connect(self.changed)
        controls.addWidget(self.limit)
        fit = QtWidgets.QPushButton("Fit tracks")
        fit.clicked.connect(self.fit_tracks)
        controls.addWidget(fit)
        self.pause = QtWidgets.QPushButton("Pause display")
        self.pause.setCheckable(True)
        self.pause.toggled.connect(self.pause_changed)
        controls.addWidget(self.pause)
        note = QtWidgets.QLabel(
            "Last 3,600 GPST seconds only.\n\nIPP on a 6,821 km shell.\nEach pair / arc has its own zero.\nNo absolute TEC calibration.\n\nPause does not stop input."
        )
        note.setWordWrap(True)
        controls.addWidget(note)
        self.details = QtWidgets.QLabel("Waiting for JSONL…")
        self.details.setWordWrap(True)
        controls.addWidget(self.details)
        controls.addStretch()
        self.set_extent(extent or (-180, 180, -80, 80))
        self.timer = QtCore.QTimer(self)
        self.timer.timeout.connect(self.tick)
        self.timer.start(500)

    def attach(self, source):
        self.reader = JsonlReader(source)
        self.reader.start()

    def changed(self, *_):
        self.dirty = True

    def pause_changed(self, paused):
        self.pause.setText("Resume display" if paused else "Pause display")
        self.changed()

    def map_changed(self, *_):
        self.coast_dirty = True
        self.changed()

    def set_extent(self, extent):
        west, east, south, north = extent
        self.map_ax.set_xlim(west, east)
        self.map_ax.set_ylim(south, north)
        self.map_ax.set_aspect("auto")

    def fit_tracks(self):
        data = self.model.array()
        if not len(data):
            return
        values = np.unique(data["longitude"] % 360)
        gap = np.argmax(np.diff(np.r_[values, values[0] + 360]))
        origin = values[(gap + 1) % len(values)]
        lons = (data["longitude"] - origin) % 360 + origin
        center = (lons.min() + lons.max()) / 2
        if center > 180:
            lons -= 360
        south, north = max(-89, data["latitude"].min() - 6), min(89, data["latitude"].max() + 6)
        box = self.map_ax.get_window_extent()
        ratio = box.width / max(1, box.height) / max(0.2, np.cos(np.radians((south + north) / 2)))
        span = min(360, max(lons.max() - lons.min() + 16, (north - south) * ratio))
        center = (lons.min() + lons.max()) / 2
        self.set_extent((center - span / 2, center + span / 2, south, north))
        self.auto_fit = False

    @staticmethod
    def choices(widget, values):
        entries = ["All"] + sorted(set(values))
        if entries == [widget.itemText(i) for i in range(widget.count())]:
            return
        selected = widget.currentText()
        with QtCore.QSignalBlocker(widget):
            widget.clear()
            widget.addItems(entries)
            widget.setCurrentText(selected if selected in entries else "All")

    def pick(self, event):
        if event.artist is self.points and len(event.ind):
            self.satellite.setCurrentText(self.pick_satellites[event.ind[0]])

    def tick(self):
        if self.reader:
            deadline = time.monotonic() + 0.05
            for _ in range(2048):
                try:
                    epoch = self.reader.queue.get(timeout=0.002)
                except queue.Empty:
                    break
                self.model.append(epoch)
                self.last_received = time.monotonic()
                self.dirty = True
                if time.monotonic() >= deadline:
                    break
            if self.reader.error and self.reader.error != self.last_error:
                self.last_error = self.reader.error
                click.echo(self.last_error, err=True)
        state = "Waiting for input"
        if self.last_received is not None:
            state = "LIVE" if time.monotonic() - self.last_received < 10 else "STALE — no recent input"
        if self.reader and self.reader.done and self.reader.queue.empty():
            state = self.reader.error or "EOF — last window retained"
        if self.pause.isChecked():
            state = "DISPLAY PAUSED | " + state
        if self.model.latest is not None:
            date = datetime(1980, 1, 6) + timedelta(seconds=self.model.latest // 10**9)
            state += f" | {date:%Y-%m-%d %H:%M:%S} GPST | {self.model.count:,} samples"
        if self.model.trimmed:
            state += " | capacity limit: window shortened"
        if self.model.reset_count:
            state += f" | source/time resets: {self.model.reset_count}"
        self.statusBar().showMessage(state)
        self.details.setText(self.model.setup or "Waiting for JSONL…")
        if self.dirty and not self.pause.isChecked():
            self.render()
            self.dirty = False

    def render(self):
        if self.auto_fit and self.model.count:
            self.fit_tracks()
        west, east = self.map_ax.get_xlim()
        south, north = self.map_ax.get_ylim()
        center = (west + east) / 2
        if self.coast_dirty:
            coast = []
            for p in self.coast:
                x = (p[:, 0] - center + 180) % 360 - 180 + center
                if x.max() < west or x.min() > east or p[:, 1].max() < south or p[:, 1].min() > north:
                    continue
                v = np.column_stack([x, p[:, 1]])
                coast.extend(vv for vv in np.split(v, np.flatnonzero(np.abs(np.diff(x)) > 180) + 1) if len(vv) > 1)
            self.coast_artist.set_segments(coast)
            self.coast_dirty = False
        data = self.model.array()
        self.choices(self.satellite, data["satellite"])
        self.choices(self.pair, data["pair"])
        selected_systems = [s for s, box in self.systems.items() if box.isChecked()]
        mask = np.isin(data["satellite"].astype("U1"), selected_systems)
        if self.satellite.currentText() != "All":
            mask &= data["satellite"] == self.satellite.currentText()
        if self.pair.currentText() != "All":
            mask &= data["pair"] == self.pair.currentText()
        data = data[mask]
        if len(data):
            data = data[np.lexsort((data["time"], data["arc"], data["segment"], data["pair"], data["satellite"]))]
        keys = data[["satellite", "pair", "segment", "arc"]]
        cuts = np.flatnonzero(keys[1:] != keys[:-1]) + 1
        segments, colors, latest, active = [], [], {}, set()
        pair_colors = {p: f"C{i % 10}" for i, p in enumerate(sorted(set(data["pair"])))}
        for track in np.split(data, cuts):
            if not len(track):
                continue
            key = tuple(track[n][0].item() for n in ("satellite", "pair", "segment", "arc"))
            stride = max(1, int(np.ceil(len(track) / 2000)))
            track = track[np.unique(np.r_[np.arange(0, len(track), stride), len(track) - 1])]
            x = (track["longitude"] - center + 180) % 360 - 180 + center
            points = np.column_stack([x, track["latitude"]])
            if len(track) > 1:
                valid = np.abs(np.diff(x)) < 180
                segments.extend(np.stack([points[:-1], points[1:]], axis=1)[valid])
                colors.extend(((track["tec"][:-1] + track["tec"][1:]) / 2)[valid])
            tail = track[-1]
            identity = (key[0], key[1])
            if identity not in latest or tail["time"] >= latest[identity]["time"]:
                latest[identity] = tail
            if self.satellite.currentText() != "All":
                if key not in self.series:
                    (self.series[key],) = self.time_ax.plot([], [], linewidth=1, label=key[1])
                self.series[key].set_data((track["time"] - self.model.latest) / 60e9, track["tec"])
                self.series[key].set_color(pair_colors[key[1]])
                active.add(key)
        for key in list(self.series):
            if key not in active:
                self.series.pop(key).remove()
        self.time_ax.set_title(
            "Select a satellite or click its latest IPP"
            if self.satellite.currentText() == "All"
            else self.satellite.currentText() + " — independent pair / arc traces"
        )
        handles = {key[1]: line for key, line in self.series.items()}
        if handles:
            self.time_ax.legend(handles.values(), handles.keys(), loc="upper left", fontsize=8)
        elif self.time_ax.get_legend() is not None:
            self.time_ax.get_legend().remove()
        self.norm.vmin, self.norm.vmax = -self.limit.value(), self.limit.value()
        self.colorbar.update_normal(ScalarMappable(norm=self.norm, cmap="coolwarm"))
        self.time_ax.set_ylim(-self.limit.value(), self.limit.value())
        self.trails.set_segments(segments)
        self.trails.set_array(np.asarray(colors))
        tails = list(latest.values())
        self.pick_satellites = [r["satellite"] for r in tails]
        self.points.set_offsets(
            np.array([[(r["longitude"] - center + 180) % 360 - 180 + center, r["latitude"]] for r in tails]).reshape(-1, 2)
        )
        self.points.set_array(np.array([r["tec"] for r in tails]))
        for annotation in self.annotations:
            annotation.remove()
        self.annotations.clear()
        seen = set()
        for r, point in zip(tails, self.points.get_offsets()):
            if r["satellite"] in seen:
                continue
            seen.add(r["satellite"])
            self.annotations.append(
                self.map_ax.annotate(r["satellite"], point, xytext=(4, 4), textcoords="offset points", fontsize=8, clip_on=True)
            )
        self.canvas.draw_idle()

    def closeEvent(self, event):
        self.timer.stop()
        if self.reader:
            self.reader.stop.set()
            self.reader.join(timeout=0.25)
        event.accept()


@click.command()
@click.option(
    "--input", "input_path", type=click.Path(exists=True, dir_okay=False, path_type=Path), help="Read JSONL file instead of stdin."
)
@click.option(
    "--extent", nargs=4, type=float, metavar="WEST EAST SOUTH NORTH", help="Initial map bounds; default fit received IPPs."
)
@click.option("--tec-limit", type=click.FloatRange(min=0.1, max=10000), default=50.0, show_default=True)
@click.option(
    "--max-samples",
    type=click.IntRange(min=4096),
    default=1_000_000,
    show_default=True,
    help="Additional RAM safety cap; oldest epochs are removed.",
)
@with_coastline
def cli(input_path, extent, tec_limit, max_samples, coastline):
    """Display the last GPST hour of ngo-stec-realtime JSONL using PySide6."""
    if extent and not (extent[0] < extent[1] <= extent[0] + 360 and -90 <= extent[2] < extent[3] <= 90):
        raise click.BadParameter("Expected WEST < EAST (span <= 360), -90 <= SOUTH < NORTH <= 90", param_hint="--extent")
    if input_path is None and sys.stdin.isatty():
        raise click.UsageError("Pipe ngo-stec-realtime JSONL to stdin or specify --input")
    app = QtWidgets.QApplication.instance() or QtWidgets.QApplication([sys.argv[0]])
    window = Viewer(coastline, extent=extent, tec_limit=tec_limit, max_samples=max_samples)
    try:
        source = input_path.open("rb") if input_path else sys.stdin.buffer
    except OSError as error:
        raise click.ClickException(str(error)) from error
    window.attach(source)
    window.show()
    app.exec()


if __name__ == "__main__":
    cli()
