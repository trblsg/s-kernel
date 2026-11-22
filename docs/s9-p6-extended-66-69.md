# Galaxy S9 Exynos9810 — P6.6 to P6.9 extended diagnostics, ONE BOOT + SYSTEM

**Next diagnostic phase:** [P6.10–P6.12 governor state, IOSurface checksum, EAGL/scanout correlation](s9-p6-deep-610-612.md). Requires a newer matching BOOT+SYSTEM image, then independent runtime `-DeepOnly` phases 20–24.

Branch: `s9/p6-unified-wake-thermal-display-lab`, following the real
P6.1–P6.5 COM logs of 2026-11-21. These changes are **observational only**.
No new boot-args; runtime sysctls control each phase. They deliberately
**do not** skip any GL draw or frame, enforce a CoreAnimation dirty policy,
change DVFS, modify GPU voltages, alter interrupt routing, touch/key/USB
polling cadence, or enable actual G3D power gating.

## Why this is worth a new, single build

Real device log `putty-P6-unified-20261121-111237.log` showed ~10 EAGL
frames/s with 22.7 draw calls/frame, mean EAGL 6.7ms/frame, ~20% CPU in
backboardd, CA display thread 9.73%, ~2% GPU busy yet the domain stays ON.
Only two *observed input IOSurface* changes appeared in 1500 frames. This
does **not** prove the final output framebuffer was unchanged. The earlier
P6.4 OFF log also lacked a second detected physical power key press.
The new diagnostics discriminate these possibilities instead of guessing
that a particular clock/rail/animation is responsible.

## Interfaces and evidence

| Workstream | Switch | Measurement/trace prefix | What it cannot establish |
|---|---|---|---|
| **P6.6** CA frame/target provenance | `kern.hakios.p66_mode=1` | `p66-ca:` per 60 frames; `p66: armed` on phase change. Target IOSurface identity, number of target surfaces in frame, stable vs switched target, draws, input CRC checks/changed/skipped, candidate runs, no-present/no-draw | No proof of identical final framebuffer pixels; no detailed CA layer invalidation/call stack. A candidate never means safe to skip |
| **P6.7** CPU-side GL slot costs | `kern.hakios.p67_mode=1` | `p67-cost:` per 60 EAGL frames, `p67-path:` sample 1-in-16 binds and draw-elements calls. Inclusive slot wall time, top slot, native vs lookup/check/upload timing | Does not isolate GPU execution time; GL nested timings are inclusive and may overlap; samples have tracing overhead |
| **P6.8** G3D OFF/quiet comparison | `kern.hakios.p68_mode=1` | `p68-g3d:` on existing 5s G3D PMU worker: domain, PLL, mux, awake hint, clock *hint*, last frame/job ages, 10s quiet eligibility, OFF observed, faults | Does not power-gate GPU, measure watts, read G3D register when off, or guarantee actual C-state residency |
| **P6.9** periodic callback attribution | `kern.hakios.p69_mode=1` | `p69-source:` on existing userland sampler: USB disconnected/stopped, touch GPIO-low/read rounds, key GPIO poll and debounced power transitions. Combine with `p6-source:` USB/touch/Wi-Fi counters | Not all platform IRQs/callouts; not the number of times a CPU exits WFI, CPD or SICD |

All four mode values are **0 OFF by default**. Independent sources are
still available as read-only cumulative sysctls; each mode only enables
additional aggregation, timing or log output. The P6.8 sampler reuses the
existing G3D monitor thread rather than scheduling another thread.

### P6.6 decision boundaries

A `p66-ca` **candidate** requires one identified output target, at least
one draw, at least one **checked** IOSurface input, zero observed changed
inputs, zero skipped input checks and zero input uploads. A frame with no
check is **unknown**, not unchanged. The target is identified by
`IOSurfaceGetID` *before* `target_to_surface` clears the `drawn` bit.
A stable target ID is the *address/identity*, not an output hash. Even
with all checks apparently unchanged, shader uniforms, framebuffer fetch,
procedural effects, other layer content, display overlays, present timing
or an untracked input can still alter pixels. No skipping is implemented.

