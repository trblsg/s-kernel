# S9 P5.1 + P5.2 + P5.3 — Display/Wake/GPU Attribution Lab

Branch: `s9/p5-display-wake-gpu-lab`, based on P4 HEAD `6e722a0`. P0 checksum, P1/P3, P4 CPU idle, G3D Power Lab and all thermal caps remain intact. P5 has **NO frame drop, forced 5fps, automatic power gating, PMIC/PLL writes, or CPU suspend**.

P4 actual-device evidence: when Home Screen is stationary, backboardd ~19% of a CPU, `com.apple.coreanimation.display.primary` ~9.6%, ~10 GLES frames/s, ~60 Mali job chains/s at 455MHz, GPU busy~1.7%. When display OFF, render frames cease and backboardd ~4.6%. When display ON, the first seconds hit ~59fps, up to 1,991 job chains/s, GPU busy48% and BIG87°C with thermal throttling. This is the reason for P5.

## One build, all experiments via sysctl

All under `kern.hakios.`. **New P5 sysctls are not boot arguments** and default to OFF:

| sysctl | Values | Function |
| --- | --- | --- |
| `p5_mode` | 0(default),1(passive),2(bounded burst) | 5s PMU/CMU+GPU owner samples, 60-frame GLES summaries; mode2 adds 1 detailed burst line per60 frames and faster per-thread sampling if enabled |
| `p5_phase` | 0..99 | Time marker only, no power or display change |
| `p5_threads` | 0(off),1(backboardd),2(+SpringBoard,PosterBoard) | Per-thread CPU, policy, priority, state via existing `poster_cost`, requires `poster_probe=1` boot arg |
| `p5_stats` | read-only | Last completed 5s GPU snapshot |

Existing `p4_mode`, `p4_threads`, `p4_phase`, `p4_log_ms`, `p4_stats` work independently. Retain `g3d_lab_gate=0`, `g3d_lab_mv=950`, `mali_dvs=0` during P5 experiments.

### P5.1: display thread and frame-end proxy

`p5-display-thread` samples the `com.apple.coreanimation.display.primary` thread **even when its CPU delta is 0**, if it exists and libproc returns its data. Includes thread ID, raw user/system time (observed nanosecond-compatible on actual S9), CPU%, run state, policy and priority. `p5-thread` prints top six threads; `p5-thread-state` counts states at each sample (NOT wakeup events).

In mode1 `poster_cost` retains 5s samples; mode2 with threads enabled uses 1s samples to capture brief wake bursts. Both are passive libproc reads, without task suspend, changing QoS or thread scheduling.

`p5-gl` summarizes once per60 GLES frames: draws, clears, uploads, draw-target callbacks classified by size >=1000x2000 (large target heuristic, **not proven panel dirty**), IOSurface inputs actually sampled/same/changed/skipped, unchecked frames, gap histograms, and glFlush/glFinish/finishObject end reasons. `p5-gl-resume` detects a >=2-second gap in the *same GLES context*; it is not itself proof of panel power ON. Its timestamp can be compared with `GenericARMPE: the display on`.

**Limit:** P5 does NOT have access to the private CoreAnimation timer/vsync callback implementation. A glFlush frame-end event is NOT the origin of the timer that woke display.primary. Sparse IOSurface checks also cannot prove identical framebuffer pixels. P5 NEVER skips a frame.

### P5.2: the wake burst

In mode2, `p5-gl-burst` emits a single bounded line per60 frames containing fastest frame gap, largest draw/upload/changed sample, top draw frame. This yields detail at ~1 line/s during 60fps wake, not 60 lines/s. `p5-display-thread` in mode2 samples every second; P5 GPU reports rolling-ish maximum one-second job count per5s. These can be correlated with `fps`, P4 CPU-idle, and existing TMU/thermal logs.

### P5.3: GPU client attribution and OFF-state power sampling

The Mali queue already labels each submitted chain with `owner`, a unique **HakiosMaliUserClient number, NOT a PID or application name**. Under the existing queue lock P5 counts enqueue/complete events, fragment vs vertex/tiler jobs, queue depth, queue wait, worker completion time, dropped/errors. Results include:

- `p5-gpu` every ~5s: PMU_G3D_STATUS, awake hint, G3D PLL and mux, frame/job ages, job counts, peak 1s jobs, errors, owners, max queue depth, average worker wait/run.
- `p5-owner`: up to 12 highest-volume client owner IDs with fragment/vertex counts, queue delays, run duration, failed/dropped counts.

`worker_run_us` covers CPU/kernel/GPU completion wait and is NOT pure shader time. `peak_jobs_1s` is approximate when a 1s burst spans two buckets. These are real driver client IDs, but attribution to backboardd vs SpringBoard requires additional verified task→PID linkage.

The probe **reuses the existing independent G3D Power Lab 5s kernel thread** and samples G3D PMU/CMU even when the Mali governor stops on display OFF. PMU/CMU are outside GPU power domain; it does NOT read JOB/MMU/GPU core registers or wake the GPU by MMIO. If the independent thread itself prevents deeper idle, that is a measurement overhead — compare baseline vs P5-on.

