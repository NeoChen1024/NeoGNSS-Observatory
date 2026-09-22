# SPDX-License-Identifier: GPL-3.0-only
"""Multi-GNSS exact-code bias coverage and explicit CODE GIM datum transfer."""

import datetime as dt
import math
import re
from collections import defaultdict, deque
from pathlib import Path

from .antenna import FREQUENCIES_HZ
from .gpst import EPOCH, calendar
from .ppp_products import LocalProducts, bias_time
from .stec_pairs import meters_per_tecu


def ionex_header(path):
    """Validate the selected CODE family and read its C1W-C2W satellite DCBs.

    IONEX UT epochs stay explicitly tagged until native UTC->GPST conversion.
    No filename-derived scientific time coverage.
    """
    values, dcbs, first, last, end, references = {}, {}, None, None, False, {}
    with open(path, encoding="latin-1") as stream:
        for line in stream:
            label = line[60:].strip()
            if label in ("EPOCH OF FIRST MAP", "EPOCH OF LAST MAP"):
                date = dt.datetime(*map(int, line[:36].split()))
                if label == "EPOCH OF FIRST MAP":
                    first = date
                else:
                    last = date
            if label in ("INTERVAL", "BASE RADIUS", "# OF MAPS IN FILE"):
                values[label] = float(line[:10])
            if label == "HGT1 / HGT2 / DHGT":
                values[label] = tuple(map(float, line[:60].split()))
            for name, system, pair in (("GPS", "G", "C1W-C2W"), ("GALILEO", "E", "C1X-C5X")):
                if re.match(r"Reference observables for " + name + r"\s*:", line.strip()):
                    if pair not in line:
                        raise ValueError(f"Unsupported CODE {system} IONEX reference")
                    references[system] = pair
            if label == "PRN / BIAS / RMS" and line[3:4] in ("G", "E"):
                satellite, bias, rms = line[3:6], float(line[6:16]), float(line[16:26])
                if satellite in dcbs or not math.isfinite(bias) or not math.isfinite(rms) or rms < 0:
                    raise ValueError("Invalid or duplicate IONEX satellite DCB")
                dcbs[satellite] = bias * 299792458e-9
            if label == "END OF HEADER":
                end = True
                break
    if not end or "G" not in references or first is None or last != first + dt.timedelta(days=1) or not dcbs:
        raise ValueError(f"Incomplete CODE C1W-C2W IONEX header: {path}")
    if values != {"INTERVAL": 3600, "BASE RADIUS": 6371, "# OF MAPS IN FILE": 25, "HGT1 / HGT2 / DHGT": (450, 450, 0)}:
        raise ValueError(f"Unsupported CODE IONEX grid definition: {values}")
    return first, last, {sat: value for sat, value in dcbs.items() if sat[0] in references}


def signal_biases(path):
    """Read bounded GPST satellite code OSB/DSB records; never rename codes."""
    rows = defaultdict(list)
    time_system = mode = None
    end = False
    with open(path, encoding="latin-1") as stream:
        for line in stream:
            if line.startswith(" TIME_SYSTEM"):
                time_system = line.split()[-1]
            if line.startswith(" BIAS_MODE"):
                mode = line.split()[-1]
            if line.startswith("-BIAS/SOLUTION"):
                end = True
            kind = line[1:4]
            if kind not in ("OSB", "DSB") or line[11:12] not in ("G", "E", "C", "J") or line[15:24].strip():
                continue
            first, second = line[25:29].strip(), line[30:34].strip()
            if not first.startswith("C") or (kind == "DSB" and not second.startswith("C")):
                continue
            if line[65:69].strip() != "ns":
                raise ValueError("STEC requires nanosecond code biases")
            start, stop = bias_time(line[35:49]), bias_time(line[50:64])
            value = float(line[70:91]) * 299792458e-9
            if not math.isfinite(value) or stop <= start:
                raise ValueError("Invalid satellite code bias")
            if kind == "OSB":
                if mode != "ABSOLUTE":
                    raise ValueError("OSBs require ABSOLUTE BIAS_MODE")
                first, second = "_", first
            else:
                value = -value  # DSB is b(first)-b(second); graph edges store b(second)-b(first).
            rows[line[11:14]].append((start, stop, first, second, value))
    if time_system != "G" or not end:
        raise ValueError("Expected complete GPST Bias-SINEX")
    return rows


