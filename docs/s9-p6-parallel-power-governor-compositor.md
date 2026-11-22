# Galaxy S9 / @ioss9 — Parallel source, Scion, compositor & thermal investigation

Status: read-only integration work for branch `s9/p6-unified-wake-thermal-display-lab`.
This is a diagnostic experiment, **not an optimization** and **not permission
to modify charging current, charger watchdog, CPU power states or GPU gates**.

## Why one combined trial?

The user verified **the same charger, USB cable, USB port and inline power meter**
were used across both runs. External USB draw increased from ~0.47–0.49 A
to ~0.71–0.78 A. The 2026-11-22 05:43 raw S9 log reported that at ~15min
the MAX77705 gauge had only declined by 6 mAh, versus 67 mAh in the
2026-11-21 23:53 run, but CPU/GPU work remained essentially unchanged.
The improved battery balance was visible **before** P6.10–P6.12
observer phases started: it cannot be attributed to their runtime
activation.

More USB input current is not automatically less device power consumption:
input volts, charger protocol/current limit, battery SOC, thermals and
power-path routing matter. The fuel gauge measures **battery-side current**,
not 5 V USB input current. High SOC near 100% also confounds the charger
state. A byte-for-byte similar source/port does not guarantee an identical
charging IC state across boots.

## Parallel evidence streams, one new matched image

| Stream | Data, source and cadence | What it distinguishes |
|---|---|---|
| **A. MAX77705 power path** | Existing `max77705_diag=1` + new opt-in `max77705_diag_s=30`: charger registers snapshot each 30 s (20 I2C registers, never 0xB0). Existing `max77705_fg_s=10`: gauge Current, AvgCurrent, SOC, capacity and raw QH every 10 s. Neither writes charger registers. | CHGIN valid input vs CHG_DTLS, WDTEN, CNFG_00 and CNFG_09 before/after charging stops; gauge battery support vs external input |
| **B. Scion governor** | Existing `p610-gov`/`p610-gap`, plus `p610-trans` only on state transition (at most 4 lines per 1s bucket) with timestamp and exact frame/touch/quiet/gap snapshot. | Whether the rare STATIC transition with 100ms frame ages was real, a race, or a summary aggregation mismatch; how much state lingered ANIMATING |
| **C. IOSurface / EAGL / GPU** | Existing P6.11 sampled checksum by IOSurface, P6.12 EAGL double-buffer output and independent kernel `fps`; existing `p6-ca` and `p5-gpu` work proxies. | Whether large IOSurfaces 131/132 still dominate checksum and whether CA continues to produce ~10 FPS without GPU work or display losses |
| **D. Thermal/CPU comparison** | Existing TMU battery/BIG/GPU, `cpufreq_gov`, `poster_cost`, P6 wake and thread sampling, baseline vs combined vs isolated. | Whether differences track CPU/GPU work, charger state, battery heat, or simply elapsed time |

No additional kernel thread. New charger diagnostic sampling occurs on the
already running 5-second MAX77705 battery worker and is **OFF by default**.
It reads 0xB2..0xC5 but **never 0xB0 CHG_INT (read-to-clear)**.
The existing watchdog write path remains **disabled** unless someone
separately chooses `max77705_wdt=1`; do **not** enable it in this test.

### Required bootargs for one combined BOOT+SYSTEM

Retain your previous known-working BOOT/SYSTEM bootargs and append
the **three** read-only tokens:

```text
max77705_diag=1 max77705_diag_s=30 max77705_fg_s=10
```

Ensure `max77705=1`, `poster_probe=1`, `mali_gov=1`,
`mali_dvs=0` and the established Metal/GL settings are preserved.
**Do not include `max77705_wdt=1`, a charger limit override, or
G3D real gate mode.** These are diagnostic bootargs parsed at driver
startup; merely setting a sysctl later **does not** enable them.
Changing the kernel for P6.10 transition logging and new MAX77705
sampling requires building one new **matching BOOT + SYSTEM** pair;
do not mix the new kernel with an older EAGL SYSTEM.

Installer branch `fix/s9-installer-device-selection` has previously
been used with `kernel_ref=s9/p6-unified-wake-thermal-display-lab`,
`metal=true`, `userdata_blocks=starlte=13917184`, `time_limit=0`.
An installer may **FORMAT USERDATA**. Before any flash, verify
off-device BOOT, SYSTEM, EFS and USERDATA backups and recovery.

