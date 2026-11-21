# S9 P6 first phone log — wake source, background heat, screen-OFF failure
Date: 2026-11-21. Device: S9 Exynos9810. Source:
`C:\Users\ADMIN\Documents\GitHub\sios\putty-P6-unified-20261121-103114.log`
(5,736,868 bytes, 41,254 lines). P6 BOOT + SYSTEM already on phone.
**This is analysis of the user's original raw log; never edit its bytes.**

## Decisive OFF -> ON evidence

- BEFORE P6 phases begin: three `GenericARMPE` screen events
  at raw lines 3161 (ON), 3393 (OFF), 5203 (ON).
  Those events **do not** prove ON after the P6.4 screen-off experiment.
- P6 phase5 begins at uptime 996,714 ms, line 39306.
- The **only** side-key pair in P6.4 is
  `exynos_keys: power down` line 39507 and `exynos_keys: power up` line 39513.
  `GenericARMPE: the display off` is at line 39552.
- The subsequent `exynos_keys: a minute: presses power 1` lines
  39891, 40439, 41030 **stay at 1**; there is no second key down/up.
  There is no subsequent `GenericARMPE: the display on` in phase5.
  **The OS driver did not report any second ON key press in the log.**
  The user may not have pressed it during the short host-script prompt, or
  may have pressed and the driver missed it; the raw log cannot decide.
  The host script is NOT physically pressing the power button.
- `p5-gpu` frame/job ages increase together from ~8s to ~168s;
  the last window has 0 submitted/completed, 0 errors, domain1,
  PLL enabled/stable, GPU `clock_hint=260` MHz. No `p5-gl-resume`.
  `p6-wake` has 0 presents/touches/boost requests after OFF.
- `g3d_lab_gate=1` shadow, 36 samples of PMU domain ON
  (0/36 OFF), BUCK6 measured 950mV, no fault, wake_fail, or
  actual off-count; `shadow` reaches 33 by SAFE EXIT. It **did not**
  power gate G3D. Therefore not a measured GPU power transition failure.
- The user's SAFE EXIT ACKs on `g3d_lab_gate=0`,
  `g3d_lab_rail_probe=0`, `p6_mode=0`, `p5_mode=0`,
  `p5_threads=0`, `p4_mode=0` succeeded. No reset/voltage writes
  needed because actual gate2 was not attempted.

**Conclusion:** The display-ON timeout is justified by missing second
physical-key event *and* missing frame recovery. It is unsafe to describe
this as merely "an ON log line was dropped" or as a GPU fault.
The revised host script at commit `0c1976f` extends prompt 120s,
reminds operator, permits clearly marked FRAME_PROXY but cannot infer the
panel itself illuminated. Do NOT run G3D gate2 until ON is proven.

## P6.1: read-only source-specific rates, not actual CPU WFI wakeups

Phase2 (B1, ~120s):
- `p6-wake` 24/5s windows: LITTLE scheduler-idle avg 98.96%,
  BIG 89.42%; `presents=1201` (~10.0/s), `boost_requests=479`
  (~4/s), user touches 0.
- `p6-source` 116 valid one-second counter windows:
  USB gadget periodic callbacks 57,470 (~495.4/s),
  ALL 57,470 while USB connected; overlap 0.
  Touch fallback IRQ-pin polling 28,921 (~249.3/s),
  actual touch GIC handler invocations 0; Wi-Fi IRQ bridge 0.
- These are **actual periodic driver calls and pin checks** but cannot
  be summed into CPU wake-from-WFI counts. The USB gadget is actively
  carrying our COM log, so USB-connected results cannot be extrapolated
  to unplugged standby. Touch polling exists independent of CPU work.
- B2 phase3 (~120s): USB 59,503 / 120 windows (~495.9/s),
  touch polls 29,947 (~249.6/s); `presents=1213`,
  `boost_requests=491`; BIG idle avg 89.62%.
- B3 phase4: USB 59,454 / 120 (~495.5/s), touch 29,920 / 120
  (~249.3/s); `presents=1201`,
  `boost_requests=510`; BIG idle avg 89.54%.

The dominant on-screen background costs to prioritize are constant
~10 fps frame activity, backboardd CPU/CA, USB-connected polling during
diagnostics, and 4ms touch pin polling. Do not switch drivers to IRQ
without correctly verifying S9 GIC routing/wakeup/hardware and a new
image/bootarg test.

## P6.2 process interpretation — avoid falsely blaming a crash storm

