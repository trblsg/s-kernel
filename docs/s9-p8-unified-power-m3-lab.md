# S9 P8 unified CPU / PSCI / PMU / thermal / GPU lab

P8 follows the real `putty-S9-p7-cpu-20261123-051827.log` failure. On that
run, four BIG processors (logical 3,4,5,6) exited the **XNU scheduler**.
`PSCI_AFFINITY_INFO=0` (firmware said ON) and Exynos Cluster1 PMU
status remained ON. `CPU_ON` returned 0 for CPU3 but target stage3 was
never reached within five seconds; GenericARMPE reported failure 5. This is
**not** evidence of physical CPU_OFF and **not** a successful re-entry.
Do not run `-P7PowerTest CPU` or `p7_cpu_park=1` on the P7 image again.

## One image, many diagnoses

Branch: `s9/p8-unified-power-m3-lab` in `trblsg/s-kernel`.
It integrates on the same BOOT+SYSTEM/APFS image, without rebuilding between
phases:

| Source | Evidence / limitation |
| --- | --- |
| CPU | XNU per-core scheduler idle %, online mask, observed frequency and current thermal cap. Idle is **not** WFI, C2, CPD or SICD residency. |
| PSCI | Explicit read-only `AFFINITY_INFO` on each M3 (logical CPU 3–6), stage and start/off/return attempts. Stage1 is written by the **caller**, not the restarted core. PSCI ON/OFF is firmware state, not direct wattage. |
| Exynos PMU | Always-on Cluster1 configuration/status, shared NONCPU and L2 registers; never access powered-off CPU-domain registers. |
| Interrupt/wake | P6 USB callback and touch poll deltas as **proxies**. No direct per-IRQ CPU wake causality asserted. |
| Thermal | BIG/LITTLE/G3D TMU, DVFS and thermal/OCP ceiling, battery thermistor's last available reading. No current meter. |
| G3D | Existing P7 GPU PMU/PLL/driver ring (expanded to 512 rows), queue/fps diagnostics and optional **shadow-only** gate candidate `g3d_lab_gate=1`; no real G3D power gate. |
| Display | GenericARMPE requested ON/OFF, observed foreground and frame cadence; requested OFF is not panel-rail proof. |
| Compositor | P4 backboardd thread, GL and P6/HI2 counters where available; cannot equate unchanged IOSurface input with identical framebuffer. |
| PMIC | Demand-only ACPM BUCK2/BUCK3 VSEL code and CPU DVFS readback; VSEL -> nominal mV is **not** an independent voltmeter reading. |
| Context | Absolute uptime, common phase, 512-sample kernel ring; time-correlate with userland fps/clock observations. |

P8 instruments the existing governor worker; it creates no new worker.
`p8_mode=0` defaults OFF, `1` retains ring without verbose prints,
`2` prints a summary every configured 5–30 s. The ring retains at most
512 samples (about 42.7 minutes at 5 s) in RAM. P7 GPU ring is likewise
512 rows. `p8_psci_stats`, `p8_pmu_stats` and `p8_rail_stats` are
explicit sysctl requests, not 40 ms hot-path polling.

**Hardware interlock:** `p7_cpu_park=1` returns EPERM in both the
sysctl and GenericARMPE entry. The host also rejects legacy CPU power
testing **before opening COM**. P8 does not issue CPU_OFF, CPU_ON,
CPU_SUSPEND, CPD, SICD, PMU power writes, or voltage writes. The optional
BIG A/B/A phase temporarily lowers only the *requested maximum frequency*
to 1469 MHz via the existing governor. Stock thermal/OCP limits continue
to take precedence; 0 restores the original ceiling. This is an active
**frequency** experiment, not a purely read-only phase or a voltage test.

## Image and launch

Use the SIOS GitHub Actions `s9-installer` workflow on branch
`fix/s9-installer-device-selection`, kernel ref
`s9/p8-unified-power-m3-lab`, `metal=true`,
`userdata_blocks=starlte=13917184`, `time_limit=0`.
Compatible P7 boot args may be reused, but REMOVE `sleep_cores=1`
and do not add any CPU_OFF/CPU_SUSPEND flags. An example to adapt to
the currently verified boot args is:

```text
hi=2 mali_gov=1 mali_gov_frame=450 mali_probe=1 p3power=1 poster_probe=1 max77705_fg_s=10 max77705_diag=1 max77705_diag_s=30 glstage=1 glcost=1 p0hash=1 p0verify=1
```

P8 needs matching BOOT and SYSTEM userland (HI2/P4/GL observers); BOOT
alone cannot install missing userland instrumentation. **The full SIOS
installer formats USERDATA.** Preserve and verify BOOT, SYSTEM/APFS,
USERDATA and EFS backups. Do not flash repeatedly for every A/B/A: P8
performs all phases on one firmware. Ensure prior P7 recovery/reboot has
restored 8 online cores before starting.

