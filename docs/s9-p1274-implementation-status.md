# S9 P12.7.4 — Late Thermal Burst Control

Scope: one paired BOOT/SYSTEM source build; one explicit bounded profile per boot.
No PSCI/CPU_OFF, CPU_SUSPEND, voltage, panel DCS or unvalidated layout activation.
The original P8/P9/P1273 guards remain. No automatic profile escalation.

## Implementation

| Item | Source / evidence |
|---|---|
| .1 | p1274 late trigger: BIG busiest >=80% for 250ms, direct >=70C, +3C in recent <=2s; emergency stop retained. |
| .2 | Existing TFR rolling 128 x 250ms begins at uptime 230s; eligibility >=240s, 10s post trigger or earlier abort; independent timeline from arm to 420s. One event, not all events. |
| .3 | Peak, weighted mean and aggregate load, current request/ceiling, direct TMU time, cached LITTLE/GPU. Bucket weights assigned at end of governor window, not split exactly. |
| .4/.5 | Windows typed serial with boot nonce, checkpoint each validated page, BIG topology mask first, first GPU wake and FG; finite per-invocation budget. |
| .6 | Runtime mode cached 250ms, BASE=0 logging, all other profiles=2 suppress repetitive unsupported-layout logging. Existing isolated env mode retained. True atomic refusal count, bounded snapshots, descriptor/caller. Last snapshot is a lower bound (no periodic worker / no promise of final flush). API return unchanged. |
| .7 | lock acquisition, dlopen/dlsym, opener, device, ICD select, runtime Device::system_default_with, queue wall ticks and thread CPU ns where available. Timings nested, not additive, do not prove CPU fallback or split inside external runtime. |
| .8 | cputop existing 5s process sample selected names, observed new PID (not exact spawn/DAS scheduling); poster_retry decisions, PID/task/TID producer. User event contents advisory. |
| .9 | B1/B2/B3 lower-only BIG ceilings 1469/1261/1066 MHz before burst. LITTLE unchanged. |
| .10 | BIG direct TMU each ~200ms in opted-in lease; separate lower-only fast limit for FAST/B/R, emergency also BASE/LOG2. On sensor failure/thermal abort no upward release; 5s valid cool interval before clearing; stock HOT_CUT/OCP retained. Actual sensor conversion and ACPM latency still need measurement. |
| .11 | R2/R1/R0 scheduler recommendation through existing governor owner, dynamic affinity map; actual point sample vs requested, all 8 CPUs online. R profiles refused if OCP requires BIG binding; R0 skips active BIG frequency spin, not all possible BIG wake sources. |
| .12 | Independent bounded RAM timeline, FG, advisory Metal/process event, WFI, USB, DVFS and high-res CPU/GPU streams; freeze on planned end/abort; re-read only same boot. No USB polling required offline. |
| .13 | Fixed common 250–410s net battery discharge Wh (interpolation, no extrapolation, <=15s gaps), raw signed modular QH bracket delta; measured peak and load-to-idle proxy; no validated QH scale, no external supply watts. |

## Kernel contract

Opt-in boot flags p1273=1 p1274=1; all policy modes inactive until sysctl request.
Arm before mach uptime 225s with 4+4 online, screen requested OFF, valid thermal data,
policy2, no previous trace, no concurrent P125/P126/P127/P8/P12/P124 trial.
Intervention needs observed input quiet and safe UFS. A missing or unsafe condition refuses.
Absolute deadline 420s (not 420s after arm). Abort on display ON/unknown, thermal,
input for intervention, conflict or cancellation. Profile 0 requests stop. Emergency
thermal ceiling can remain latched after profile stops until valid cool hysteresis.

Profile IDs 1 BASE, 2 LOG2, 3 FAST, 4 B1, 5 B2, 6 B3, 7 R2, 8 R1, 9 R0.
Frozen state 3, reason 1 planned, 2 heat, 3 missing thermal, 4 display, 6 operator,
7 input, 8 conflict. Trace reasons separate, 7 normal post-trigger.
Rows: 0..479 timeline, 512..575 FG, 1024..3071 user event, 3072..3583 DVFS,
4096..4223 detail, 5000..5007 WFI. Check status counts; not all slots valid.
Finite arrays never overwrite except TFR/detail/GPU rolling history. Drops and
missing user-space reports (e.g. sandbox restrictions) invalidate attribution.
WFI counts completed sampled WFI intervals; in-flight at freeze can be omitted;
not power-state residency. No synthetic wake for R0 qualification.
User event write is a fixed 64-byte advisory payload, rate and count bounded.
It cannot alter profiles. Kernel stamps task identity; user payload is not a trusted
launch event. No PID names, paths or pointers accepted by the ABI.

Event kinds: 1 no-copy refusal, 2 layout refusal (a calls,b printed,c suppressed,
d measured logging-path us,e format,f width:height packed 32bits each,g
type/storage/mips/backed packed 16bits each); 3 stage (a stage,b wall ticks,c CPU ns
or UINT64_MAX,d success,e begin,f caller category,g offset), stages 1 lock,
2 dlopen,3 dlsym,4 full opener,5 nmb_device_open,6 queue,7 ICD,8 runtime,
20/21 first caller; 4 process (a code,b subject PID,c delta ticks,d cumulative
ticks,e observed new PID,f sample span,g sample tick); 5 poster_retry decision.
Process codes in userland/src/cputop.c; actual DAS policy unavailable.
DVFS type1 ceiling change, type2 requested/returned ACPM rate; measured MHz is a
separate sampled field, not instantaneous proof.

## Validation

Run node tools/tests/test-p1274-contract.js, portable C test
tools/tests/test_s9_p1274_policy.c, and PowerShell tools/tests/test-s9-p1274-serial.ps1.
CI also runs legacy contracts, host tests and iOS syntax checks.
Native XNU / Rust bridge / full SYSTEM build and S9 tests are required before any
claim of hardware qualification. Synthetic cases do not prove thermal improvement.
No native build or hardware data produced by this source patch alone.