def bias_segments(rows, pairs, reference_dcbs, utc_start=None, utc_stop=None):
    """Graph differences within one product family; no cross-product code splicing.

    For G/E, align the ionospheric component of the product datum to CODE GIM:
    B_target - (K_target/K_reference) * (B_reference + DCB_GIM_first_minus_second).
    C/J retain the selected product datum and need their own GIM-constrained
    effective receiver bias; those outputs remain explicitly product-dependent.
    """
    out = []
    for satellite, entries in rows.items():
        system, prn = satellite[0], int(satellite[1:])
        selected = [p for p in pairs if p["system"] == system]
        bounds = sorted({v for r in entries for v in r[:2]})
        for start, stop in zip(bounds, bounds[1:]):
            graph = defaultdict(dict)
            for a, b, first, second, value in entries:
                if not a <= start < stop <= b:
                    continue
                if second in graph[first]:
                    raise ValueError("Overlapping/duplicate satellite signal bias")
                graph[first][second] = value
                graph[second][first] = -value

            def difference(first, second):
                if first == second:
                    return 0.0
                seen = {first: 0.0}
                queue = deque([first])
                while queue:
                    node = queue.popleft()
                    for target, value in graph[node].items():
                        value += seen[node]
                        if target in seen:
                            if abs(seen[target] - value) > 1e-5:
                                raise ValueError("Inconsistent satellite bias graph")
                        else:
                            seen[target] = value
                            queue.append(target)
                return seen.get(second)

            for pair in selected:
                value = difference("C" + pair["signal1"], "C" + pair["signal2"])
                if value is None:
                    continue
                item = dict(pair_id=pair["id"], prn=prn, start_ns=start, end_ns=stop)
                if system in ("G", "E"):
                    if satellite not in reference_dcbs:
                        continue
                    r1, r2 = ("C1W", "C2W") if system == "G" else ("C1X", "C5X")
                    reference = difference(r1, r2)
                    if reference is None:
                        continue
                    kr = meters_per_tecu(FREQUENCIES_HZ[system + "01"], FREQUENCIES_HZ[system + ("02" if system == "G" else "05")])
                    value -= pair["meters_per_tecu"] / kr * (reference + reference_dcbs[satellite])
                    item.update(start_utc_ns=utc_start, end_utc_ns=utc_stop)
                out.append(item | dict(meters=value))
    return out


class StecProducts(LocalProducts):
    def __init__(self, root, scratch, pairs, margin_hours=6, bias_family="COD0MGXFIN"):
        super().__init__(root, scratch, margin_hours)
        self.pairs = pairs
        if not re.fullmatch(r"[A-Z0-9]{10}", bias_family):
            raise ValueError("Invalid bias_product_family")
        self.bias_family = bias_family
        self.coverage = defaultdict(set)
        self.missing = set()
        self.touched = set()
        self.ionex_cache = {}
        self.bias_cache = {}

    def unpack(self, path):
        value = super().unpack(path)
        self.touched.add(Path(path).resolve())
        return value

    def available(self, name):
        if not self.files.get(name) and not self.files.get(name.removesuffix(".gz")):
            self.missing.add(name)
            return None
        return self.find(name)

    def prepare(self, start_ns, end_ns):
        first = (calendar(start_ns / 1e9) - self.margin).date()
        last = (calendar(end_ns / 1e9) + self.margin).date()
        key = first, last
        if self.loaded_key == key:
            return None
        self.touched.clear()
        result = dict(sp3=[], clk=[], nav=[], ionex=[], biases=[])
        for offset in range((last - first).days + 1):
            day = first + dt.timedelta(days=offset)
            stamp = day.strftime("%Y%j") + "0000"
            for kind, name in (
                ("sp3", f"COD0MGXFIN_{stamp}_01D_05M_ORB.SP3.gz"),
                ("clk", f"COD0MGXFIN_{stamp}_01D_30S_CLK.CLK.gz"),
                ("nav", f"BRDC00IGS_R_{stamp}_01D_MN.rnx.gz"),
            ):
                path = self.available(name)
                if path is not None:
                    result[kind].append(path)
            # Choose one declared daily bias family; a missing file is not an
            # invitation to silently switch datum to another producer.
            names = [
                n
                for n in self.files
                if n.startswith(f"{self.bias_family}_{stamp}_01D_")
                and (n.endswith(("_OSB.BIA.gz", "_OSB.BIA", "_DCB.BSX.gz", "_DCB.BSX")))
            ]
            if len(names) > 1:
                raise ValueError(f"Ambiguous daily bias product: {names}")
            bia = self.find(names[0]) if names else None
            if bia is None:
                self.missing.add(f"{self.bias_family}_{stamp}: satellite OSB/DSB")
            ionex = self.available(f"COD0OPSFIN_{stamp}_01D_01H_GIM.INX.gz")
            dcbs, utc_start, utc_stop = {}, None, None
            if ionex is not None:
                if ionex not in self.ionex_cache:
                    self.ionex_cache[ionex] = ionex_header(ionex)
                start, stop, dcbs = self.ionex_cache[ionex]
                if start.date() != day:
                    raise ValueError("IONEX contents do not match selected product date")
                utc_start = int((start - EPOCH).total_seconds()) * 10**9
                utc_stop = int((stop - EPOCH).total_seconds()) * 10**9
                result["ionex"].append(ionex)
            if bia is not None:
                if bia not in self.bias_cache:
                    self.bias_cache[bia] = signal_biases(bia)
                rows = bias_segments(self.bias_cache[bia], self.pairs, dcbs, utc_start, utc_stop)
                result["biases"].extend(rows)
                for row in rows:
                    self.coverage[row["pair_id"]].add(row["prn"])
        # Keep only this requested window. Never unlink an uncompressed source.
        for source in set(self.unpacked) - self.touched:
            target = self.unpacked.pop(source)
            if source.suffix == ".gz":
                Path(target).unlink()
            self.ionex_cache.pop(target, None)
            self.bias_cache.pop(target, None)
        self.loaded_key = key
        return result
