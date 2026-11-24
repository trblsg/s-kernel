# S9 / iOS 16.5 P12.2: identify touch heat without guessing a fix

## Context and evidence

The real 2026-11-24 P12.1 `083605` raw serial confirms that **backboardd uses Mesa/Panfrost OpenGL ES 3.1**; a Metal device is not available *in that process*, even though `metal=2` appears in bootargs. It is **not proof OpenGL is inherently the cause of heat**. The previous 60-frame GL summaries showed calls taking ~970 ms inclusive wall-time, particularly `glDrawElements`, `glBindTexture` and `glTexImage2D`, but those are **not CPU cycles and not GPU execution time**. A few outlier frames above 140 ms occurred during earlier userland start/scene transitions before the host live fence.

Mali source: `mali_gov_frame=450` is rounded to the real 455-MHz level for 100 ms following each presented frame; in some raw intervals `floor_forced=100/100` with GPU busy ~2%. It is not proof that lowering the floor is safe. `p0hash=1 p0verify=1 glcost=1 glstage=1` were also enabled on the **old** boot, so the measured overhead includes active instrumentation. `hi=2` automatic baseline/cut1 overlapped old passive monitoring. GPU domain and PLL did turn OFF on physical screen-OFF, but CPU idle remains uncertain. The P12.2 firmware must preserve P7 CPU3 wake and P9 panel safety locks.

## One matched BOOT+SYSTEM with runtime profiles

Use the already established **paired installer branch** `fix/s9-p12-thermal-idle-installer` and kernel ref `s9/p12-thermal-deep-idle`, but only after its CI passes. P12.2 source includes both kernel and userland, and the installer rejects an old/mismatched source before a costly build.

**Bootargs extra input** (keep other proven-safe options if needed, total extra budget <=280 bytes; avoid duplicates):

    hi=2 p122=1 p12_gpu_boot_auto=1 mali_gov=1 mali_gov_frame=450 mali_probe=1 p3power=1

Remove previous bootargs `glstage=1`, `glcost=1`, `glpath=1`, `glhash=1`, `p0hash=1`, `p0verify=1`, `glprof=...`, `gltrace=...` and other per-call probes for **valid causal overhead comparisons**. `glstage=0 glcost=0 p0hash=0 p0verify=0` may be explicit but are redundant when absent. Keep `hi=2`; host requires its existing SpringBoard `hi_cmd=201` acknowledgement to stop autonomous phase changes before causal profiling. Do not add `sleep_cores=1`, `syssleep=1`, `panel_sleep=1`, `p9_panel_mode=1`, `mali_dvs=1` or other unsafe CPU/GPU experiments.

The installer formats USERDATA. **Back up before flashing**. A successful CI source check is not evidence of a good flash or a temperature improvement.

## P12.2 controls and measurement semantics

- `kern.hakios.p122_diag_mode=0`: default minimal, no active per-GL-slot timing or extra GL stage calls; only lightweight per-frame duration/histogram. The **same bounded GPU governor/job counters run in all three profiles** when bootarg `p122=1`, so profiling modes have matched kernel measurement overhead.
- `=1`: Stage: 60-frame histogram (p50/p95/p99 **bins**, not exact quantiles), draw/upload/call counts and best-effort 1/16 thread CPU samples. No GPU clock writes. The GPU observer is intentionally constant across modes 0, 1 and 2.
- `=2`: Detailed: the same plus inclusive per-GL-slot CPU wall timing and top GL calls, in **60-frame groups aligned with the active profile**.
- `kern.hakios.p122_gpu_stats`: read-only cumulative GPU window counts, busy/elapsed ns, frame-floor forced counts, residency 260/455/598 MHz, job queue and execution sums/max/histogram. Host computes **deltas between snapshots** for each phase, instead of treating totals since boot as per-phase values.
- `p122-gl: mode=N ack=kernel_sysctl`: direct proof the backboardd GLES hook observed the kernel mode change. **Sysctl write ACK alone is not enough**; host waits for a second ACK, otherwise aborts and attempts `mode=0` restoration.
- `p122-gl-window`: 60-frame grouped wall times, p50/p95/p99 histogram buckets, CPU time on the **same 1/16 frames if `CLOCK_THREAD_CPUTIME_ID` is available**, draw/upload/call counts. Empty CPU samples mean **unavailable**, not zero work.

