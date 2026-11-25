# S9 P12.7.2 — powered-off Mali GET_PARAM safety

## Confirmed failure 092915

Firmware 11ea4c69, anchor fffffff007486584. Updated matching symbol map
resolves PC fffffff0075460cc to hakios_mali_parameter+0x44 and LR
fffffff007acee28 to HakiosMaliUserClient::externalMethod+0x568.
PosterBoard PID136 TID2239 was initializing its lazy Metal device.
G3D PMU/PLL/mux were off with wake_count=0. x0=2, ESR=0x96000610
(synchronous external abort; FnV means FAR not architecturally valid).
FAR low bits match GPU base+SHADER_PRESENT offset 0x100.
ABOX stop/resume counts1/1 fault0, panic 13.7s after restore.
No BT/WIFI/TICKER/PSR/LITTLE949 trial started.

## Kernel
- Cache the entire pre-existing parameter ABI 0..40 once after successful
  powered boot/core/table test, under mali_job_lock, before user service
  publication. Retain zero values as valid, mask 1ffffffffff, TLS minimum
  and AFBC semantics unchanged. Release/acquire publication. Immutable
  until reboot; no runtime cache fill, no GPU wake for GET_PARAM. Reject
  invalid index/null or incomplete initialization. Driver-specific
  written/sampled handle flags remain in the user client.
- GET_PARAM validates the request/response structure length. Normal
  client creation and queries remain available while GPU is off.
- Runtime MMIO paths check PMU full ON + PLL stable + CMU mux + verified
  software safe/TZ state under mali_job_lock. Actual job wake retains
  existing power/DTZPC/reset/core/MMU sequence. IRQ arm/teardown,
  mapped/unmapped, failed-job reset and explicit recovery refuse unsafe
  access. Late IRQ handler uses only GIC mask + wake; raw job bits remain
  worker-owned and watchdog still verifies completion. IRQ empty now
  denotes idle/unavailable software state, not observed raw==0.
  Unknown/partial ON on unmap latches fault and quarantines VA rather
  than reusing it before verified recovery. Governor does not change
  clock/rail while domain is off/unsafe.
- Read-only kern.hakios.p1272_gpu_caps: ready, mask, hits, off_hits,
  param2, off_param2, reject, denied, live. off_hits is software
  availability hint, not a physical residency measurement.
- Read-only p1272_probe_state takes a trylock and observes only PMU/CMU
  and cached temperature. No new timer in kernel. Probe request=1
  only if boot p1272_probe=1, stable OFF>=10s and valid temperatures
  BIG<70 LITTLE<67 GPU<68 battery<45, TMU age<=2s; one request per boot,
  60s expiry, cancel=0. No direct hardware writes from request.
  No automatic change to CPU, GPU voltage policy, power-gate timing,
  ABOX, BT, WiFi, PSR, GNSS, or P127 energy policy.

## One build, explicit separate qualification
Use s9/p12-4-guarded-thermal and fix/s9-p124-thermal-installer.
Same arguments as P12.7, append p1272_probe=1 for the optional qualification:
```
hi=2 panel=1 p122=1 p12_gpu_boot_auto=1 mali_gov=1 mali_gov_frame=450 mali_probe=1 p3power=1 max77705_fg_s=10 p1272_probe=1
```
Keep panel_sleep, CPU_OFF, CPU_SUSPEND, mali_dvs and forced gates disabled.
The paired userland rc.jobs launches mali_test --p1272-auto, immediately
exits without the boot opt-in. With opt-in it waits for a host request
at most 900s, no job before request. Start host test promptly after boot.

Use matched s9-p125-timer-serial.ps1, s9-p12-dual-serial.ps1 and
s9-screen-lifecycle-serial.ps1. Close other COM clients.
```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\s9-p125-timer-serial.ps1 -Scenario GpuCacheCheck -Port COM125 -Execute
```
Screen OFF throughout (press POWER once if prompted). No USB unplug,
no symbol map required. No P125/P126/P127 lease active. Host waits for
stable OFF then sets one bounded request. Userland opens a fresh IOKit
connection and reads all 41 params, rejects index41, verifies OFF stayed
OFF and wake/off counts unchanged, rechecks conditions, submits one
write-value job and verifies wait/data, compares all 41 ON values.
Host requires OFF and COMPLETE PASS markers, off_hits>=41 and param2>=1
delta, denied=0, then independent re-gate proof. It cancels request on
exit; cannot interrupt a job already submitted, which is bounded by
existing job watchdog. Any missing proof, thermal stop, refusal or
unexpected wake is inconclusive/failed, never auto-repeat. If the worker
has expired or request already used, reboot to retry (not reflash).
A new unrelated GPU job can confound this one-shot; no false claim of
isolated energy savings from this probe.

After successful qualification, run only remaining P127 modules:
```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\s9-p125-timer-serial.ps1 -Scenario EnergyTrials -EnergyModules ABOX,BT,WIFI,TICKER,PSR,LITTLE949 -EnergySeconds 30 -RequireGpuCache -Port COM125 -Execute
```
USB clean A/B/A 092915 already passed (-10.21% IRQ27, -70.78% delayed).
ABOX-B effect not qualified until A1. Keep P127.1 checkpoint readbacks.
RequireGpuCache checks matched cache ABI and denied=0 before/after phases.
Keep all original thermal/restore/lease guards. No lower OPP or combined
policy before functional qualification. Capture new symbol map from
matched installer build (do not use the 11ea4c6 map for new firmware).
The boot flag is not needed for ordinary safe GPU queries, only the
explicit once-per-boot qualification.

## Limits
This fixes the confirmed software path and hardens related accesses;
PMU/PLL readback does not prove every physical bus transaction succeeds.
Existing hardware sequencing, AS0 update handshake implementation,
memory lifetime and other drivers are not a formal hardware safety proof.
No changes to ABOX hardware policy in this patch; repeat ABOX A/B/A only
after GPU qualification. Source and mocked tests do not substitute for
native kernel/userland build or physical device test. The test deliberately
wakes GPU once; do not mix its data with energy measurements.
