# SPDX-License-Identifier: GPL-3.0-only
"""Build a vector PPP report from numerical summaries and solution tables."""

import json
import textwrap

import numpy as np
from tqdm import tqdm

from .gpst import calendar


def write_report(source, output, kinds, render_figure):
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.backends.backend_pdf import PdfPages
    from matplotlib.patches import Ellipse

    summary = json.loads((source / "summary.json").read_text())
    settings, position = summary["settings"], summary["position"]
    final = summary["final_epoch_solution"]
    station = str(summary["station"])
    accent = "#174c69"
    report = output / "ppp-report.pdf"

    def timestamp(ns):
        return calendar(ns / 1e9).strftime("%Y-%m-%d %H:%M:%S") + " GPST"

    def vector(values, precision=4):
        return " / ".join(f"{v:.{precision}f}" for v in values)

    def page(title, subtitle):
        fig = plt.figure(figsize=(11.69, 8.27), facecolor="white")
        fig.text(0.06, 0.94, "NEOGNSS OBSERVATORY", fontsize=10, color=accent, weight="bold")
        fig.text(0.06, 0.875, title, fontsize=22, color=accent)
        fig.text(0.06, 0.83, subtitle, fontsize=10, color="#555555")
        return fig

    def table(fig, rows, *, width=78, value_x=0.31):
        y = 0.76
        for label, value in rows:
            lines = textwrap.wrap(str(value), width=width, break_long_words=True) or ["unavailable"]
            fig.text(0.06, y, label, fontsize=10, weight="bold", va="top", color=accent)
            fig.text(value_x, y, "\n".join(lines), fontsize=10, va="top", linespacing=1.4)
            y -= max(1, len(lines)) * 0.027 + 0.017
        return y

    with PdfPages(report, metadata={"Title": f"{station} - PPP Float report", "Author": "NeoGNSS Observatory"}) as pdf:

        def save(fig):
            fig.text(0.06, 0.035, "NeoGNSS Observatory | PPP Float | GPST", fontsize=8, color="#666666")
            fig.text(0.94, 0.035, str(pdf.get_pagecount() + 1), fontsize=8, ha="right", color="#666666")
            pdf.savefig(fig)
            plt.close(fig)

        windows = summary.get("product_windows") or [summary["products"]]
        products = ", ".join(sorted({w["family"] for w in windows}))
        catalogs = ", ".join(sorted({w["antenna_source"] for w in windows}))
        references = ", ".join(sorted({w["reference_system"] for w in windows}))
        attempted, solved = summary["attempted_epochs"], summary["solved_epochs"]
        fig = page("PPP solution summary", f"{station} | {summary['engine']}")
        table(
            fig,
            [
                ("Solution", f"{summary['systems']} | {summary['mode'].replace('_', ' ')}"),
                ("Input protocol", summary["protocol"].upper()),
                ("First attempted epoch", timestamp(summary["start_gpst_ns"])),
                ("Last attempted epoch", timestamp(summary["end_gpst_ns"])),
                ("Last valid estimate", timestamp(final["gpst_ns"])),
                ("Valid / attempted", f"{solved:,} / {attempted:,} ({100 * solved / attempted:.2f}%)"),
                (
                    "Sampling / mask",
                    f"{settings['interval']:g} s estimation interval / {settings['elevation_deg']:g} deg elevation",
                ),
                ("Signals", f"GPS {settings['signal1']} + {settings['signal2']} (ionosphere-free combination)"),
                ("Products / frame", f"{products} / {references}"),
                ("Receiver antenna", settings["antenna"]),
                ("Antenna catalogs used", catalogs),
                ("ARP E / N / U (m)", vector(settings["arp_enu_m"])),
            ],
        )
        fig.text(0.06, 0.12, "Valid percentage counts attempted decimated epochs, not raw recording completeness.", fontsize=10)
        fig.text(
            0.06, 0.085, "Static result is the last valid forward filter estimate, not an average or an AR solution.", fontsize=10
        )
        save(fig)

        fig = page("Position and formal uncertainty", f"{station} | Marker coordinates | Ellipsoidal height")
        covariance = np.asarray(position["covariance_enu_m2"])
        ellipse = position["horizontal_ellipse_95"]
        table(
            fig,
            [
                ("ECEF X (m)", f"{position['ecef_m'][0]:.4f}"),
                ("ECEF Y (m)", f"{position['ecef_m'][1]:.4f}"),
                ("ECEF Z (m)", f"{position['ecef_m'][2]:.4f}"),
                ("Latitude / longitude", vector([position["latitude_deg"], position["longitude_deg"]], 9) + " deg"),
                ("Ellipsoidal height", f"{position['ellipsoidal_height_m']:.4f} m"),
                ("Delta E / N / U (m)", vector([final[k] for k in ("east", "north", "up")])),
                ("Formal E / N / U 1σ", vector(np.sqrt(np.maximum(np.diag(covariance), 0))) + " m"),
                ("95% ellipse axes", vector([ellipse["semi_major_m"], ellipse["semi_minor_m"]]) + " m (semi-major / minor)"),
                ("Ellipse azimuth", f"{ellipse['azimuth_deg']:.2f} deg clockwise from north"),
            ],
            width=40,
            value_x=0.28,
        )
        ax = fig.add_axes((0.68, 0.29, 0.27, 0.40))
        major, minor = ellipse["semi_major_m"] * 1000, ellipse["semi_minor_m"] * 1000
        ax.add_patch(Ellipse((0, 0), 2 * major, 2 * minor, angle=90 - ellipse["azimuth_deg"], fc="#dbeaf2", ec=accent))
        ax.plot(0, 0, "+", color=accent)
        extent = max(major * 1.25, 1)
        ax.set(xlim=(-extent, extent), ylim=(-extent, extent), xlabel="East (mm)", ylabel="North (mm)")
        ax.set_aspect("equal")
        ax.grid(alpha=0.2)
        ax.set_title("Horizontal 95% ellipse\nCentered on final estimate", fontsize=10)
        fig.text(
            0.06,
            0.15,
            "Delta is estimated minus a priori marker position. Antenna ARP offsets are applied separately.",
            fontsize=10,
        )
        fig.text(
            0.06,
            0.105,
            "Formal uncertainty describes the adopted model; it is not independently measured positioning accuracy.",
            fontsize=10,
        )
        save(fig)

        sections = [
            ("Applied models", summary["models"]),
            ("Scientific limits", [v for v in summary["limitations"] if v != "not equivalent to CSRS-PPP"]),
            (
                "Interpretation",
                [
                    "No PPP-AR: ambiguities remain float. No fixed percentages or reference ambiguities are fabricated.",
                    "Clock is the GPS-referenced PPP estimate, not UBX-NAV-CLOCK. Jumps are not unwrapped.",
                    "Residuals are post-fit ionosphere-free phase/code residuals in meters, not pure receiver noise.",
                    "The position detail plot omits the first hour for readability; it does not detect convergence.",
                ],
            ),
        ]
        for heading, entries in sections:
            # Bound each page by wrapped line count, including unusually long metadata.
            lines = [line for entry in entries for line in textwrap.wrap("• " + str(entry), 100)]
            for offset in range(0, len(lines), 22):
                fig = page(heading, f"{station} | Processing assumptions and limitations")
                for index, line in enumerate(lines[offset : offset + 22]):
                    fig.text(0.06, 0.75 - index * 0.028, line, fontsize=11, va="top")
                save(fig)

        for kind in tqdm(kinds, desc="PPP report", unit="page"):
            fig = render_figure(source, kind)
            fig.set_size_inches(11.69, 8.27)
            fig.set_layout_engine("constrained", rect=(0.02, 0.075, 0.96, 0.91))
            save(fig)
    return report
