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

### Recommended: direct USB CDC-ACM script on COM125, with verified acknowledgments

**2026-11-21 actual-device finding:** our PuTTY GUI keystroke script caused the loader to receive truncated lines such as `usb> sctl kn.haki.p5_phe=0`, followed by `usb: sctl: not a command (help)`. The problem persisted after per-key pacing and double Enter. These responses are emitted from `console()` in `drivers/usb/hakios.dwc3.c` after `usbacm_line_feed()` has collected the received USB bytes; they are NOT just PuTTY visual display corruption. The line parser accepts one CR or LF; double Enter cannot replace dropped letters.

**Use `tools/s9-p5-serial-auto.ps1` instead. This script replaces PuTTY during the measurement. PuTTY MUST be fully closed/disconnected before starting it** because Windows COM port handles are exclusive. It opens COM125 as USB CDC ACM at the documented 115200 8N1, no handshake, DTR on; sends each complete ASCII command plus a single CR in one `SerialPort.Write()` call, reads the actual loader stream, and only moves on after BOTH the exact `usb> sysctl ...` echo and the corresponding confirmed `usb: sysctl ...: set (0)` or readback. It logs every raw received byte to a **new timestamped file**, so no data needs to be captured in PuTTY. It does not require re-flashing the already installed P5 BOOT+SYSTEM.

Open local Windows PowerShell 5.1 (on the PC attached to COM125):

```powershell
# Preview without opening COM125
powershell -NoProfile -ExecutionPolicy Bypass -File tools/s9-p5-serial-auto.ps1 -Port COM125

# Safest first device check: ONLY p5_stats read and p5_phase=0 marker, with positive echo+ACK verification
# First CLOSE PuTTY, then run:
powershell -NoProfile -ExecutionPolicy Bypass -File tools/s9-p5-serial-auto.ps1 -Port COM125 -Execute -PreflightOnly

# Once preflight passes, run the full A1/B1/A2/B2/OFF/ON experiment:
powershell -NoProfile -ExecutionPolicy Bypass -File tools/s9-p5-serial-auto.ps1 -Port COM125 -Execute
```

During the full run, physically turn the phone's screen OFF and ON when the script says so; it watches for actual `GenericARMPE: the display off/on` lines and refuses to advance without them. Run when uptime >=420s and the Home Screen is settled; brightness, Wi-Fi and charging conditions fixed. The P5 sysctls and A/B waits are unchanged from the manual table below. On failure after diagnostics have started it attempts to restore `p5_mode=0`, `p5_threads=0`, `p4_mode=0`, but must not claim cleanup without `set (0)` acknowledgments. The script does not flash, change power or thermal settings, skip frames, or touch voltage. The saved `putty-P5-display-wake-gpu-serial-YYYYMMDD-HHMMSS.log` can be fed directly to `tools/s9-p5-report.ps1 -Log <file>` and returned for analysis.

**Limitations:** serial-specific physical transport is not yet hardware-tested with this script from the @ioss9 workspace, which does not expose the user's COM125. The Windows dry-run and source validation are testable; final correctness depends on the observed USB echo and device ACK. If it cannot open the port, close PuTTY and other monitors; do not run both concurrently. If the loader still echoes truncated commands, save the new serial log to investigate USB OUT handling in the kernel rather than forcing P5 phases.

### Legacy: automation through a PuTTY window (COM125 execution disabled)

The old `tools/s9-p5-putty-auto.ps1` is retained for historical reference and dry-run only on COM125. Its `-Execute -Port COM125` path now deliberately refuses to run after repeated real-device corrupted commands. **Do not reattempt P5 via GUI keystrokes; use the direct serial script above.**

`tools/s9-p5-putty-auto.ps1` is a Windows PowerShell 5.1 keyboard automation for the **existing PuTTY window**. It does not open COM125 and therefore does not conflict with PuTTY's exclusive serial port; leave PuTTY running and its **session logging enabled**. The script must run on the **same interactive Windows desktop** as PuTTY, not on a remote workspace host that does not see COM125. PuTTY should show a logged-in shell prompt. The target window is selected by its title containing `COM125`, or by the explicitly supplied PuTTY PID when its title was customized.

From a local checkout of `s9/p5-display-wake-gpu-lab`, open a separate local PowerShell console:

```powershell
# Dry-run first: prints ALL commands, waits, and physical actions, sends NOTHING.
powershell -NoProfile -ExecutionPolicy Bypass -File tools/s9-p5-putty-auto.ps1 -Port COM125

# After verifying PuTTY session, its COM port, log file, and P5 kernel+SYSTEM:
powershell -NoProfile -ExecutionPolicy Bypass -File tools/s9-p5-putty-auto.ps1 -Port COM125 -Execute
```

Execution asks you to confirm `COM125`, sends **only** `sysctl kern.hakios.p5_stats` and a harmless `sysctl kern.hakios.p5_phase=0` marker, then requires an exact human check of the `p5: PHASE=0 up=...` output (type `PHASE=0`, not READY). **After the real 2026-11-21 COM125 run** showed every `SendKeys::SendWait(full-string)` command mangled (`sysctl` became `sctl` and loader replied `usb: sctl: not a command`), the corrected script sends each keystroke individually at **90 ms/key**, then **two Enter keystrokes by default**, with **450 ms between Enter presses** and **600 ms between commands**. This reproduces the user's observed manual PuTTY COM125 behavior: the loader sometimes needs an additional Enter before it accepts the line. A second Enter normally submits an empty line if the first one already executed. Adjust with `-KeyDelayMs 120 -EnterDelayMs 600 -CommandGapMs 900` if the loader serial console still drops characters. For other consoles that only require one Enter, use `-EnterCount 1`. The exact `p5: PHASE=0` preflight marker remains mandatory. Do not continue if the exact P5 preflight phase marker is absent; if necessary close the script and use another serial transport. The earlier failed trace did not activate any P5 mode and does not prove a P5 instrumentation problem. The timed A1/B1/A2/B2 phases and their `p5_phase` markers are then automatic. On the OFF/ON phase it prompts you to press the **physical** display power button (the script cannot toggle screen power safely); wait 110 seconds with display OFF, then physically wake and press Enter promptly. It ends by sending `p5_mode=0`, `p5_threads=0` and `p4_mode=0` through the same verified window. These safe-exit commands are **best effort**, not a device acknowledgement, so check PuTTY output and run them manually if communication was lost. Keep the host desktop otherwise idle while running; keyboard automation may steal focus. If multiple PuTTY windows exist or the title hides the COM number, select an explicitly verified target with `-PuttyProcessId <PID>`.

**Important limitations:** this is GUI keystroke automation, not a serial/RPC API with positive acknowledgement or reliable process isolation. If PuTTY loses focus, closes, or Windows blocks focus changes, the script aborts rather than type into another window. It cannot read PuTTY's terminal content for you, cannot turn the screen off/on, and cannot start PuTTY logging. Do not use `putty -m` for a serial connection; `-m` is for remote SSH commands. If `p5_stats` is an unknown OID, stop and deploy the matching P5 BOOT+SYSTEM first.

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