There is no 1-ms stack sampling, no per-GPU-job console printing, no dropped GL call, no skipped frame, and no automatic Metal switch. A profiled GL API call's wall duration may include waiting, and a Mali queue/run duration is not total electrical power.

## Safe host workflow

Download these **three files from the same P12.2 commit** to `C:\Users\Admin\Downloads\ioss9-data2`:

- `tools/s9-screen-lifecycle-serial.ps1`
- `tools/s9-p12-dual-serial.ps1`
- `tools/s9-p122-rootcause-serial.ps1`

Close PuTTY and start only after cooling naturally. Keep iOS display ON (auto-lock can remain disabled), do **not** swipe:

    cd C:\Users\Admin\Downloads\ioss9-data2
    powershell -NoProfile -ExecutionPolicy Bypass -File .\s9-p122-rootcause-serial.ps1 -Port COM125 -WindowSeconds 20 -Execute

The host does: live serial fence; reads the actual kernel version and idle/GPU stats; restores inherited nonzero CPU/WFI/diagnostic modes; **12 s cool-start** (fresh >=3 CPU/2 GPU samples and BIG<=70C LITTLE<=68C G3D<=68C); sends existing HI2 stop mailbox `201` with real `hi: gate=usb-stop` ACK; profiles **MINIMAL A -> STAGE -> DETAIL -> MINIMAL B**, each with >=1 complete 60-frame GL window and independent GPU counter snapshots; always sends P12.2 `mode=0` during cleanup on any abort.

P12.1 early stops BIG76C/LITTLE70C/G3D72C and hard stops BIG82C/LITTLE75C/G3D77C/BAT47C are unchanged. Serial and thermal sampling may lag; user should stop if the phone becomes unusually hot, not wait for the script. A lack of fresh samples causes refusal. Do not increase thresholds. The device does not need further repeated swipe-stress tests at this point.

**Optional, only after seeing safe passive logs:** `-MicroGesture` requests ONE gentle swipe inside a 4-second guarded window, after an independent new cool-start; no repeated gesture/25-second swipe is permitted. This short window might not generate 60 frames; such a result cannot be used to claim a measured p95. If the passive run already exceeds heat limits, never use it.

**Optional CPU OFF diagnostics:** `-IncludeIdle -WfiProbe` records P12.1 `kern.hakios.p12_idle_diag` during screen-OFF; physically press POWER to turn screen OFF when the script asks. No forced wake. This is not CPU_OFF, CPD or SICD.

If any sysctl/userland ACK fails, or a later temperature check fails, the run is incomplete, raw logs are retained, and mode0 restoration is attempted; a lost UART response remains **UNCONFIRMED**, not PASS.

## Expected artifacts, analysis and decision rule

The host emits `putty-S9-p122-rootcause-TIMESTAMP.log`, `.events.txt`, `.samples.csv`, `.p122-report.txt`, and P12.1 lifecycle/thermal reports. Compare MIN_A and MIN_B as drift controls; STAGE vs DETAIL reveals slot timer overhead; GPU floor-forced and DVFS residency distinguish GPU load from forced clock, queue vs run distinguishes waiting. Do not claim causality if HI2 rearmed, temps drift, GPU stats counter reset, GPU frame windows are missing, or no complete GLES profile window.

**Root-cause acceptance:** the suspected bottleneck must independently correlate with poor frame-time/temperature, explain a material cost in matched CPU-wall/thread-CPU/GPU snapshots, and survive observer-off/observer-on comparisons. Any proposed future optimization must preserve interactive 60 Hz latency/p95/p99 vs matched control with no new missed refreshes, GPU wake faults, CPU3 restart faults, display damage or thermal guard regressions.

P12.2 is **diagnosis first, not a validated heat fix**.
