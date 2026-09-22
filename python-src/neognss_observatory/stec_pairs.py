# SPDX-License-Identifier: GPL-3.0-only
"""Fixed L1-anchored families and exact-code priorities for automatic STEC."""

from itertools import product

from .antenna import (
    FREQUENCIES_HZ,
    CalibrationUnavailable,
    model_metadata,
    receiver_model,
)

# Order is a scientific selection policy, independent of row/channel order.
# Each family emits at most one exact pair for a satellite/epoch.
FAMILIES = (
    ("G", "L1L2", "G01", "G02", "1C 1W 1L 1X 1S 1P 1Y 1M", "2L 2W 2X 2S 2C 2P 2Y 2M"),
    ("G", "L1L5", "G01", "G05", "1C 1W 1L 1X 1S 1P 1Y 1M", "5Q 5X 5I"),
    ("J", "L1L2", "J01", "J02", "1C 1E 1L 1X 1S", "2L 2X 2S"),
    ("J", "L1L5", "J01", "J05", "1C 1E 1L 1X 1S", "5Q 5X 5I"),
    ("E", "E1E5b", "E01", "E07", "1C 1X 1B", "7Q 7X 7I"),
    ("E", "E1E5a", "E01", "E05", "1C 1X 1B", "5Q 5X 5I"),
    ("C", "B1CB2b", "C01", "C07", "1P 1X 1D", "7D 7Z 7P"),
    ("C", "B1CB2a", "C01", "C05", "1P 1X 1D", "5P 5X 5D"),
    ("C", "B1IB2I", "C02", "C07", "2I 2X 2Q", "7I 7X 7Q"),
)


def meters_per_tecu(f1, f2):
    return 40.3e16 * (f2**-2 - f1**-2)


def build_pairs(station, setup, antenna_mode, antenna_policy):
    pairs, models, missing = [], {}, {}
    for family_id, (system, family, band1, band2, first, second) in enumerate(FAMILIES):
        antenna = None
        if antenna_mode == "required":
            calibration = setup["antenna"].get("calibration_file")
            if not calibration:
                raise ValueError("STEC requires Setup antenna.calibration_file, or explicit receiver_antenna='none'")
            try:
                antenna = receiver_model([station / calibration], setup["antenna"], [band1, band2], **antenna_policy)
            except CalibrationUnavailable as error:
                missing[f"{system}:{family}"] = str(error)
        models[str(family_id)] = antenna
        f1, f2 = FREQUENCIES_HZ[band1], FREQUENCIES_HZ[band2]
        for signal1, signal2 in product(first.split(), second.split()):
            pairs.append(
                dict(
                    id=len(pairs),
                    family_id=family_id,
                    family=family,
                    system=system,
                    signal1=signal1,
                    signal2=signal2,
                    fusion_group=("C-B1I" if family == "B1IB2I" else "C-B1C" if system == "C" else system),
                    frequency1_hz=f1,
                    frequency2_hz=f2,
                    meters_per_tecu=meters_per_tecu(f1, f2),
                )
            )
    return pairs, models, missing


def antenna_metadata(models):
    return {k: model_metadata(v) if v else None for k, v in models.items()}
