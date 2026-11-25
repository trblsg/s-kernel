# S9 P12.4.1 warm-start guarded thermal experiments (iOS 16.5)
One matched BOOT+SYSTEM build. All new optimization leases default off. The delayed CPU stress benchmark is removed from rc.jobs; the no-argument topology report remains. Explicit manual benchmark: cpu_test --benchmark (not needed for these tests).

## Build
Installer branch fix/s9-p124-thermal-installer; kernel_ref s9/p12-4-guarded-thermal; use the existing installer settings (metal=true, time_limit=0, starlte=13917184). Build fresh from the paired source; the pinned XNU fork overlay adds the IRQ observer during checkout.
Boot args:
```
hi=2 panel=1 p122=1 p12_gpu_boot_auto=1 mali_gov=1 mali_gov_frame=450 mali_probe=1 p3power=1
```
Do not add panel_sleep=1, p9_panel_mode=1, sleep_cores=1, syssleep=1, mali_dvs=1, old hash/per-call profiling flags or custom frequency caps. panel=1 initializes the panel driver; not panel sleep. Full installer may erase USERDATA; back up first.

## CPU policies
p124_cpu_mode: 0 stock; 1 LITTLE floor 949MHz; 2 LITTLE floor 832MHz; 3 only initial BIG burst floor at most 1469MHz; 4 BIG burst/hold floor at most 1261MHz, moving-frame floor at most 1066MHz, LITTLE touch floor at most 1053MHz. No change to the 120ms burst duration, load-governor ceiling, thermal/OCP limit, voltage, CPU online mask or CPU_OFF refusal. Exact supported DVFS table required. Lower floor is not a frequency cap. A 30s nonrenewable lease reverts on expiry or guard failure. LITTLE only when quiet, display on, all cores online, stock floor 1053MHz, native UFS healthy, loader state GONE (not just requested), zero observed storage errors/fallback/refusal; stock floor while native I/O is in flight, evaluated at the next governor decision. Native wall-time totals are inclusive, not NAND latency. Boost reduction bypasses high load, thermal limitation and recent late-frame evidence. A/B/A quiet trials are independent of gesture trials.

P12.4.1 replaces the unattainable shared cold-start gate. The initial 12s live baseline uses BIG <=70C, LITTLE <=68C, GPU <=68C. Every trial arm requires fresh temperatures and recent 10s trend evidence (at least 3 CPU, 2 GPU samples; reject first/last short-average rise >1C). Quiet floor/GPU/Clock <=68/67/67C; fast hash <=68/66/67C; one gesture <=67/65/66C. Kernel CPU arm uses matching quiet/gesture caps and fresh TMU <=2s. Kernel floor revokes at rise >=3/2/3C from arm; host quiet B phases enforce the same relative-rise stop. Existing early stops >=76/70/72C and hard stops >=82/75/77C, battery >=47C unchanged. No auto wait/retry; skip ineligible modules, abort suite on runtime safety/ACK/quality failures. p124_cpu_stats guard_revision=2 required; effective/bypass count evaluations, not actual rate changes.

