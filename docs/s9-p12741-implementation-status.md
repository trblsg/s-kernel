# S9 P12.7.4.1 — protected late-burst capture (host ABI 2)

## A0 evidence

BASE ended at uptime 420 s. A slope-only trigger at 353.4 s froze the
owner trace at 363.4 s. Actual late burst near uptime 467–480 s:
BIG 88 C and LITTLE 80.9 C; PosterBoard/ImageIO opens Metal in 1411 ms;
PridePoster and CollectionsPoster also open it. cputop 477% aggregate,
PosterBoard 136%. Historical serial backlog before first Dump clock/nonce
read at uptime 517 s, not evidence that Dump caused the burst.

## Protection and recorder

- Existing boot args p1273=1 p1274=1 remain required. Explicit new
  p12741_profile request before uptime 225 s, OFF, 4+4 online, policy 2,
  no conflicting lease, valid thermal and battery conditions. Legacy
  p1274_profile write endpoint removed to fail closed with old host.
  Host reads p12741_revision=2 before writing.
- Absolute recorder deadline 720 s; terminate earlier on existing
  thermal/display/cancel/sensor/conflict guards. TFR starts 230 s,
  eligible 240 s. Existing WFI/FG/identity writer barriers retained.
- Independent guard arms with explicit profile and stays armed until
  reboot, including after cancellation, freeze or screen ON. Releases
  only its own ceilings after 5 continuous cool seconds (BIG/LITTLE
  <=65 C, GPU <=68 C, battery <44 C, fresh valid data). Stock/OCP/hardware
  protection remains. BASE and LOG2 share proactive BIG thresholds:
  70 C or rising at 65 C -> <=1469 MHz; 73 C or rising at 68 C -> <=1261;
  76 C -> <=1066. FAST/intervention earlier 67/70/73 C. LITTLE <=1248
  at 67 C or BIG rising at 68 C, <=1053 at LITTLE 70 C or BIG 76 C.
  Adjacent-zone/battery hard threshold clamps both. Unknown/stale data
  holds or lowers. Min(stock, safety, trial), rounded down to supported
  rate; no CPU_OFF/SUSPEND, voltage, GPU power/panel sleep, radio changes.
- Target 200 ms governor-owned direct BIG/LITTLE TMU sample, not an ADC
  conversion or DVFS latency guarantee. No new kernel thread. Guard is
  not a substitute for hardware shutdown. Not active before explicit arm.
- Multiple independent episode summaries (32, reserve 8 for major),
  40 samples/event at target 250 ms, first ~10 s, full-event peaks.
  Severity 1 slope/isolated sustained load; severity 2 BIG >=70 C or
  sustained high load with BIG sum >=200% and >=67 C; severity 3 hard
  thermal stop. Minor does not consume full owner TFR. First major
  reserves 32 s rolling owner trace, up to 10 s post-trigger or earlier
  safety stop. **Only one full PID/TID owner TFR per boot**; subsequent
  episodes retain compact samples and timeline, not another owner ring.
- Timeline 768, FG 128, advisory events 4096, rate events 1024, full
  detail 128. No overwrite in scalar catalog. Episode headers 5100..5131,
  timeline 6000..6767, user events 8192..12287, episode samples
  13000..14279. Frozen peak/index available before full Dump.
- Live guard status separate from immutable record: both caps, last
  change, transition count, sampled boot-since-arm BIG/LITTLE peaks,
  point-in-time frequencies; not proof of every transient.
- LOG2/FAST/etc suppress the same two unsupported API log families after
  freeze until reboot, without event sysctls/timing instrumentation. BASE
  reverts to baseline outside active observer. No API/layout/fallback
  semantics change. Sandbox or explicit env override may prevent
  observation/suppression.

## Host and qualification

Revision 2, raw log and sync.json clock/nonce fence; backlog before fence
historical or unclassified. Direct live guard after identity and every
16 new pages; original host stops unchanged. Priority episode headers,
hottest timeline, latest events, severe sample prefix, first GPU wake,
closest present BIG owner bin, then scalar remainder and exhaustive
owner/GPU pages. Checkpoint resumes; partial summary has immutable peak
but cannot qualify. Frozen peak vs guard-since-arm peaks separate.
Energy fixed 250–710 s, bracketed FG, USB continuity; no connected wall
watts or QH scale claims. Offline only after connected review.
Qualification needs complete pages, no reported drops, normal reason,
valid power, foreground check, mask restore and both caps released.
B2/B3/R remain prior-qualified only.

Portable policy and actual user header with API stubs are CI/preflight
checks, not iOS SDK/XNU full build. Native build and hardware remain
unverified. No hardware temperature outcome promised.
