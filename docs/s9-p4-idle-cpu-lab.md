# S9 P4 Idle Compositor & CPU Residency Lab

Branch: `s9/p4-idle-compositor-cpu-lab`, based on `s9/g3d-power-lab` at `5bd82cf`. P0, P1, P3 and G3D Power Lab retained. P2 still deferred.

**One kernel + matching SYSTEM build; many diagnostic phases through sysctl.** This version does NOT skip frames, alter GL synchronization, force WFI/CPD/SICD/CPU_SUSPEND, or relax CPU/GPU thermal protections.

## Why P4 exists

The real-device 900mV S9 log between uptime 450–720s (no touches, screen ON) has 2700 GL frames in 270sec (=10fps), 61,446 draw calls, 99,336 texture-*category* GL operations and backboardd using ~19.4% of one core (~75–80% of all idle CPU). Out of 235 **sampled** IOSurface checksum checks, 234 were unchanged. This is not proof that framebuffer pixels or all unchecked surfaces are unchanged. Panel scanout ~60Hz is separate from 10fps compositor work.

## Runtime sysctls

All under `kern.hakios.`, no P4 boot arguments needed:

| Sysctl | Values | What it does |
| --- | --- | --- |
| `p4_mode` | 0 baseline (default), 1 read-only, 2 shadow | Mode2 **classifies** potential repeat frames; it never omits, delays, reorders or reuses them |
| `p4_threads` | 0 off (default), 1 backboardd, 2 backboardd+SpringBoard+PosterBoard | Best-effort per-thread proc_pidinfo telemetry in existing poster_cost sampler |
| `p4_phase` | 0–99 (default0) | Uptime phase marker; resets CPU-delta baseline, no hardware action |
| `p4_log_ms` | 1000–30000 (default5000) | Interval of CPU-idle probe **inside existing cpufreq governor worker**, no new kernel thread |
| `p4_stats` | read-only | Last CPU scheduler-idle and frame/touch data, valid/offline masks, phase |

P4 mode is disabled by default at boot; on the kernel path, each frame/touch only gains a disabled branch check.

### CPU source and important limitations

`drivers/power/exynos/hakios.exynos_cpufreq.c` already uses the XNU `ml_cpu_idle_time()` scheduler counter. P4 samples eight CPUs approximately once every 5s when enabled; `p4-cpu:` logs LITTLE/BIG scheduler-idle %, valid-core mask, removed-core mask, cluster MHz, display **present notification** counts, touch counts and event gaps (<17,17–50,50–120,>=120ms). `p4-core:` lists each CPU's idle percentage; **255 means unavailable**, not 255% idle.

These are NOT direct power-state counts. Actual architectural WFI, Exynos CPD/SICD, CPU_SUSPEND and electrical watts are explicitly **unmeasured**. No sleeping-domain registers are accessed and no power state changes.

### GLES frame evidence (matching APFS SYSTEM userland REQUIRED)

`eagl.c` polls P4 sysctls no more often than once each 60 frames per context, with no new GL calls or expensive framebuffer hash. Every 60 completed GL frames it prints:

- `p4-gl:` GL calls/draws/clears/uploads, IOSurface actually-checked/unchanged/changed/skipped counts, checked/unchecked frames, frames with no draws, shadow candidate frames.
- `p4-gl-gap:` completed-frame gaps, calls ending a frame (`glFlush` / `glFinish` / `glFinishObjectAPPLE`), host presents, copy/in-place counts, CPU elapsed time.

**Candidate** means some *checked texture inputs* matched without observed change/upload. It does NOT prove identical final pixels: unscanned surfaces, procedural animation, other buffers and fences may still differ. The GL call ending a frame is not the original reason a CoreAnimation vsync/timer callback fired. P4 does not implement frame-skipping or modify P2.

### Per-thread CPU telemetry

With pre-existing boot arg `poster_probe=1`, `poster_cost` remains alive for up to **40 minutes**, checking PIDs every five seconds. `p4_threads=1/2` enables `PROC_PIDLISTTHREADS`/`PROC_PIDTHREADINFO` enumeration, printing up to six hottest threads per target with id, thread name, raw user/system counter deltas, run-state and sleep-time hint. To avoid misleading estimates before calibrating the iOS libproc timebase, it explicitly prints **both** CPU-% interpretations (`cpu_if_mach` and `cpu_if_ns`). A failed API call prints `unavailable` or `missing`, never a fabricated zero. Context switches remain counted at PROCESS level by poster_cost; the new data do not claim per-thread wakeup counts or deep WFI residency.

