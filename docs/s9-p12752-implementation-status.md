# P12.7.5.2 — independent idle, scheduler and workload attribution

P12751 A0 established zero flat/dynamic PanVK imports (4784 -> 0), ICD stage
7 CPU 4.17–12.40 ms, layout shim ABI=1 with early nil, but OFF baseline
BIG/LITTLE 62/60.5 C and natural poster burst 558% CPU, guard peaks
76/71.779 C. These observations motivate this build; they do not prove a
specific codec, allocation volume, or power saving.

## Implemented paths

- Preserve dual PanVK artifacts and OptEarly latch (3/7); no change to
  Mesa, nil semantics, alignment return, private texture-layout object or
  shader ABI. New opt-in p12752=1 requires p12751=1, p1275=1, p1274=1, p1273=1.
- Idle: 96-row RAM recorder on the existing governor (no new kernel thread).
  One explicit P125 phase, 150s config expiry, thermal rows every 2s;
  guard checked up to 5Hz, earlier stop BIG>=67, LITTLE>=66, GPU>=68 C,
  battery>=45, invalid TMU age>2s, display wake or sustained combined
  cluster load >=200 for 500ms. Uses existing P125/P126/P127 revocation.
  Existing hard stop and driver-specific guards unchanged. On unplug,
  autonomous protection remains; no host heartbeat required. The existing
  P125/P127 watchdog bounds a phase which never actually starts.
  Rows include frame-count readback, temperatures, CPU loads, requested
  and last measured rates, mode/readback and validity; no inference of
  physical panel self-refresh or actual DSI/DRAM bytes from frame count.
- BusTrace: additionally reads up to 13 ACPM domains, one GET per 4s;
  raw rate, errno, timestamp and latency. No ACPM SET, no bus/rail write,
  no MIF change. Invalid/zero results retained. Legacy readback may also
  run; this is a diagnostic window, not a pure energy baseline.
- Existing P127 governor PSR (100ms threshold, poll cadence may delay)
  is tested against legacy PSR disabled; P126/P127 driver leases and
  LITTLE949/832/715/598 are tested independently A/B/A. Deeper OPP
  requires an applied prior report and explicit consent; UFS, input,
  OPP, loader, traffic and topology refusal masks remain authoritative.
  Host report is not proof of hardware qualification. No arbitrary
  clock overrides or stacked old leases with P1274.
- Idle52Offline: single nominal 60s disconnect window, existing FG/WFI
  RAM rings. Host validates phase identity, contiguous samples,
  external=0 and negative current at every sample, voltage, link
  timestamps, no wake, thermal validity and >=80% temporal coverage.
  Trapezoidal integral reports only observed interval. Not continuous
  proof between samples, not lab-calibrated power, connected net battery
  is not device power. No automatic repeat or combined policy.
- Scheduler: p12752_profile 1=LOG2,2=REC2,3=REC1,4=REC0 uses the existing
  P1274 recommended-core selector and P1275 EarlyLoad cap/telemetry.
  Keeps all eight online; no core power-state operation. Arm rejects OCP
  on, unsafe or busy UFS for REC, wrong topology or input/conflicts.
  Runtime recommendation returns stock on OCP/UFS/thermal/input/wake/
  expiry/conflict. R1/R0 host requires qualified prior step with explicit
  intervention; no auto progression. Actual scheduler mask/CPU placement,
  wake/restore and thermal qualification use existing P1274 recorder.
  A refusal is evidence; do not disable OCP to obtain REC.
- Workload: bounded in-process PC/LR snapshot of up to two highest observed
  CPU-usage threads, <=20 rounds/process, <=128 enumerated threads per
  round; only selected P12752 burst profile, busy window uptime 290–390s.
  Mediaserverd uses existing injection; allowlisted posters use loaded
  HakiosMetal bundle (no coverage promised for extensions not loading it).
  No task_for_pid, explicit suspend, stack-memory walk, private framework
  patch or broad process injection. Sampling is biased, may have overhead,
  can fail or report unresolved images; not a full stack trace or CPU
  time percentage. Natural early-nil calls record bounded descriptor
  fields and caller chain; alignment logs actual IMP image and inherited
  equality, does not change return. Existing cputop samples add RSS,
  virtual size, cumulative COW/messages and DAS/RBS codes. PID reuse and
  raw lifecycle log correlations must be considered. No codec causality
  asserted. Analyze exports PC/LR, sampler status, layout, memory,
  lifecycle text alongside existing trace and stage evidence.

## Build/validation boundary

drivers/map includes the new header at bsd/dev/hakios.p12752.h.
Installer checks opt-in, paired files and post-apply header identity.
Source contract, PowerShell mock, old suite and installer negative tests
are distinct from native SDK/kernel builds and on-device qualification.
CI includes iOS SDK syntax probes. No native device run in local workspace.

Deferred by design: writes to INT/DISP (pre-use restore not proven), MIF
changes, physical CPD/SICD/CPU_SUSPEND/CPU_OFF, DCS 28/10, full async stack
unwinding, speculative alignment fast-path, MTLTextureLayout/no-copy and
combined auto progression. Report raw/invalid evidence, cool, review each
independent phase before attempting another.
