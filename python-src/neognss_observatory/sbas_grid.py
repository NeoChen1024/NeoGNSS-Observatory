# SPDX-License-Identifier: GPL-3.0-only
"""Conservative SBAS ionospheric state and time-weighted hourly statistics."""

from collections import Counter, defaultdict

L1_HZ = 1575.42e6
TECU_PER_M = L1_HZ**2 / (40.3 * 1e16)


from ._native import igp_coordinates

COORDINATES = igp_coordinates()


class HourlyGrid:
    """Hourly reduction of batches emitted by the native interval state machine."""

    def __init__(self, correction_age=600, mask_age=1200):
        from ._native import GridProcessor

        self.processor = GridProcessor(correction_age, mask_age)
        self.pending = []
        self.stats = defaultdict(lambda: [0.0, 0.0])

    def integrate(self, intervals):
        for row in intervals:
            start, end = row["start_gpst_ms"], row["end_gpst_ms"]
            while start < end:
                hour = start // 3600000 * 3600
                stop = min(end, (hour + 3600) * 1000)
                stats = self.stats[hour, row["band"], row["mask_bit"]]
                seconds = (stop - start) / 1000
                stats[0] += row["vtec_tecu"] * seconds
                stats[1] += seconds
                start = stop

    def process(self, time, message):
        self.pending.append(dict(gpst_ms=round(time * 1000), sbas=message, offset=0))
        if len(self.pending) >= 4096:
            self.integrate(self.processor.process(self.pending))
            self.pending.clear()

    def finish(self, time):
        self.integrate(self.processor.process(self.pending))
        self.pending.clear()
        self.integrate(self.processor.finish(round(time * 1000)))

    @property
    def diagnostics(self):
        return Counter(self.processor.diagnostics)

    def rows(self):
        for (hour, band, bit), (integral, seconds) in sorted(self.stats.items()):
            if not 0 < seconds <= 3600:
                raise ValueError("Overlapping or invalid hourly grid coverage")
            lat, lon = COORDINATES[band, bit]
            yield dict(
                hour_gpst=hour,
                band=band,
                mask_bit=bit,
                latitude=lat,
                longitude=lon,
                vtec_tecu=integral / seconds,
                valid_seconds=seconds,
                coverage=seconds / 3600,
            )