## Other independent leases
- WFI probe: read-only per CPU acknowledged GIC interrupt counters (SGI/PPI/SPI, virtual timer, top four INTIDs), MPIDR and WFI duration bins <100, 100-499, 500-999, 1000-9999, >=10000us. No IRQ acknowledge/dispatch changes, no extra IAR read, no physical CPU suspend. Eight sequential snapshots are not simultaneous. Compare deltas between stable windows; cumulative top-four selection is not a full per-window ranking. IRQs and WFI returns are correlation, not one-to-one wake attribution.
- Timer service: p124_timer0..7 read-only counts calls, idle_due, queue_due, running_expired (actual return value), none_due, before_rtcPop at timer_intr entry after rtclock handling, setPop calls, overdue requested deadline and programmed delay bins. Reasons may overlap; queue_due is not callback count, none_due not proof of hardware fault, setPop excludes direct SetIdlePop writes. The pinned XNU overlay preserves actual callback execution and hardware timer programming. No interrupt masks, scheduler/timer policy or deadline changes. Probe disabled by default.
- GPU adaptive floor: validated P12.3.1 eligibility retained; real touch cancels GPU/hash lease without fake present or clock write. Governor restores stock behavior at next decision; actual GPU clock can lag.
- Clock cadence: 25s exact SBHClockApplicationIconImageView class/selector source, Home scene fresh and input quiet, ~1Hz forwarding (not global frame cap). Cancels on input, unsupported scene, display off, competing policy, mailbox failure, expiry. No blanket CA/display-link suppression; verify the icon time and motion. Display/scene polling is bounded but not instantaneous.
- Hash: legacy(0)/fast(2)/legacy(0), each 12s warm + 12s measured; scalar trial mode1 refused. Fast 30s lease, sampled scalar verification at most 2MiB and once per 2s per context; report skipped verification. On mismatch return legacy, refresh image digest, report fallback. Require matching mode and two migration-free windows, exclude boundary windows; workload/cache comparability still requires log analysis. No claim of comprehensive checksum proof.
- Texture: sampled create/remove inclusive CPU wall durations and counts with explicit -GestureDetail; existing swapwait and GL/GPU stage data retained. No texture reuse, bind skip or fence removal.

## Windows commands
Use matching s9-p122-rootcause-serial.ps1, s9-p12-dual-serial.ps1 and s9-screen-lifecycle-serial.ps1 in one directory, close other serial readers. Use -P124, not older -P123 for hash. Replace COM125 if necessary. Preferred one-pass run below; the four quiet A/B/A modules use separate leases and fresh admission checks in the same serial session. Optional idle phase at the end includes IRQ and timer source snapshots. Keep Home visible, no touches until physical POWER OFF prompt. Stop if device becomes hot or unresponsive; do not bypass guards. The separate commands remain available for missing modules.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\s9-p122-rootcause-serial.ps1 -P124 -QuietSuite -IncludeIdle -WfiProbe -IdleWindowSeconds 120 -WindowSeconds 12 -Port COM125 -Execute
```

Omit `-IncludeIdle -WfiProbe -IdleWindowSeconds 120` for quiet-only suite. Report `MODULE ... COMPLETE/SKIPPED/FAILED` distinguishes results; no eligible quiet modules is not a pass. No cooling waits between modules. An explicit admission refusal skips safely; runtime safety, unconfirmed restore, missing evidence, or quality regression stops the suite.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\s9-p122-rootcause-serial.ps1 -P124 -IdleOnly -WfiProbe -IdleWindowSeconds 120 -Port COM125 -Execute
powershell -NoProfile -ExecutionPolicy Bypass -File .\s9-p122-rootcause-serial.ps1 -P124 -CpuFloorStep 1 -Port COM125 -Execute
powershell -NoProfile -ExecutionPolicy Bypass -File .\s9-p122-rootcause-serial.ps1 -P124 -AdaptiveFloor -WindowSeconds 12 -Port COM125 -Execute
powershell -NoProfile -ExecutionPolicy Bypass -File .\s9-p122-rootcause-serial.ps1 -P124 -ClockCadence -Port COM125 -Execute
powershell -NoProfile -ExecutionPolicy Bypass -File .\s9-p122-rootcause-serial.ps1 -P124 -HashCrossover -Port COM125 -Execute
powershell -NoProfile -ExecutionPolicy Bypass -File .\s9-p122-rootcause-serial.ps1 -P124 -MicroGesture -Port COM125 -Execute
powershell -NoProfile -ExecutionPolicy Bypass -File .\s9-p122-rootcause-serial.ps1 -P124 -MicroGesture -BoostTrial 1 -Port COM125 -Execute
```
First idle: physically turn screen OFF only when prompted. All other quiet scenarios: Home, no touch. Gesture: one gentle swipe only when prompted. Default mode0 profiler, stock vs reduced initial burst in separate headroom-checked runs; no second mandatory 12s cool-start. If the gesture cannot safely start, skip it rather than spending 20 minutes repeatedly trying. Do not run step2 or -BoostTrial 2 before reviewing step1, UFS, FPS/latency and recovery results. -GestureDetail is optional separate higher-overhead diagnostic. Save raw .log and all matching reports. No hardware performance or power saving is established by mock/portable tests.
