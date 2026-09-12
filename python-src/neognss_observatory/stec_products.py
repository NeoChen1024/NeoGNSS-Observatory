# SPDX-License-Identifier: GPL-3.0-only
"""CODE GIM datum plus exact-signal MGEX code OSBs for GPS STEC."""

import datetime as dt
import math
from collections import defaultdict
from pathlib import Path

from .gpst import EPOCH, calendar
from .ppp_products import LocalProducts, bias_time


def ionex_header(path):
    """Validate the selected CODE family and read its C1W-C2W satellite DCBs.

    IONEX UT epochs stay explicitly tagged until native UTC->GPST conversion.
    No filename-derived scientific time coverage.
    """
    values, dcbs, first, last, end, reference = {}, {}, None, None, False, False
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
            if "Reference observables for GPS" in line and "C1W-C2W" in line:
                reference = True
            if label == "PRN / BIAS / RMS" and line[3:4] == "G":
                prn, bias, rms = int(line[4:6]), float(line[6:16]), float(line[16:26])
                if prn in dcbs or not math.isfinite(bias) or not math.isfinite(rms) or rms < 0:
                    raise ValueError("Invalid or duplicate IONEX satellite DCB")
                dcbs[prn] = bias * 299792458e-9
            if label == "END OF HEADER":
                end = True
                break
    if not end or not reference or first is None or last != first + dt.timedelta(days=1) or not dcbs:
        raise ValueError(f"Incomplete CODE C1W-C2W IONEX header: {path}")
    if values != {"INTERVAL": 3600, "BASE RADIUS": 6371, "# OF MAPS IN FILE": 25, "HGT1 / HGT2 / DHGT": (450, 450, 0)}:
        raise ValueError(f"Unsupported CODE IONEX grid definition: {values}")
    return first, last, dcbs


def intra_frequency_biases(path, signal1, signal2):
    """Return (b2-b2W) - (b1-b1W), not the MGEX interfrequency datum."""
    groups = defaultdict(dict)
    time_system = mode = None
    end = False
    required = {signal1, signal2, "1W", "2W"}
    with open(path, encoding="latin-1") as stream:
        for line in stream:
            if line.startswith(" TIME_SYSTEM"):
                time_system = line.split()[-1]
            if line.startswith(" BIAS_MODE"):
                mode = line.split()[-1]
            if line.startswith("-BIAS/SOLUTION"):
                end = True
            if not line.startswith(" OSB") or line[11:12] != "G" or line[15:24].strip() or line[25:26] != "C":
                continue
            signal = line[26:28]
            if signal not in required:
                continue
            if line[65:69].strip() != "ns":
                raise ValueError("STEC requires nanosecond code OSBs")
            start, stop = bias_time(line[35:49]), bias_time(line[50:64])
            value = float(line[70:91]) * 299792458e-9
            if not math.isfinite(value) or stop <= start:
                raise ValueError("Invalid satellite code OSB")
            group = groups[(int(line[12:14]), start, stop)]
            if signal in group:
                raise ValueError("Duplicate satellite OSB")
            group[signal] = value
    if time_system != "G" or mode != "ABSOLUTE" or not end:
        raise ValueError("Expected complete GPST absolute Bias-SINEX")
    rows = []
    for (prn, start, stop), values in groups.items():
        if not required <= values.keys():
            continue  # Native lookup rejects an actually observed missing pair.
        rows.append(
            dict(prn=prn, start_ns=start, end_ns=stop, meters=values[signal2] - values["2W"] - values[signal1] + values["1W"])
        )
    return rows


class StecProducts(LocalProducts):
    def __init__(self, root, scratch, signal1, signal2, margin_hours=6):
        super().__init__(root, scratch, margin_hours)
        self.signal1, self.signal2 = signal1, signal2
        self.missing = set()

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
        # Only temporary expansions owned by this resolver are discarded.
        for source, target in self.unpacked.items():
            if source.suffix == ".gz":
                Path(target).unlink()
        self.unpacked.clear()
        result = dict(sp3=[], clk=[], nav=[], ionex=[], biases=[], gim_biases=[])
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
            bia = self.available(f"COD0MGXFIN_{stamp}_01D_01D_OSB.BIA.gz")
            if bia is not None:
                result["biases"].extend(intra_frequency_biases(bia, self.signal1, self.signal2))
            ionex = self.available(f"COD0OPSFIN_{stamp}_01D_01H_GIM.INX.gz")
            if ionex is None:
                continue
            start, stop, biases = ionex_header(ionex)
            if start.date() != day:
                raise ValueError("IONEX contents do not match selected product date")
            result["ionex"].append(ionex)
            result["gim_biases"].extend(
                dict(
                    prn=prn,
                    start_utc_ns=int((start - EPOCH).total_seconds()) * 10**9,
                    end_utc_ns=int((stop - EPOCH).total_seconds()) * 10**9,
                    meters=value,
                )
                for prn, value in biases.items()
            )
        self.loaded_key = key
        return result