On the actual Windows machine with COM125, CLOSE PuTTY: only one
process can own CDC-ACM. First run the preview without `-Execute`.
Then, with the device cool and working normally:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\s9-home-idle-serial-auto.ps1 -P8Unified -Port COM125
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\s9-home-idle-serial-auto.ps1 -P8Unified -Port COM125 -Execute -P8WindowSeconds 60 -P8LeaveAuditRunning
```

The script demands exact serial echo + kernel ACK, thermal and online
core checks, a proven quiet Home scene, fresh G3D status and read-only
PSCI/PMU snapshots before advancing. Defaults abort on BIG >=82°C,
LITTLE >=75°C, G3D >=77°C or battery >=47°C; missing/stale thermal
measurements also fail closed. Stop if any guard fails. **Do not use
`-P7PowerTest CPU` as a substitute**, even if the phone can return to recovery.

| Phase | Meaning |
| --- | --- |
| 80 | Sensor warm-up, all eight cores online |
| 81 | Quiet Home CPU/GPU/thermal baseline |
| 82 | P4 per-thread and P6 USB/touch wake proxy |
| 83 | Requested display OFF, no CPU/G3D power gate |
| 84 | Display ON and wake burst |
| 85 | G3D shadow gate candidates only |
| 87 | A1 stock BIG frequency ceiling |
| 88 | B lower-only 1469 MHz BIG frequency ceiling |
| 89 | A2 stock BIG frequency ceiling restored |
| 90 | Final PSCI/PMU/rail snapshots and records |
| 91 | Optional passive logging retained for USB-disconnected segment |

To capture a screen-locked or USB-disconnected segment, use
`-P8LeaveAuditRunning`, disconnect COM while the kernel samplers retain
rows in RAM, then reconnect and run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\s9-home-idle-serial-auto.ps1 -DumpP8History -Port COM125 -P8DumpLimit 512 -Execute
```

This **freezes** P8/P7 rings before extracting their read-only records.
When `-P8LeaveAuditRunning` is not selected, the normal cleanup disables
P8/P7 samplers and resets all altered controls. If the PC loses COM,
the host cannot guarantee immediate restore of the optional lower BIG cap;
rebooting from a known-good state resets the lab sysctl to stock (0).

## Offline reports and interpretation

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\s9-p8-report.ps1 -Log .\putty-S9-p8-unified-YYYYMMDD-HHMMSS.log -CsvPath .\p8.csv
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\s9-home-idle-serial-auto.ps1 -ReplayP8Log .\putty-S9-p8-unified-YYYYMMDD-HHMMSS.log
```

Preserve raw `.log` and `.events.txt`. The parser groups distinct
phase IDs and exports CPU/PMU/thermal CSV plus a separate GPU CSV. It
specifically flags CPU_ON re-entry failures and never translates a
`taken off (0)` scheduler event, a caller-written PSCI stage1,
a sysctl ACK, or GPU PMU status alone into proof of CPU electrical watts.
If a sensor is absent, treat it as **unmeasured**, not zero. A/B/A across
one boot is a useful control for time drift but is not a wattmeter or a
guaranteed thermal improvement.

## Real COM125 false timeout: P8 phase 82 (23 November 2026)

First physical P8 capture `putty-S9-p8-unified-20261123-091536.log`
advanced through phases 80 and 81. Phase 82's host transaction falsely
timed out after 24 seconds. The **raw kernel USB stream** contains all
three positive signals:

```text
p4-thread: ... s_raw=usb> sysctl kern.hakios.p7_cpu_phase=82
usb: sysctl kern.hakios.p7_cpu_phase=82: set (0)
p4-thread: ... thr=(unnamed) usb: kern.hakios.p7_cpu_phase = 82
```

The old host `Test-SysctlReadback` required `^usb:` at column zero.
Userland `p4-thread` output and kernel USB output share an **unserialized**
character stream; the reply can appear in the middle of another line.
This is **not** evidence of USB byte loss, kernel hang, or failed phase
change. P7 CPU samples continued reporting `phase=82`, 4+4 cores,
`exit=0`; PSCI CPU3–6 stage3/AFFINITY ON. A later cleanup
`p4_mode=0` command likewise received a real echo, `set (0)` and
readback embedded in `p4-thread` lines, but was wrongly flagged
unconfirmed. The subsequent safety resets (BIG cap 0, G3D gate 0,
P8/P7 CPU/GPU modes 0) had normal ACKs.

**Host-only fix:** accept the exact `usb: kern.hakios.NAME = NUMBER`
readback **at the end of any line**, even if a preceding logger's text is
on that line. Preserve independent exact command echo, `set (0)` ACK,
fresh per-command buffer and full numeric matching (`82` cannot match
`820`). Added a synthetic regression based on both real interleavings.
Only `tools/s9-home-idle-serial-auto.ps1` and test files change; the
already installed matching P8 BOOT+SYSTEM does **not** need reflashing.

This capture has 31 valid P8 rows (phases 80–82), 32 GPU PMU/PLL rows,
read-only PSCI 3–6, PMU and rail responses. Screen-OFF phase83 and
later G3D/frequency A/B/A phases **were not run**. Before rerunning,
verify a cool device and 8 online cores; close PuTTY and update the
Windows host script from this branch.
