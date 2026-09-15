# Receiver time association

Status: agreed contract; see the importer checklist for implementation coverage.

RawBits and receiver telemetry share this association policy. Observation and
MeasurementClock retain their own measurement time; navigation time never
overwrites it. All absolute coordinates use GPST decimal seconds.

## Missing absolute time

Keep complete records even without a trustworthy navigation anchor. Their GPST
coordinate is null; preserve available receiver uptime and arrival order. Do not
extrapolate GPST from uptime, host time, byte counts or expected cadence. Do not
wait for a future anchor, discard an untimed RawBits backlog, or retrospectively
fill timestamps after recovery. Protocol framing may still buffer incomplete bytes.

Use the last known GPST day for untimed records. Before any reliable date is
known, use `1980/01/06/`, the GPST origin date, solely as an archive directory.
The row timestamp remains null, not zero. On recovery, subsequent records use
the new date; previously published rows are not moved. Closed files receive
continuation through a new `partNN`, never an in-place Parquet append.

Directory dates are placement context, not asserted measurement dates for
untimed records. Readers preserve part/row order, not a sort of nullable GPST.
Absolute-time consumers skip unusable times or break the affected calculation;
they must not silently hold the preceding GPST forward.

## Freshness

`epoch_period_s` is required. The association timeout is exactly
`10 * epoch_period_s`; equality is accepted. A valid navigation anchor can be
held while fresh. Explicit invalid navigation time disables it immediately.
Age advances only through receiver time evidence: trusted GPST progress or
uptime progress from a matched anchor. Host processing time is irrelevant.
Without new receiver time evidence, elapsed age cannot be measured; this is not
a physical reception-age guarantee.

Associated uptime uses the same freshness limit and retains its native
resolution. MON-SYS seconds are acceptable; do not interpolate subsecond uptime.
Directly reported uptime and associated uptime are distinguished. A nearby
status report usually gives roughly second-level pulse context at normal
cadence, not a guaranteed subsecond target-pulse timestamp.

## Restart boundaries

An uptime decrease is a restart under this project's policy; do not unwrap
native uptime rollover. A fresh paired navigation GPST and uptime establish
`offset = gpst - uptime`. A change from the previous fresh pair greater than
`max(5 s, 2 * epoch_period_s)` also indicates restart under this policy.
Equality is accepted. This is an engineering threshold, not a GNSS standard or
spoofing detector. Do not classify further possible causes.

Only pair fresh nearby reports, within one configured cadence of independently
known receiver time. Do not compare a newly reported GPST with a status held
near the full association timeout: that would manufacture a false restart.
Where freshness cannot be established, omit the offset-based check; the direct
uptime-decrease check remains available.

Emit a restart Event with the triggering evidence, clear the old associations
and start a new relationship. Store valid GPST/uptime pairs and the original
uptime reports so downstream clock processing can reconstruct continuity.
Never bridge an unwrap across a restart. File/day/batch boundaries alone do not
reset this state. Resume must retain the same association state.
