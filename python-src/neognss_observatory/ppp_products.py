# SPDX-License-Identifier: GPL-3.0-only
"""Local product selection for the GPS L1/L2 float PPP research path."""

import datetime as dt
import gzip
import math
import shutil
from pathlib import Path

from .gpst import EPOCH, calendar


def bias_time(text):
    year, day, seconds = map(int, text.split(":"))
    return int(((dt.datetime(year, 1, 1) - EPOCH).total_seconds() + (day - 1) * 86400 + seconds) * 1_000_000_000)


class LocalProducts:
    """Bounded-window users share local discovery and temporary gzip expansion."""

    def __init__(self, root, scratch, margin_hours=6):
        if not math.isfinite(margin_hours) or margin_hours < 1:
            raise ValueError("Product safety margin must be at least one hour")
        self.root, self.scratch = Path(root), Path(scratch)
        self.margin = dt.timedelta(hours=margin_hours)
        self.files = {}
        for path in self.root.rglob("*"):
            if path.is_file():
                self.files.setdefault(path.name, []).append(path)
        self.unpacked, self.loaded_key, self.current = {}, None, None

    def unpack(self, path):
        path = Path(path).resolve()
        if path in self.unpacked:
            return self.unpacked[path]
        if path.suffix != ".gz":
            self.unpacked[path] = str(path)
        else:
            target = self.scratch / f"{len(self.unpacked):04d}-{path.stem}"
            with gzip.open(path, "rb") as source, target.open("xb") as output:
                shutil.copyfileobj(source, output)
            self.unpacked[path] = str(target)
        return self.unpacked[path]

    def find(self, name):
        candidates = self.files.get(name, [])
        if not candidates:
            candidates = self.files.get(name.removesuffix(".gz"), [])
        if len(candidates) != 1:
            raise ValueError(f"Expected one local product {name}; found {len(candidates)}")
        return self.unpack(candidates[0])


class Products(LocalProducts):
    def __init__(self, root, scratch, antenna, catalogs, margin_hours=6):
        super().__init__(root, scratch, margin_hours)
        self.antenna, self.catalogs = antenna, catalogs

    def receiver_antenna(self, start_ns, end_ns):
        matches = []
        for catalog in self.catalogs:
            path = self.unpack(catalog)
            with open(path, encoding="latin-1") as stream:
                block = []
                for line in stream:
                    if "START OF ANTENNA" in line:
                        block = []
                    block.append(line)
                    if "END OF ANTENNA" not in line:
                        continue
                    identity = next((v[:20].rstrip() for v in block if "TYPE / SERIAL NO" in v), None)
                    if identity is None or identity.split() != self.antenna.split():
                        continue
                    valid_start, valid_end = 0, 2**63 - 1
                    for row in block:
                        if "VALID FROM" in row or "VALID UNTIL" in row:
                            fields = row[:43].split()
                            date = dt.datetime(*map(int, fields[:5])) + dt.timedelta(seconds=float(fields[5]))
                            value = int((date - EPOCH).total_seconds() * 1e9)
                            if "VALID FROM" in row:
                                valid_start = value
                            else:
                                valid_end = value
                    if not valid_start <= start_ns <= end_ns <= valid_end:
                        continue
                    frequencies = {v[:10].strip() for v in block if "START OF FREQUENCY" in v}
                    if not {"G01", "G02"} <= frequencies:
                        continue
                    matches.append((path, block, valid_start, valid_end))
        if not matches:
            raise ValueError(f"No exact antenna/radome with GPS G01/G02 calibration: {self.antenna}")
        # Configuration order expresses catalog precedence; select a whole record.
        path, block, valid_start, valid_end = matches[0]
        self.antenna_validity = (valid_start, valid_end)
        selected = self.scratch / "receiver-selected.atx"
        header = "     1.4            G                                       ANTEX VERSION / SYST\n"
        header += "A                                                           PCV TYPE / REFANT\n"
        header += "                                                            END OF HEADER\n"
        selected.write_text(header + "".join(block), encoding="latin-1")
        return str(selected), path, len(matches)

    def prepare(self, start_ns, end_ns):
        start, end = calendar(start_ns / 1e9), calendar(end_ns / 1e9)
        first, last = (start - self.margin).date(), (end + self.margin).date()
        key = (first, last)
        if key == self.loaded_key:
            if self.antenna_validity[0] <= start_ns <= end_ns <= self.antenna_validity[1]:
                return None
        days = [first + dt.timedelta(days=i) for i in range((last - first).days + 1)]
        out = dict(sp3=[], clk=[], nav=[], erp=[], biases=[], time_ns=start_ns)
        for day in days:
            stamp = day.strftime("%Y%j") + "0000"
            for kind, suffix in (
                ("sp3", "01D_05M_ORB.SP3.gz"),
                ("clk", "01D_30S_CLK.CLK.gz"),
                ("erp", "01D_12H_ERP.ERP.gz"),
            ):
                out[kind].append(self.find(f"COD0MGXFIN_{stamp}_{suffix}"))
            out["nav"].append(self.find(f"BRDC00IGS_R_{stamp}_01D_MN.rnx.gz"))
            path = self.find(f"COD0MGXFIN_{stamp}_01D_01D_OSB.BIA.gz")
            with open(path, encoding="latin-1") as stream:
                for line in stream:
                    if "TIME_SYSTEM" in line and line.split()[-1] != "G":
                        raise ValueError("Only GPST Bias-SINEX is supported")
                    if not line.startswith(" OSB") or line[11:12] != "G" or line[15:24].strip() or line[25:26] != "C":
                        continue
                    if line[65:69].strip() != "ns":
                        raise ValueError("Only nanosecond code OSB is supported")
                    meters = float(line[70:91]) * 1e-9 * 299792458
                    if not math.isfinite(meters):
                        raise ValueError("Non-finite satellite code OSB")
                    out["biases"].append(
                        dict(
                            prn=int(line[12:14]),
                            signal=line[26:28],
                            start_ns=bias_time(line[35:49]),
                            end_ns=bias_time(line[50:64]),
                            meters=meters,
                        )
                    )
        out["receiver_antex"], antenna_source, matches = self.receiver_antenna(start_ns, end_ns)
        out["satellite_antex"] = self.unpack(self.catalogs[0])
        out["metadata"] = dict(
            family="COD0MGXFIN",
            reference_system="IGS20",
            days=[str(d) for d in days],
            margin_hours=self.margin.total_seconds() / 3600,
            antenna_source=Path(antenna_source).name,
            antenna_matches=matches,
            antenna_selection="first exact GPS G01/G02 record in configured catalog order",
        )
        self.loaded_key, self.current = key, out["metadata"]
        return out