- B2 `backboardd` avg 19.5% of one core,
  43,215 CSW per ~120s; `SpringBoard` avg 2%,
  12,170 CSW; `wifid` avg 0.9%.
- B1/B2/B3 `backboardd` CA display primary sampled
  `cpu_if_ns` avg 9.52 / 9.72 / 9.57% of one core, respectively.
- The raw reporter originally printed **239 `p6-churn`** lines
  for `CollectionsPoster`. They are **ALL FALSE churn**:
  two same-name live processes (PIDs 142 and 154) were observed
  together in the same one-second `p6-proc` snapshots, and a
  singleton `p6_last_pid[name]` alternated on each enumeration.
  No crash is implied.
- Separately, **seven actual SIGSEGV** messages in launchd for
  `com.apple.PridePoster.PridePosterExtension`, PIDs
  148,192,193,194,195,196,197, before P6 phase0 (around 17:49:10–37Z).
  This is real early/background instability, not 239 crashes.
- Offline reporter patched to show `pidChangesRaw=239`,
  `pidAliasSameSecond=239`, `pidChangesCandidate=0`,
  with one summary line instead of 239 false alerts.
  Host-only report fix, no reflash.

## P6.3 thermal shadow and P6.4 cooling

- B3 P6.3 `bigPeakMC=77000`, `gpuPeakMC=75000`,
  thermal shadow steps big=0, gpu=0; parking candidate
  17/24 window checks; no governor/voltage writes.
- The continuous `cpufreq_gov` log's BIG CPU temperatures:
  B1 avg 74.8C, B2 avg 75.3C, B3 avg 76.1C
  (one peak 81C), while screen OFF at ~1010–1175s
  BIG avg 71.5C (end 70.7C), LITTLE avg 70.3C.
- G3D TMU in phase5 falls from roughly 74–75C to 69C.
  Thermal readings are NOT a measurement of GPU/CPU electrical watts.
- All 4 LITTLE and 4 M3 CPUs remain online; when off,
  BIG scheduler-idle ~94%, LITTLE ~99%.
  No verified CPU C2/PSCI_SUSPEND, CPD, SICD.

## P6.4 / P6.5 userland observer lifetime

- `userland/rc.jobs` starts `/redwood/bin/poster_cost 5 480 &`.
  The userland code loops over a **fixed number of 480 samples**.
  During P6 mode2 with flags matching wake or CA it switches sleep
  from 5 seconds to 1 second without increasing the sample budget.
- Last `p6-source` and `poster_cost` appeared at uptime ~995s,
  just before phase5 ~997s: the process had exhausted its sample
  budget. Hence there is NO `p6-source` process census or CA
  thread sampler during phase5/phase7 on the current phone image.
  This is a **time-base/sampling design bug**, not a kernel/device crash.
- Current image can still collect kernel `p6-wake`, PMU/G3D,
  P5 GPU and possibly EAGL `p6-ca-gl` on `-ResumeCAOnly`,
  but it cannot resurrect `poster_cost` just by toggling sysctl.
  Avoid claiming complete P6.5 CA-thread attribution.
- Do not reflash only to fix this; if another boot/image is
  needed for independent reasons, correct the observer's
  wallclock run budget and same-name PID set-handling together.

## Next safe device steps without reflashing

1. Physically press the side button and confirm the panel lights
   and responds; check serial for *new* `exynos_keys: power down/up`
   then `GenericARMPE: the display on`.
2. If screen is ON and interactive, download the revised host script
   at commit 0c1976f and run `-ResumeCAOnly -Port COM125 -Execute`
   to collect only P6.5 phase7 and A2 phase8. Do not enable
   actual G3D gate2 yet.
3. To inspect cumulative driver counters manually while
   `poster_cost` is no longer running, USB console sysctl reads:
   `kern.hakios.p6_usb_ticks`, `kern.hakios.p6_usb_connected_ticks`,
   `kern.hakios.p6_touch_polls`, `kern.hakios.p6_touch_irqs`,
   `kern.hakios.p6_wifi_irqs`. Two readings spaced 30s give
   approximate per-second source rates.
4. Baseline OFF power without USB/UART instrumentation requires
   a different acquisition strategy. COM-connected USB callback
   rate is intrinsic to this observation setup, not direct proof
   of an idle-device 500Hz wakelock.

## How reproduced

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/s9-p6-report.ps1 -Log putty-P6-unified-20261121-103114.log
```

The P6 image does not need to be changed to use this revised host-side
reporter. All instrumented tests should distinguish real hardware
interrupts and key presses from proxy counters and userland logs.
