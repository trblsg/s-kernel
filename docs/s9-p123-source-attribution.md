# S9 P12.3 source attribution and adaptive GPU frame floor

P12.3 is a paired BOOT + SYSTEM experiment on the P12.2 baseline. It
uses the same `p122=1` flag and profile controls (0/1/2). No new boot
argument is required. Build both halves from the **same pinned commit**.
Do not flash merely to change a host option. Back up data before using a
formatting installer.

## Safety and scope

- `hi=2 p122=1` now starts passive: no autonomous Clock cut at boot.
  Only an explicit host Clock crossover re-arms the legacy guarded
  exact-target 20s A/B/A/B/A. This is NOT a permanent timer optimization.
- The Clock owner classifier still fails open on UNKNOWN. P12.3 logs
  runtime class identity (pointer equality, class image, method argument
  count and return encoding), not guessed private offsets. A class name
  alone never authorizes callback suppression.
- `p123-clock-window`, `p123-ca-window`, `p123-gl-window` and
  `p123_floor_stats` use mach absolute ticks. They are aggregate
  observation windows, **not cross-process frame IDs or exact input
  latency**. Clock intervention is the separate causal experiment.
- Existing G2 bookkeeping records bind-cache candidates in all profiles.
  No bind/draw/flush is skipped; `g2_bind=1` must not be used. Mode 2
  adds sampled native/lookup/check/upload and draw-native/other wall
  time. Existing GL stage and slot timings overlap; do not sum them.
  Sample residue rotates across 16-frame blocks to reduce phase aliasing.
- No new CPU_OFF, C2/CPD/SICD, panel DCS, PMIC or fixed GPU clock writes.
  WFI observation is not deep hardware idle proof.
- Adaptive trial `kern.hakios.p123_floor_mode=1` is disabled by default,
  requires p122, running Mali governor, exactly three levels,
  baseline floor450, no DVS, screen ON, known input quiet >=3s.
  Absolute 30s deadline cannot be extended while active. Relax only
  presentation floor when >=6 recent gaps 80–150ms, G3D load <=8%,
  current TMU <72C, display ON and known input quiet. The load-based
  governor, down hysteresis, stock upper limits and thermal cap remain.
  Real input, display loss, invalid/hot TMU or baseline changes revoke
  the trial. Fast/irregular frames keep the original floor. Mode 0
  restores the normal policy; this is not an instantaneous clock write.
- Host early thermal stops BIG76 LITTLE70 G3D72; existing hard
  BIG82 LITTLE75 G3D77 BAT47 unchanged. No repeated gestures.

## Boot inputs

```text
hi=2 panel=1 p122=1 p12_gpu_boot_auto=1 mali_gov=1 mali_gov_frame=450 mali_probe=1 p3power=1
```

Panel driver initialized, DCS sleep disabled. No sleep_cores, mali_dvs,
glcost/glstage/glpath/p0hash/p0verify/g2_bind or heavyweight GL tracing.
The existing physical GPU OFF policy remains guarded by its P9/P12 logic.

## Windows capture, separate runs on one firmware

Use the three matching scripts in one directory:
`s9-p122-rootcause-serial.ps1`, `s9-p12-dual-serial.ps1`,
`s9-screen-lifecycle-serial.ps1`. Close PuTTY. Replace COM125 as needed.
Unlock to Home, wait for boot tasks to settle, cool before each run. Run
without `-Execute` first for preview. Execute these separately, not all
at once. A thermal stop is an invalid/incomplete phase, not a reason to
raise limits. For no-touch tests, do not touch the screen once started.

```powershell
# Source & GL cost profiling only (4 x 20s; no timer or GPU policy change)
powershell -NoProfile -ExecutionPolicy Bypass -File .\s9-p122-rootcause-serial.ps1 -P123 -Port COM125 -Execute

# Independent GPU floor A/B/A (12s per phase)
powershell -NoProfile -ExecutionPolicy Bypass -File .\s9-p122-rootcause-serial.ps1 -P123 -AdaptiveFloor -WindowSeconds 12 -Port COM125 -Execute

# Independent exact Clock causal crossover (legacy 20s A/B/A/B/A)
powershell -NoProfile -ExecutionPolicy Bypass -File .\s9-p122-rootcause-serial.ps1 -P123 -ClockCrossover -Port COM125 -Execute

# ON WFI sample then physical screen OFF, optional manual wake after verified OFF
powershell -NoProfile -ExecutionPolicy Bypass -File .\s9-p122-rootcause-serial.ps1 -P123 -IdleOnly -WfiProbe -IdleWindowSeconds 120 -Port COM125 -Execute

# Last, only after a fresh cool start: ONE gentle swipe and 8s recovery
powershell -NoProfile -ExecutionPolicy Bypass -File .\s9-p122-rootcause-serial.ps1 -P123 -MicroGesture -Port COM125 -Execute
```

Add `-WakeCheck` to the idle command only for a deliberately verified
manual wake (>=60s physical OFF and actual new PMU/PLL OFF). Never use
`-LegacyFirmware`. If a floor trial is aborted, host attempts immediate
restore even while hot; the kernel deadline is a second safety fence.

Output: raw .log, .events.txt, .samples.csv, .p123-report.txt,
.p122-report.txt, .p12-report.txt, .lifecycle-report.txt. For A/B,
compare same scene, frame cadence, GPU present count, floor-forced and
frequency residency, jobs and queue/run, frame-wall histogram, temperature
trajectory and recovery. If source evidence or eligibility is missing,
do not claim an improvement. No physical energy measurement is included.
