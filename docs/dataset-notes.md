# Dataset interpretation notes

These facts affect how the existing observations are processed. They are not
a live inventory or a substitute for reading payload timestamps.

## Era A: overlaps and acquisition gaps

Era A `ubx24h` captures were requested for 86,460 seconds through
`receiver → RPi 4B PL011 UART0 → gpsd/gpspipe → xz → storage`.
The recording host used `dtoverlay=disable-bt`, not the mini UART. Storage
changed from SD card to USB 3.0 SSD. The maintained C++ logger was not in this
acquisition path.

The longer-than-one-day capture window explains why adjacent files can
overlap. Remove duplicate regions only after byte-level overlap proof using
the [restitch profile](dataset-qa.md). Capture duration and wall-clock file
names do not establish observation coverage.

Frequent payload gaps exist independently of these overlaps. Operator reports
of direct TCP acquisition did not reproduce the frequent-gap pattern, making
the gpsd/gpspipe chain a suspect, not a proven cause. Do not attribute these
archive gaps to the maintained logger or assume that changing storage alone
explains them. Preserve observed gaps; neither reconstruction nor analysis
should fill them or relax thresholds merely to hide them.

## Eras B and C

Nonoverlapping Era B UBX and Era C SBF do not require reconstruction before
extraction. Physical recording boundaries are not necessarily GPST day
boundaries, receiver restarts, or missing observations. Different logging eras
may overlap in calendar time; do not merge them into one receiver stream
without resolving their relationship.

Era C includes raw SBF and receiver-generated RINEX. The receiver RINEX's
30-second observations are not a replacement for native-rate SBF when an
analysis needs higher-rate measurements. Header signal declarations alone do
not establish that those signals have observations.

Unassigned reconstruction outputs have no defensible GPST assignment and are
excluded from automatic extraction. Filenames never supply their missing time.