`p66-ca` also exposes `multi_target`, `target_unknown`,
`stable_target`, `switched_target`, `unchecked_frames`,
`input_candidates`, `max_streak`, `not_presented` and
`long_gap`. Use these to decide what to instrument next before any
future behavioral change.

### P6.7 wall-time meaning

`p67-cost` uses the existing GL entry/exit timing around the EAGL slot
call. `draw_us`, `texture_us`, `target_us` etc are **inclusive**
CPU-thread wall time; adding all categories does not necessarily equal
unique thread CPU time. `p67-path` samples native glDrawElements,
GL bindings and IOSurface lookup/check/upload timing at **1/16 rate**
(no additional GL calls or hash passes). Complement with existing
`hakios-gl-stage`, `hakios-gl-cost-top` and `p6-ca-thread`.
The new timing overhead itself must be A/B checked versus observers off.

### P6.8 idle semantics and OFF safety

`quiet10s=1` only if **both** frame and job timestamps exist and are
at least 10 seconds old. `could_shadow=1` means that condition was
met while **PMU domain still ON**, not that the GPU may safely power off
under every policy. `actual_off=1` is a PMU observation, not the
result of toggling a new power control. A constant 10 FPS will normally
keep `quiet10s=0`; optional physical screen OFF tests establish a
separate sample. `g3d_lab_gate=1` is the old **shadow** behavior,
never the actual gate2/3 path. GPU/domain observations are 5s snapshots
and cannot see rapid intersample transitions.

### P6.9 source counter mapping

Read-only cumulative kernel sysctls newly added:

```text
kern.hakios.p69_usb_disconnected_ticks
kern.hakios.p69_usb_link_stopped_ticks
kern.hakios.p69_touch_pin_low
kern.hakios.p69_touch_event_reads
kern.hakios.p69_key_polls
kern.hakios.p69_key_power_down
kern.hakios.p69_key_power_up
kern.hakios.p69_key_held_polls
```

They supplement prior `p6_usb_ticks`, `p6_usb_connected_ticks`,
`p6_usb_overlap_ticks`, `p6_touch_irqs`, `p6_touch_polls`,
`p6_wifi_irqs`. Bit positions 0..7 of `p69-source`
`present_mask`/`delta_valid` correspond in the order listed above.
The first sample in each phase and any missing/regressed counter are
**invalid**, not zero; `span_s` allows per-second normalization.
Key polling is the existing 10ms GPIO DAT loop; the real debounced
key down/up counters are separate. A USB-connected COM terminal
normally causes about 495 driver callbacks/s; this does not describe
disconnected phone idle. Touch fallback polls about 249/s.
No poll delays, GPIO mode, GIC routing or button semantics change.

## Single matching image then several runtime phases

P6.6/P6.7 run in `userland/src/eagl.c` and require new userland
`s9_p66_frame_bucket.h`; P6.9 source aggregation and fixed sample
lifespan run in `userland/src/poster_cost.c`. Kernel requires
`hakios.p6_lab.h`, Mali G3D monitor, exynos_keys, DWC3 and S6SY761.
**Rebuild matching BOOT + SYSTEM once**, then all phase toggles require
no further flash. Bootargs keep known S9 working graphics settings
(e.g. `poster_probe=1 mali_gov=1 mali_dvs=0`). Do not flash using an
old SYSTEM: missing `p66-ca`/`p67-cost`/`p69-source` indicates
stale userland. Old P6 images lack the new kernel sysctls and cannot
run this experiment. An installer can FORMAT USERDATA — preserve
off-device backups and known-good BOOT/SYSTEM/EFS/USERDATA rollback.

P6.2 also avoids its previous false `CollectionsPoster` PID churn:
a *bounded* set of concurrently live PIDs per executable name is
compared across complete census iterations. A PID replacement is still
not evidence of SIGSEGV without launchd signal records. Overflow
(`p6-identity-overflow`) makes that target unmeasurable rather than
silently inventing a restart. The `poster_cost 5 480` process now uses
at least **4 hours of wall-clock budget**, not 480 loop iterations:
raising the sampling frequency to 1Hz can no longer exhaust it at
~995 seconds, as happened in the previous P6 log. The sampler still
runs only with explicit `poster_probe=1` at boot.

