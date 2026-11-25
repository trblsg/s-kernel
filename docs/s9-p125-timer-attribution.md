# S9 P12.5 – timer source attribution (iOS 16.5)
P12.5 is diagnostic, default OFF. Do not change IRQ27, deadlines, coalescing,
scheduler, PSCI, GPU voltage or panel settings from this experiment.

## Build and run
Use the paired installer branch and kernel revision. Download the matching
s9-p125-symbols artifact (p125-symbols-starlte.txt) alongside the installer.
Copy s9-p125-timer-serial.ps1, s9-p12-dual-serial.ps1 and
s9-screen-lifecycle-serial.ps1 into the same PC directory.
Run:
```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\s9-p125-timer-serial.ps1 -Scenario Trace -SymbolMap .\p125-symbols-starlte.txt -Port COM125 -Execute
```
Trace = 25s OFF-STABLE, 20s aggregate, 60s detail (plus frozen snapshot reads).
UsbCrossover adds 60s offline B and 60s connected A1. Unplug and reconnect
only when prompted; keep screen OFF. -ReconnectPort COM126 if COM changes.
OfflineOnly captures B alone; AggregateOnly captures lightweight counters alone.
No automatic cooling wait. Probe arm requires fresh TMU, display OFF; abort
on display ON, thermal/stale conditions or bounded 180s expiry. No device reset.
End time for B is kernel USB-link reconnect, not host serial read time.
Host USB poll deltas include snapshot traffic; distinguish link from physical
cable/charging effects. Do not use concurrent serial scripts or other probes.

## Evidence
Per-CPU cumulative rows (512 slots, four independent 128-slot banks) and
counters, bounded lookup, no eviction, no IRQ printing or allocations.
Aggregate vs detail measures observer perturbation. Frozen cursor dumps all
populated rows. Match the kernel symbol anchor; unknown addresses/names remain
unknown. Stable kernel thread entry map is bounded and never reuses IDs.
PID names are looked up only in frozen readback and require task generation
match; exited processes remain unidentified by name.

IRQ queue services = zero callback services + nonzero callback services.
Callbacks >= nonzero services; table per-kind totals = recorded + drops.
Layers nest; never add thread-call, Mach timer and IOKit counts to IRQ counts.
Wall time is sampled inclusive duration (1/32 callbacks), not exclusive CPU time.
Requested interval, leeway-zero and effective-zero differ; UINT64_MAX marks
unknown effective slack. Flags are cumulative OR, not phase-specific.
Enter rows expose local/boot assignment when known (aux = logical CPU + 1,
zero unknown); expiry CPU is observed separately, not a per-object migration
trace. Mach send count is an attempt, not guaranteed delivery. Table drops
and thread-map drops limit completeness. No claimed source until measured.
Kernel callback in flight can cross observation boundaries; coverage mismatch
is rejected, not silently normalized. Retain raw and all CSV reports.

## Optional quiet experiments
Use matched s9-p122-rootcause-serial.ps1 and dependencies. Example:
```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\s9-p122-rootcause-serial.ps1 -P124 -QuietSuite -Modules CLOCK,HASH -Port COM125 -Execute
powershell -NoProfile -ExecutionPolicy Bypass -File .\s9-p122-rootcause-serial.ps1 -P124 -CpuFloorStep 1 -CpuFloorWindowSeconds 60 -Port COM125 -Execute
```
Default remains 12s A/B/A; explicit 60s window requires matched P12.5.
Mode 1 LITTLE949 lease maximum 90s, modes 2–4 remain 30s; no rearm.
Same thermal, rise, input and UFS safeguards. Run individually only if needed.
Hard stop ends suite; admission skip remains explicit.