### Physical test protocol

Use the **same known charger/cable/USB meter/port** and an unchanged
display brightness and wallpaper. For meaningful charger state, a
moderately charged battery (ideally ~40–80%) is more informative than
100%, but do **not** deliberately drain or heat the battery. Begin
while the device is cool, preferably below 40°C battery temperature.
Keep the battery thermistor reading visible and abort if hot or
abnormally behaving; do not force charging.

Record on paper the inline meter **volts and amps** at initial
connection and ~2, 5 and 10 minutes after boot, including any
sudden jump from ~0.48 A to ~0.75 A. External values are **not**
automatically present in the kernel raw log.

After the matching image has booted, the screen is visibly ON,
and PuTTY is closed (COM cannot be shared), first preview:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/s9-p6-serial-auto.ps1 -ParallelOnly
```

Then optionally run (only if the battery is cool enough and the
COM port is owned by this script):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/s9-p6-serial-auto.ps1 -Port COM125 -ParallelOnly -BatteryStopC 46 -Execute
```

Its **host runtime** comprises:

| Phase | Duration | Runtime observer plan |
|---|---:|---|
| 30 | 60s | All P610/P611/P612, P5/P6 OFF; temperature, framebuffer and MAX77705 boot probes continue |
| 31 | 60s | P610 + P611 + P612 ON together, P5/P6 supporting observations ON |
| 32 | 60s | Only P610 ON; exact state transition snapshots |
| 33 | 60s | Only P611 ON; checksum attribution without P610/P612 |
| 34 | 60s | Only P612 ON; framebuffer present/flip vs EAGL target IDs |
| 35 | 60s | All observers OFF again |
| 9 | cleanup | Exact sysctl echo + ACK SAFE EXIT, including G3D gate0 |

P6 mode/flags preserve CPU governor policy and all graphics behavior.
No power button or physical screen-OFF action. No actual G3D gate.
The watcher only **requests early stop** if *received serial telemetry*
reports battery ≥46°C; the check cannot guarantee hardware protection
if serial disconnects, stops producing temperature lines, or lags.
Always observe the physical device. Existing kernel thermal shutdown
is separate. The `-BatteryStopC` parameter must **not** be raised to
extend a hot battery experiment.

Note: the MAX77705 bootarg streams run from boot, even when P6
runtime observers are OFF, making pre-phase comparisons possible.
Turn P6 observers ON after initial charger behavior has been captured,
not in place of charging telemetry.

### Reports

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/s9-p6-parallel-report.ps1 -Log putty-P6-unified-YYYYMMDD-HHMMSS.log
powershell -NoProfile -ExecutionPolicy Bypass -File tools/s9-p6-deep-report.ps1 -Log putty-P6-unified-YYYYMMDD-HHMMSS.log
```

The parallel report correlates exact kernel uptime for charger snapshots
and fuel gauge, raw QH beginning/end (scale deliberately **not**
converted to mAh), MAX77705 CHGIN / WDTEN / CNFG_09 / CHG_DTLS,
governor transitions and phase-level compositor/FPS/thermal metrics.
It explicitly marks missing charger telemetry **UNMEASURED**,
rather than inventing charger state or USB current.

If `p610-trans` shows `from=ANIMATING` to `STATIC`
without `quiet_ms>=300`, investigate torn timestamps or read
order; do not change DVFS based on averages alone. The Scion
`dev_state` classifier is not itself a command that changes CPU
voltage/frequency in the tested policy2 path.

If the two heavy IOSurface IDs again consume most checksum wall time,
the next optimization must preserve dirty detection correctness.
No image hash replacement, framebuffer identity inference or frame
skipping is enabled here.

### Thermal and data validity

The 2026-11-22 device run ended with battery thermistor **47.4°C**,
which is too warm to use as a starting condition for extended charging
experiments. `max77705: on a charger, not charging` at 100% SOC
may reflect full/thermal/charge state, not necessarily a charger bug.
WDTEN=1 alone is **not** proof of a watchdog timeout; look for a
change in CHG_DTLS and confirm from fresh, read-only charger registers.
USB input measured at ~0.75 A is not battery charging current,
and QH is raw (unknown scale/sign here).

Tests: `tools/tests/p6-parallel-synthetic.log` and
Windows PowerShell 5.1 CI preview/report regression; the legacy
P6.6–P6.12 scripts remain supported.
