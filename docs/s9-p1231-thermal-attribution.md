# S9 P12.3.1 – thermal attribution, same-firmware bounded trials

Derived from P12.3 hardware captures 122946, 124011, 124926, 125511, 130629, 132047 (2026-11-24).
Use paired BOOT+SYSTEM and the three matching Windows scripts from this branch.
No CPU_OFF, CPU_SUSPEND, CPD, SICD, panel DCS sleep, raw rail voltage or raw frequency writes.
Default remains stock P12.3 rendering/clock policy, no autonomous Clock cut, no adaptive floor or checksum trial armed.

## Fixes
- CPU idle probe uses XNU logical cpu_number, not opaque PE cpu_id (GenericARMCPU object).
  Both entry and exit paths fixed. Report rejects advancing unmapped or missing mapped entries.
  WFI timestamps cover existing idle opcode path only; not electrical residency or deep sleep proof.
- GPU floor accepts exact [260,455,572] or [260,455,572,598] tables. A 30s nonextendible trial
  lowers only the frame floor, not load governor or thermal caps. Input/scene/temperature guards unchanged or stronger.
  arm refusal bitmask: 1 diagnostic disabled; 2 governor not running; 4 DVS; 8 unsupported table;
  16 floor not 450; 32 display not ON; 64 input not quiet; 128 TMU stale/invalid/hot >=72C.
  GPU stats schema3 maps actual MHz 260/455/572/598 and other; schema2 index2 was mislabeled 598.
- Clock rearm wait uses firmware uptime >=180s plus 25s margin (20..200s), active deadline
  115s from observed baseline, overall 315s cap. Explicit STOP 201 plus SpringBoard ACK on completion/abort.
  Do not clear pending STOP mailbox if STOP ACK missing. Preserve existing thermal and no-input guards.
  Runtime metaclass evidence plus bounded read-only main-queue scan of live UIKit Clock instances
  every 20 sampler ticks. 512 nodes/18 levels, no suppression policy based on scan; geometry is not complete visibility proof.
- GL mode requested vs applied: actual context applies on render thread, polls first frame after idle and
  at most 250ms or 16 frames while rendering. Read failure falls back to minimal without false ACK.
  Bounded 500ms control-only observer (starts when nonzero applied in backboardd, stops at mode0 or 180s cap);
  no GL calls, no GL state from its thread. MicroGesture may finish with control ACK + no new presents,
  explicitly deferred, NOT a userland context applied ACK. Fresh mode2 applied ACK required.
- MicroGesture waits for actual TouchEvents contact/release, one gesture only. Snapshots before wait,
  after release, after recovery; serial receive latency and straddling frames remain, not a precise instantaneous GPU trace.
- Profile2 times hashes as well as bind/draw/flush with rotating one-in-16 call sampling.
- Optional checksum A/B/A: p1231_hash_mode=0 legacy default, 1 scalar 4-lane reference, 2 fast
  equivalent 4-lane. Both nonzero modes expire after 30s, fail to zero on input/display/TMU refusal,
  cannot extend active deadline, mutually exclusive with adaptive floor. Per-image digest scheme
  tag forces conservative refresh when changing legacy/4-lane digest. Preserve seed, sync/fences,
  dirty check semantics and stable-surface cadence; fast path sampled scalar check every 128,
  mismatch forces refresh and sticks to reference for that process. No native GL bind skip.
  Compare byte-normalized sampled times only with sufficient comparable windows; exclude migration window,
  report verification/fallback. No automatic optimized default.
- G3D lab domain/PLL/mux OFF periodic rows recorded separately from mali-power/thermal/busy;
  do not count absent GPU data as 0 or claim wake from historical counters.
- Native UFS evidence p1231_ufs_stats is read-only, captures switch, selftest/fence, loader core taken,
  native/loader reads, errors, writes, busy. Existing UFS speed and CPU boost records retained.
  LITTLE floor remains 1053MHz: native read switch alone does not prove loader service no longer needed.
  No frequency changes until hardware evidence supports a separate guarded trial.

## Install
Installer branch fix/s9-p1231-thermal-installer; kernel_ref s9/p12-3-1-thermal-attribution; metal=true.
Use existing boot args:
```
hi=2 panel=1 p122=1 p12_gpu_boot_auto=1 mali_gov=1 mali_gov_frame=450 mali_probe=1 p3power=1
```
Do not add p0hash/p0verify, glhash, glcost, glstage, g2_bind, mali_dvs, panel_sleep,
p9_panel_mode, sleep_cores or syssleep. Do not force mali_max to a different baseline.
Full installer reformats USERDATA: backup before flash. Same boot arguments as P12.3.

## Windows
Use matching s9-p122-rootcause-serial.ps1, s9-p12-dual-serial.ps1,
s9-screen-lifecycle-serial.ps1. Preview without -Execute. Use COM125 or actual port.
Fresh cool-start BIG<=70 LITTLE<=68 G3D<=68, no repeated swipes, existing early/hard thermal limits.
Run separately, cool with physical screen OFF between tests. Do not reboot merely to cool.
```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\s9-p122-rootcause-serial.ps1 -P123 -IdleOnly -WfiProbe -IdleWindowSeconds 120 -Port COM125 -Execute
powershell -NoProfile -ExecutionPolicy Bypass -File .\s9-p122-rootcause-serial.ps1 -P123 -AdaptiveFloor -WindowSeconds 12 -Port COM125 -Execute
powershell -NoProfile -ExecutionPolicy Bypass -File .\s9-p122-rootcause-serial.ps1 -P123 -HashCrossover -WindowSeconds 20 -Port COM125 -Execute
powershell -NoProfile -ExecutionPolicy Bypass -File .\s9-p122-rootcause-serial.ps1 -P123 -ClockCrossover -Port COM125 -Execute
powershell -NoProfile -ExecutionPolicy Bypass -File .\s9-p122-rootcause-serial.ps1 -P123 -MicroGesture -Port COM125 -Execute
```
Idle: press POWER once when instructed, do not wake unless explicitly using -WakeCheck.
MicroGesture: one gentle swipe only when prompted. Clock test requires page with active 10Hz Clock.
Optional ordinary profiling: -P123 without scenario switch. Retain raw, events, p123, p122, p12,
lifecycle report and samples CSV. No absolute energy/thermal gain claim without meter and comparable workload.