CPU CPD, SICD, CPU_SUSPEND, hardware WFI residency and measured watts remain **unmeasured**.

## Single-build SIOS installer (critical)

The P5 kernel **and matching userland SYSTEM/APFS** are both required. BOOT-only update gets `p5-gpu` but not P5 GLES or process/thread lines.

In `trblsg/sios` → Actions → `s9-installer.yml`:
- workflow branch: `fix/s9-installer-device-selection`
- `kernel_ref=s9/p5-display-wake-gpu-lab`
- `metal=true`
- `userdata_blocks=starlte=13917184`
- `time_limit=0`.

**The full installer formats USERDATA.** Verify off-device backups of BOOT, SYSTEM, EFS and USERDATA and a working recovery/rollback method. Do not flash solely to toggle P5 after installing the same kernel.

Keep proven boot flags, with `mali_gov_frame=450` FIRST to avoid the previously observed truncation:

    mali_gov_frame=450 mali_gov=1 mali_max=598 mali_dvs=0 mali_probe=1 p3power=1 p3rail=0
    poster_probe=1 p0hash=1 p0verify=1 glstage=1 glcost=1 glpath=1 glhash=1
    p1_userdict=1 p11_trace=1 p12_diag=1 p1_mps_diag=1 metal_layout=0
    poster_retry=2 widget_nice=0

Enter these as **one single line** in the workflow. No `p5_*` boot arguments are needed. Confirm actual `mali-power: framefloor=450MHz clock=455MHz` after boot.

## Recommended same-boot PuTTY sequence

Capture all output from boot, fixed 70 brightness, wallpaper, network, charge state, no touch except deliberate display power transition; wait >=420 seconds uptime for Poster to settle.

**A1 — 120 seconds, display ON, P5 OFF (baseline)**

    sysctl kern.hakios.p4_mode=0
    sysctl kern.hakios.p4_threads=0
    sysctl kern.hakios.p5_threads=0
    sysctl kern.hakios.p5_mode=0
    sysctl kern.hakios.p5_phase=1

**B1 — 180 seconds, passive P5 (and P4 core idle) ON**

    sysctl kern.hakios.p5_threads=1
    sysctl kern.hakios.p5_mode=1
    sysctl kern.hakios.p4_mode=1
    sysctl kern.hakios.p5_phase=2
    sysctl kern.hakios.p5_stats

Exclude first 10s after phase change; the EAGL sysctl is read once per60 frames, ~6s at idle.

**A2 — 120 seconds, return P5/P4 OFF (measure profiler overhead)**

    sysctl kern.hakios.p5_mode=0
    sysctl kern.hakios.p5_threads=0
    sysctl kern.hakios.p4_mode=0
    sysctl kern.hakios.p5_phase=3

**B2 — 120 seconds, bounded wake details, display still ON**

    sysctl kern.hakios.p5_threads=1
    sysctl kern.hakios.p5_mode=2
    sysctl kern.hakios.p4_mode=1
    sysctl kern.hakios.p5_phase=4

Mode2 costs more CPU due to 1-second libproc sampling: use A1/B1/A2 to quantify it.

**C1 — screen OFF/ON, P5 stays mode2 to capture the first seconds**

    sysctl kern.hakios.p5_phase=5

Turn screen physically OFF for 90–120s. Confirm `GenericARMPE: the display off`; P5 still samples PMU every5s. Turn screen physically ON and DO NOT touch until wake animation settles. The `p5-gl-resume`, `p5-gl-burst`, `p5-gpu` and `p5-display-thread` lines capture the transition. When settled:

    sysctl kern.hakios.p5_phase=6
    sysctl kern.hakios.p5_stats

Keep screen ON for another 2 minutes. Then:

    sysctl kern.hakios.p5_mode=0
    sysctl kern.hakios.p5_threads=0
    sysctl kern.hakios.p4_mode=0
    sysctl kern.hakios.p5_phase=7
    sysctl kern.hakios.p5_stats

Save `putty-P5-display-wake-gpu.log`. Analyze without editing it using:

    powershell -ExecutionPolicy Bypass -File tools/s9-p5-report.ps1 -Log putty-P5-display-wake-gpu.log

## Interpretation and fail-safe

- Missing `p5-gl` but present `p5-gpu`: likely no matching P5 userland, EAGL not executing, or fewer than60 frames finished. BOOT-only is insufficient.
- Missing `p5-display-thread`: could be thread enumeration failure, missing name, no previous sample. Look at `p4-threads` and `p5-thread-state` first.
- `p5-gpu domain=1` while physically OFF means G3D still powered at sample time. `domain=0` proves PMU OFF; no access to GPU MMIO should occur. Neither is a watt measurement.
- If P5 shows an unexpected spike in CPU from monitoring, revert to `p5_mode=0 p5_threads=0` and compare A/B.
- Do NOT disable thermal protections, force CPD/SICD, skip GL frames or combine GPU undervolting and gating based on P5 shadow data.
- If display stalls, graphics glitches, temperature runs away or the OS becomes unstable, disable diagnostics and use your previous matching BOOT/SYSTEM rollback.

P5 implements observation only. A production optimization awaits a verified frame dirty/fence/timer cause and measured on-device safety.
