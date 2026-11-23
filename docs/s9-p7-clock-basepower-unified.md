# S9 P7 — Clock owner proof + SoC Base Power / CPU idle
## One matched BOOT+SYSTEM, one Windows COM script, optional bounded power A/B

Feature kernel branch: `s9/p7-clock-basepower-oneboot`
Installer feature branch: `fix/s9-p7-clock-basepower-installer`

**Purpose.** HI2 A/B already proved Clock _timerFired: caused Home0's 10fps,
but it did NOT explain the thermal floor at 59–61C when the panel was dim,
off or physically absent. P6 screen-OFF logs found G3D domain ON / PLL
stable with **zero jobs and frames for 168 seconds**; P4 found four BIG and
four LITTLE online when frames=0, scheduler-idle BIG94–95% and LITTLE99%.
The P7 source is intended to determine the residual source(s) without
claiming unverified CPU C2/CPD/SICD or USB input watts.

### What is implemented

**Clock owner/visibility (userland).** Existing hi=2 causal A/B stays intact.
The previous UIView check gave `SBHClockApplicationIconImageView UIView=0`
in real logs. A runtime resolver now logs the target's Objective-C class
inheritance, looks for an actual UIView via direct target, typed view
properties or object-typed view/icon ivars, and classifies:
`unknown,visible,detached,hidden,outside`.
It does NOT guess an ivar offset or infer visibility from the class name.
A visible view overrides an offscreen candidate; conflicting view states
fail open. An offscreen state must remain identical for **8 continuous
seconds** and there must be **3 seconds without touch**, verified Home
main, known Clock action and an installed event hook.

The extra `kern.hakios.hi_policy=1` is an **opt-in, still experimental
visibility-aware policy**. It can act ONLY after HI2's five-phase
A/B/A/B has completed (`hi:auto=FINISHED`) and main Home has been
explicitly marked; only this Clock source may be thinned, forwarding
at least one callback per second and immediately forwarding on ambiguity
or user action. `hi_policy=0` restores original callback scheduling.
**If the owner cannot be resolved, there is no cut; this is correct and
the log prints `state=unknown` and `verified_cut=0`.** Visibility
or owner resolution is NOT proof of hardware power saving or that an
icon is globally absent. This mode is not enabled by default.

**P7 CPU ring (kernel, opt-in).** `kern.hakios.p7_cpu_mode=1` samples
the 8 scheduler-idle counters, physical cluster affinity, online/exit
mask, rates, requested display power state and USB/touch *driver*
callback count deltas. Uses the existing cpufreq governor thread every
5 seconds, no new worker; 96 snapshots in a ring kept in kernel RAM.
Selectors `p7_cpu_phase`, `p7_cpu_stats`, `p7_cpu_slot=0..95`,
`p7_cpu_record` can retrieve history even after USB was unplugged.
This does **not** measure hardware WFI, PSCI C2 CPU_SUSPEND, CPD,
SICD, actual GIC wakeups, DRAM frequency or electrical watts.

**P7 GPU ring (kernel, opt-in).** `p7_gpu_mode=1` uses the existing
G3D Power Lab 5s sleeping PMU/CMU worker. Logs real power-domain
and PLL readbacks, frame/job inactivity, off time, wake failures,
DTZPC safety, clock hint and last separately read BUCK6 PMIC voltage.
96 records remain readable via `p7_gpu_stats`, `p7_gpu_slot`,
`p7_gpu_record` after USB is reconnected. It NEVER reads powered-off
GPU registers or powers the domain up merely for measurement.
`g3d_lab_rail_probe=1` independently enables PMIC readback every30s.

**Real G3D OFF (EXPLICIT opt-in).** The Windows script can request
`g3d_lab_gate=3` with 10s no-job/no-frame grace on an otherwise
stationary Home secondary. This REUSES the existing GPU reset,
DTZPC SAVE/RESTORE, PMU, MMU, and 950mV safety checks; no custom
power sequence or undervolt. NEW P7 120-second watchdog runs on
the same independent worker and attempts to restore GPU ON and
gate=0 even if COM disappears. It latches and reports recovery
failure; a hardware hang cannot be guaranteed recoverable.

**Real BIG CPU_OFF (EXPLICIT opt-in, not PSCI C2).** The existing
`sleep_cores=1` allocator and per-core `processor_exit()/unpark()`
path is used only if the new runtime `p7_cpu_park=1` is requested
during a P7-monitored test. Default0 means **all cores stay on
even with `sleep_cores=1` in boot args**. It reroutes eligible
interrupts and preserves loader/master cores. A screen-OFF experiment
runs one baseline without parking, then a second with BIG cores
parked and confirmed `taken off` / `started again`. `p7_cpu_park=0`
schedules unparking even with display still OFF.
It does not implement system sleep, CPU_SUSPEND/C2, CPD or SICD.

### Single-build settings, safe boot args