## One full installer / one capture

The P4 kernel and userland changes are both required for the complete experiment. A **BOOT-only flash will not deploy p4-gl or p4-thread telemetry**. To obtain all layers, build with SIOS s9-installer workflow ref `fix/s9-installer-device-selection` and `kernel_ref=s9/p4-idle-compositor-cpu-lab`; keep `metal=true`, `userdata_blocks=starlte=13917184`, `time_limit=0` and the last known working bootargs. **A full installer FORMATS USERDATA**. Verify off-device BOOT/SYSTEM/EFS/USERDATA backups and rollback before any flash. Do not assume it is safe merely because the code compiles.

Boot arguments continue to include `poster_probe=1 glstage=1 glcost=1 glpath=1 glhash=1 p0hash=1 p0verify=1 mali_gov=1 mali_gov_frame=450 mali_max=598 mali_dvs=0 mali_probe=1 p3power=1` along with the previous P1 flags. No p4_* argument is necessary. Hold G3D Lab at `g3d_lab_gate=0` and stable 950mV for baseline comparisons.

Collect PuTTY **All session output** from boot. For consistent comparisons: fixed brightness~70, charger, Wi-Fi, wallpaper and ambient; no touches except deliberate display-off/on transitions. Wait until uptime >=420sec after initial Poster storm. Once after each sysctl transition allow ~10sec for kernel baseline and userland 60-frame polling; exclude those seconds from A/B.

**A1: baseline 120sec, diagnostics OFF**

    sysctl kern.hakios.p4_mode=0
    sysctl kern.hakios.p4_threads=0
    sysctl kern.hakios.p4_phase=1

Legacy glstage/cputop/poster_cost continue with no P4 sampling.

**B1: passive P4 180sec**

    sysctl kern.hakios.p4_threads=1
    sysctl kern.hakios.p4_mode=1
    sysctl kern.hakios.p4_phase=2
    sysctl kern.hakios.p4_stats

Observe `p4-cpu`, `p4-core`, `p4-gl`, `p4-gl-gap`, `p4-threads`, `p4-thread` plus backboardd and thermal logs.

**A2: return to baseline 120sec**

    sysctl kern.hakios.p4_mode=0
    sysctl kern.hakios.p4_threads=0
    sysctl kern.hakios.p4_phase=3

This tests P4 measurement overhead with A/B/A rather than comparing only across different boots.

**B2: shadow classifications 180sec**

    sysctl kern.hakios.p4_threads=2
    sysctl kern.hakios.p4_mode=2
    sysctl kern.hakios.p4_phase=4

Rendering, synchronization and FPS remain UNCHANGED. Correlate candidates with top thread CPU deltas.

**Optional screen-OFF contrast, 90–120sec each**

    sysctl kern.hakios.p4_threads=1
    sysctl kern.hakios.p4_mode=1
    sysctl kern.hakios.p4_phase=5

Turn screen OFF for 90–120sec, then screen ON and execute:

    sysctl kern.hakios.p4_phase=6

Wait 90–120sec after display ON. Do not infer thermal/watt savings from screen-OFF comparison because the panel itself also changes power.

**Exit**

    sysctl kern.hakios.p4_mode=0
    sysctl kern.hakios.p4_threads=0
    sysctl kern.hakios.p4_phase=7
    sysctl kern.hakios.p4_stats

Save `putty-P4-idle-CPU-lab.log` and run the read-only analyzer `tools/s9-p4-idle-report.ps1 -Log <path>`.

## Interpreting failures and stopping

- `p4-cpu` present but `p4-gl` absent: userland GLES library may not be updated, the renderer path might differ, or no 60-frame window finished. Kernel-only BOOT is not sufficient for P4 userland.
- `p4-thread` absent: inspect `p4-threads` for libproc errors or empty/truncated/ABI-mismatched results; that is not proof no CPU thread ran.
- `p4-core idle_pct=255`: unavailable counter/offline core/phase warmup; never count it as valid idle or deep sleep.
- If GPU/CPU or display becomes unstable, stop sampling with `p4_mode=0` and `p4_threads=0`; restore previous matching image if needed.
- Never force CPU_SUSPEND, CPD/SICD, enable frame skipping, or weaken thermal caps based solely on these statistics.

Goal of the next analysis: identify which backboardd threads burn CPU during repeating compositor frames, distinguish checked vs skipped IOSurface inputs, measure actual scheduler-idle opportunities, and only then design a bounded opt-in optimization. Energy savings in watts remain unmeasured.