### Host serial COM capture (Windows PowerShell 5.1)

Preview new phases without opening COM or changing the phone:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/s9-p6-serial-auto.ps1 -ExtendedOnly
powershell -NoProfile -ExecutionPolicy Bypass -File tools/s9-p6-serial-auto.ps1 -ExtendedOnly -PreflightOnly
```

Run once the matching BOOT+SYSTEM is booted, screen visibly ON,
and **PuTTY closed** (exclusive COM):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/s9-p6-serial-auto.ps1 -Port COM125 -ExtendedOnly -Execute
```

Script checks four new sysctl nodes with exact echo + ACK before
starting phases. Default uses **screen ON throughout**:

| Marker | Time (default) | Experiment |
|---|---:|---|
| 10 | 60s | A1 all observers OFF, clean thermal baseline |
| 11 | 120s | P6.6 target/damage **observation**, P5 + CA support |
| 12 | 120s | P6.7 EAGL CPU slot timing/path samples |
| 13 | 120s | P6.8 G3D PMU/PLL/quiet **shadow gate1**, no real gate |
| 14 | optional 110s OFF + wake | **Only** when `-ExtendedScreenOff` is supplied, physical power button required; failure aborts into safe exit |
| 15 | 120s | P6.9 USB/touch/key source counts at 1Hz |
| 16 | 60s | A2 all observers OFF, verify overhead |
| 9 | cleanup | exact ACK for observer reset, existing GPU gate reset to 0 |

No new build is needed to repeat phases or change duration; use
`-PhaseSeconds N`, `-ScreenOffSeconds N`, or manually toggle
`kern.hakios.p66_mode` / `p67_mode` / `p68_mode` / `p69_mode`.
For the optional **read-only** screen-OFF comparison:
```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/s9-p6-serial-auto.ps1 -ExtendedOnly -ExtendedScreenOff -Port COM125 -Execute
```
Because P6.4 previously missed a second physical side-key event,
leave this OFF experiment disabled initially. The host cannot press the
button on your behalf. A missing ON marker plus no restored frame/job
activity is a **failed screen-wake test**, not permission to power-gate
G3D. Never use `-EnableActualG3D` with `-ExtendedOnly` — the script
explicitly refuses that combination.

### Offline report

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/s9-p6-extended-report.ps1 -Log putty-P6-unified-YYYYMMDD-HHMMSS.log
```

The reporter distinguishes checked/unchecked inputs, candidate
streaks, CPU GL slot time, native-vs-wrapper timed samples, quiet
GPU while still powered, and real driver callback/keypress counts.
It groups by phase and shows source validity. Existing
`tools/s9-p6-report.ps1` remains useful for P6.1–P6.5/PridePoster
SIGSEGV and userland process census. Raw bytes are not altered.
Synthetic tests use `tools/tests/p6-extended-synthetic.log` and
`tools/tests/test_s9_p66_frame_bucket.c` (C11/C++17 UBSan).

## Test/rollback boundaries

These are **source instrumentation changes only**, NOT an optimization
that changes scheduling, render output, voltage, thermal caps, GPU
power or USB/touch polling. They can increase CPU log/measurement
overhead when enabled. Never infer savings from one unpaired run;
compare phase10 and phase16 at equivalent thermal state. A GPU domain
ON under 10FPS plus low GPU busy is a strong opportunity to investigate
CA cadence, but *not* proof that a power gate would be safe between frames.

For abort:
```text
sysctl kern.hakios.p66_mode=0
sysctl kern.hakios.p67_mode=0
sysctl kern.hakios.p68_mode=0
sysctl kern.hakios.p69_mode=0
sysctl kern.hakios.g3d_lab_gate=0
sysctl kern.hakios.g3d_lab_phase=0
sysctl kern.hakios.p6_mode=0
sysctl kern.hakios.p5_mode=0
sysctl kern.hakios.p5_threads=0
sysctl kern.hakios.p4_mode=0
```
Exact `set (0)` responses are mandatory to claim cleanup. If actual
gate2 or an undervolt experiment was enabled **separately**, this
read-only procedure is not a substitute for its documented hardware
recovery and verified 950mV reset.