From `trblsg/sios` -> Actions -> `s9-installer.yml` -> Run workflow
with workflow branch `fix/s9-p7-clock-basepower-installer`:

- `kernel_ref=s9/p7-clock-basepower-oneboot`
- `metal=true`, `userdata_blocks=starlte=13917184`, `time_limit=0`
- Enter this **ONE-LINE boot_args** (sleep_cores support is allocated,
  BUT actual CPU_OFF remains disabled until runtime p7_cpu_park=1):

```text
hi=2 sleep_cores=1 mali_gov=1 mali_gov_frame=450 mali_probe=1 p3power=1 poster_probe=1 max77705_fg_s=10 max77705_diag=1 max77705_diag_s=30 glstage=1 glcost=1 p0hash=1 p0verify=1
```

Do not append the old long P6 arguments. The installer fails fast if
the paired source and exact runtime sysctls/Windows script are absent,
and validates the final bootline budget instead of truncating it.

**FULL INSTALLER CAN FORMAT USERDATA.** Back up BOOT, SYSTEM, EFS and
USERDATA off-device and verify rollback. BOOT alone lacks the new
SpringBoard runtime. Do not use a wrong SYSTEM from an earlier HI2 commit.

### Automatic single-COM experiment

From Windows PowerShell in the checkout for the new kernel branch,
**CLOSE PuTTY**, find the true COM port (e.g. COM125), keep the phone
unlocked/charged safely and the battery thermometer monitored.

Safe **one-command** P7 baseline plus Clock, NO real GPU/CPU power gates:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\s9-home-idle-serial-auto.ps1 -Port COM125 -P7Unified -Execute
```

This checks USB echo/sysctl + SpringBoard ping, enables P7 rings, runs
the complete Clock 20/20/20/20/20s causal test, then main-Home
verified policy ON 85s / OFF45s, Home secondary65s, G3D shadow65s,
Settings65s. It refuses to call a view UNKNOWN hidden and confirms
Settings foreground before accepting the phase.

**Optional hardware A/B on the SAME image** (a lab device only,
recovery verified, do not start hot):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\s9-home-idle-serial-auto.ps1 -Port COM125 -P7Unified -ActiveG3DGate -ActiveCpuPark -Execute
```

Additional changes: G3D gate3 80s with independent kernel auto-expire
at120s; an OFF-screen baseline 75s with BIG cores online versus an
OFF-screen CPU_OFF 75s confirmed by `GenericARMPE: sleep_cores`
and actual display-OFF/ON requests. USB host ACK is checked at every
transition. A failed or uncertain wake is **an incomplete test**, not
a successful optimization. On abort it attempts `hi_policy=0`,
`p7_cpu_park=0`, `g3d_lab_gate=0`, then turns observers off.
Do not manually disconnect panel hardware while powered.

No software can guarantee recovery from a firmware deadlock; the
opt-in active power phase should be run only if you have the
validated recovery/rollback that the S9 test device already uses.
Stop for unusually high battery temperature; the script has a 47C
battery-data guard but the serial stream itself can be delayed.

### USB-disconnected idle without a second flash

The always-on serial USB gadget is a major measurement confound:
P6 logged ~495 USB periodic calls/s and ~249 touch pin polls/s when
connected, which are NOT actual CPU wake-from-WFI counts.

While `p7_cpu_mode=1` and `p7_gpu_mode=1`, the two 96-sample rings
retain ~8min of history. You may physically disconnect USB for a
controlled battery-only idle interval, reconnect USB, then run the
host script in **dump-only** mode (CLOSE PuTTY):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\s9-home-idle-serial-auto.ps1 -Port COM125 -DumpP7History -Execute
```

It reads all `p7_cpu_slot=0..95` and `p7_gpu_slot=0..95` records,
without making CPU/GPU power changes, and writes `.log` plus
`.p7-report.txt`. A reconnect that fails to enumerate COM cannot
be reconstructed by software. Ring is in RAM, so reboot loses it.
A USB power disconnect changes charger conditions, so compare
carefully; also record battery SOC, thermistors, external USB meter
voltage/current when connected, ambient and elapsed time.

### Files to send

- `putty-S9-hi2-auto-YYYYMMDD-HHMMSS.log` — original raw serial.
- `*.events.txt` — exact host timestamps and ACKs.
- `*.report.txt` — earlier HI2 FPS/Clock causal parser.
- `*.p7-report.txt` — phase-wise CPU/USB/touch/PMU/PLL/G3D
  gating/CPU_OFF comparison; without measured watts or C2 claims.

The P7 reporter reads actual `p7-cpu:` and `p7-gpu:` live log
plus retained ring records after a USB-disconnected run. Driver
callbacks, G3D PLL/domain PMU, BIG CPU_OFF and requested display state
are reported separately; this DOES NOT prove DECON/DSIM or DRAM
clock gating. Those remain future instrumented items requiring
verified always-on register reads, never guessed MMIO offsets.
