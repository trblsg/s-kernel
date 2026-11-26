# S9 P12.7.5 — ICD root-cause and protected OFF experiments

## Evidence and scope
P12741 LOG2 A0 froze at uptime 360.148 s (reason 2). PosterBoard PID 130
TID 3292: opener 1766.107 ms thread CPU, ICD selection 1710.822 ms
(~96.9%), device initialization 42.409 ms. CPU5 owner bin confirms
246.621/250 ms; only that bucket's user/system split is measured.
First GPU wake backboardd after opener. 97 layout refusals suppressed.
LITTLE reached 71.779 C, BIG 76 C in hot tail. The data do not prove
all ICD loading is binding, all Poster traffic uses Metal, or migration
from BIG to LITTLE.

## One build, cold-boot experiments
Explicit p1275=1 alongside p1273=1 p1274=1. Same arm and 720 s absolute
deadline, 225 s latest arm, existing thermal/OCP/CPU/PSCI/panel safeguards.
p1275_revision=1 extends ABI2, new p1275_profile=1+bitmask arm invokes
LOG2 recorder; p12741_profile=0 cancels capture. No boot-time profile.
Bit 1 typed layout nil; bit 2 predictive load cap; bit 4 advisory ICD
coordination; bit 8 RTLD_LAZY. Host modes Observe=1, Early=2, Load=3,
EarlyLoad=4, Coord=5, Lazy=9, Combined=8, CombinedLazy=16. Legacy modes
unchanged. Experiments only LOG2 Connected; explicit intervention opt-in.

- **P1275.1** A typed shim is installed only on a matching inherited
  forwarded selector (return object, descriptor object, exact BOOL ABI).
  P1275 boot opt-in required, runtime early flag while OFF, LAZY state,
  matching self, known disabled layout and no layout/probe flags or env
  plan. Backboardd never bypassed. Normal open and real frontend
  implementation on all other paths. Shim installation ABI=0/1 logged;
  ABI=0 safely leaves old path. Sparse event6 code100 cumulative
  deferred requests, code101 later real open: defer is not proof of
  permanently eliminated ICD load. Early optimization may persist during
  OFF hot tail; old telemetry suppression retained. Cache up to 250ms.
- **P1275.2/.8** Open selector classification (event6 105 begin, 106 end;
  codes 1 layout, 2 alignment, 3 queue, 4 texture, 5 other) with existing
  caller-offset/raw process CPU samples. Alignment return remains
  unchanged, no guessed alignment. No private ImageIO binary hook;
  fallback/mediaserverd causal attribution still indirect.
- **P1275.3/.4** Stage7 substeps 9 selected mutex, 11 dlopen, 12 entry
  dlsym, 15 negotiation dlsym, 13 negotiation call, 14 own_path.
  Stage8 scoped synchronous first calls 16 first driver GIPA,
  17 extension enumeration GIPA, 18 create-instance GIPA, 22
  enumerate-physical-devices GIPA, 19 actual extension enumeration
  call using typed Vulkan ABI. No synthetic driver calls. 18/22 are
  lookups, not actual instance/device functions. All stages nested,
  do not add parent+child. Other-thread calls may be missing.
- **P1275.0/.5** Node read-only Mach-O analysis of the exact cached
  PanVK input in installer; SHA256/UUID, segments, dependencies,
  nlist imports/exports/weak dynamic lookup, export trie, classic
  stream sizes, chained import records/populated pages, raw initializer
  entries. Optional LLVM dumps. Input dylib and existing build manifests
  uploaded; no actual binary available in local workspace before this
  change. Chained pointer walk, constructor symbol attribution and
  dynamic execution counts not guaranteed. No speculative change to
  weak dispatch generator, visibility or linker flags.
- **P1275.6** RTLD_LAZY only via bit8 on a new ICD load, otherwise NOW
  unchanged. A/B fresh process/boot, only after review. Does not assert
  lazy binding works with chained fixups; compare total eventual work
  and functional behavior, not initial dlopen alone.
- **P1275.7** Optional flock LOCK_EX|LOCK_NB on read-only descriptor
  of existing root-owned, non-group/world-writable PanVK dylib (no new
  shared file). Exact driver path and poster process allowlist. Maximum
  wait target 1800 ms, poll 20ms, recheck OFF mode, fail open on denial,
  timeout or wake; no foreground/window server waiting, no forced
  unlock of another process. Holder closes on stage7 completion or
  opener return. Event6 102 outcome (1 acquired, 2 timeout, 3 denied,
  4 wake/mode lost) and wait ticks. Sandbox/root permissions and actual
  coordination not verified. Not a claim of single-core total workload.
- **P1275.9** Workload cap after combined sum >=200% for 250ms:
  level1 BIG1261/LITTLE1248, level2 BIG1066/LITTLE1053 for >=400% or
  >=200% with BIG>=67/LITTLE>=65/rising. Lower-only min with stock,
  independent thermal guard and trial ceilings. Release after sum<100,
  both <=65 C continuously 3s, or display ON/recent known touch. After
  freeze remains eligible while OFF. Existing thermal guard stays on
  regardless of foreground. Rate event type5 records level transitions,
  type2/4 actual DVFS. p1275_load_state exposes effective level.
- **P1275.10** Thread CPU time from existing CLOCK_THREAD_CPUTIME_ID;
  event6 substep a=stage, b/c process user/system us, d/e process
  minor/major fault deltas, f=start tick, g=valid. RUSAGE_SELF includes
  other threads and is not thread attribution. Code103 stage start
  markers for censored stages; code104 actual flags. XNU owner pages
  show CPU placement, not exact migration path. No new scheduler hook.
- **P1275.11** Frozen index p1275_focus up to 48 newest stage/event6
  selectors, early DVFS last16 and up to four nearby owner bins across
  both clusters. Resume bounded pages. CSV metal-stages, metal-process-
  usage, p1275-events, thread-placement. Partial dump explicitly not
  qualified; no new energy claim.

## Verification and limitations
Local Node/PowerShell source/mock tests plus synthetic Mach-O test.
Portable C policy/header and iOS lazy shim syntax added to CI/preflight.
Actual native Rust, C/ObjC++, iOS SDK build and hardware not verified
locally (no compilers). Need successful matched BOOT/SYSTEM build and
review of actual PanVK audit artifact. No promised peak temperature or
guaranteed time savings. Keep same hard stops, do not force reentry or
raise thermal thresholds, no CPU_OFF/CPU_SUSPEND/voltage/panel changes.
