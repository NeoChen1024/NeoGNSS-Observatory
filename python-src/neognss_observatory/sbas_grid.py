# SPDX-License-Identifier: GPL-3.0-only
"""Conservative SBAS ionospheric state and time-weighted hourly statistics."""

from collections import Counter, defaultdict

L1_HZ = 1575.42e6
TECU_PER_M = L1_HZ**2 / (40.3 * 1e16)


def igp_coordinates():
    """Standard mask-bit order, cross-checked against pinned RTKLIB igpband1/2.

    Return (band, one-based mask bit) -> (latitude, longitude). These are
    sampling points, not the corners of interpolation cells.
    """
    points = {}
    for band in range(9):
        bit = 1
        for column in range(8):
            lon = -180 + band * 40 + column * 5
            latitudes = list(range(-55, 56, 5))
            if column % 2 == 0:
                latitudes = [-75, -65] + latitudes + [65, 75]
                if lon in (-180, -90, 0, 90):
                    latitudes += [85]
                if lon in (-140, -50, 40, 130):
                    latitudes = [-85] + latitudes
            for lat in latitudes:
                points[band, bit] = (lat, lon)
                bit += 1
    for band, sign in ((9, 1), (10, -1)):
        bit = 1
        for lat, step in ((60, 5), (65, 10), (70, 10), (75, 10), (85, 30)):
            start = -170 if band == 10 and lat == 85 else -180
            for lon in range(start, 180, step):
                points[band, bit] = (sign * lat, lon)
                bit += 1
    return points


COORDINATES = igp_coordinates()


class HourlyGrid:
    """One PRN/signal only. Reset at gaps; never average invalid values as zero.

    Missing/mismatching masks are not retroactively repaired using future data.
    On mask-version changes discard the previous generation conservatively.
    This exploratory policy is not an aviation SBAS integrity implementation.
    """

    def __init__(self, correction_age=600, mask_age=1200):
        self.correction_age, self.mask_age = correction_age, mask_age
        self.masks, self.cells = {}, {}
        self.iodi = None
        self.band_count = None
        self.last_time = None
        self.stats = defaultdict(lambda: [0.0, 0])  # TECU seconds, valid seconds
        self.diagnostics = Counter()

    def mask_deadline(self):
        if not self.masks or len(self.masks) != self.band_count:
            return -1
        return min(m[1] + self.mask_age for m in self.masks.values())

    def flush_cell(self, key, time):
        start, expiry, value = self.cells[key]
        end = min(time, expiry, self.mask_deadline())
        while start < end:
            hour = start // 3600 * 3600
            stop = min(end, hour + 3600)
            stats = self.stats[hour, *key]
            stats[0] += value * (stop - start)
            stats[1] += stop - start
            start = stop
        self.cells[key] = (time, expiry, value)

    def flush(self, time):
        for key in self.cells:
            self.flush_cell(key, time)

    def reset(self, time):
        self.flush(time)
        self.cells.clear()
        self.masks.clear()
        self.iodi = self.band_count = None
        self.last_time = None

    def process(self, time, message):
        if self.last_time is not None and time < self.last_time:
            raise ValueError("SBAS reception-context time reversed")
        self.last_time = time
        if message.get("status") != "decoded":
            self.diagnostics["unparsed_or_invalid"] += 1
            return
        kind = message["type"]
        content = message["content"]
        if kind == 0:
            self.reset(time)
            self.last_time = time
            self.diagnostics["test_mode_reset"] += 1
        elif kind == 18:
            self.flush(time)
            count, band, iodi = content["number_of_bands_raw"], content["band"], content["iodi"]
            positions = tuple(content["active_mask_positions"])
            if (
                not 1 <= count <= 11
                or len(set(positions)) != len(positions)
                or any((band, p) not in COORDINATES for p in positions)
            ):
                self.reset(time)
                self.diagnostics["invalid_mask"] += 1
                return
            if self.iodi != iodi or self.band_count != count:
                self.cells.clear()
                self.masks.clear()
                self.iodi, self.band_count = iodi, count
            elif self.mask_deadline() <= time:
                retained = {b: m for b, m in self.masks.items() if m[1] + self.mask_age > time}
                if len(retained) != len(self.masks):
                    self.cells.clear()
                self.masks = retained
            old = self.masks.get(band)
            if old and old[0] != positions:
                # Same IODI with a changed mask is not safe to combine.
                self.cells.clear()
                self.masks.clear()
                self.diagnostics["same_iodi_mask_changed"] += 1
            self.masks[band] = (positions, time)
        elif kind == 26:
            band = content["band"]
            if content["iodi"] != self.iodi or band not in self.masks or self.mask_deadline() <= time:
                self.diagnostics["correction_without_current_complete_mask"] += 1
                return
            positions = self.masks[band][0]
            for i, correction in enumerate(content["corrections"]):
                ordinal = content["block"] * 15 + i
                if ordinal >= len(positions):
                    continue  # Unused slots in the final block are padding.
                key = (band, positions[ordinal])
                if key in self.cells:
                    self.flush_cell(key, time)
                    del self.cells[key]
                if correction["status"] != "usable" or correction["delay_raw"] == 511 or correction["givei"] == 15:
                    self.diagnostics[correction["status"]] += 1
                    continue
                value = correction["delay_raw"] * 0.125 * TECU_PER_M
                self.cells[key] = (time, time + self.correction_age, value)

    def finish(self, time):
        if self.last_time is not None and time < self.last_time:
            raise ValueError("SBAS group end precedes its last message")
        self.flush(time)
        self.cells.clear()

    def rows(self):
        for (hour, band, bit), (integral, seconds) in sorted(self.stats.items()):
            if not 0 < seconds <= 3600:
                raise ValueError("Overlapping or invalid hourly grid coverage")
            lat, lon = COORDINATES[band, bit]
            yield dict(
                hour_utc=hour,
                band=band,
                mask_bit=bit,
                latitude=lat,
                longitude=lon,
                vtec_tecu=integral / seconds,
                valid_seconds=seconds,
                coverage=seconds / 3600,
            )
