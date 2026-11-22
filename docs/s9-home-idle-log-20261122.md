# S9 Home Idle — analysis of `putty-S9-home-idle-oneboot.log` (22 November 2026)

This analysis uses the **actual user's** 2,790,009-byte, 22,027-line S9/Exynos9810
serial capture, after the first one-boot Home idle build (kernel `gd0abc0f4`).
The log is a private workspace artifact and is **not committed**.

## Control-path finding (test integration failure, NOT the user's mistake)

- Full launchd boot command line is **485 bytes**, not cut at the old 607-byte
  limit. `hi=1` appears; `hi: enabled oneboot ... input_hook=1` appears at
  line 5758. `max77705_fg_s=10`, governor and CPU observations run.
- Every `usb> sysctl kern.hakios.hi_cmd=N` returned:
  `EPERM: not marked CTLFLAG_KERN, so the kernel may not reach it`.
  The `SYSCTL_UINT(...hi_cmd...)` registration used `RW|LOCKED|ANYBODY`
  but omitted `CTLFLAG_KERN`; hence **no** `hi: mark=` and **no**
  `hi: gate=armed` or callback suppression occurred. The image stayed
  **read-only**, with every callback forwarded (`cut=0`).
- An additional latent gate problem: even stationary Home printed 6–9
  `animationKeys`; the original `!hi_animations_absent` guard would
  always reject the causal cut. Follow-up fix preserves that gate for all
  other targets, allowing an **exact-name match**
  `SBHClockApplicationIconImageView._timerFired:` to be tested manually only,
  for 30 seconds, with the existing input grace, Home scene and
  1 callback/second minimum. Not a production optimization.
- For this original log, scene boundaries can be reconstructed from the
  successful *USB echo* lines 10736 (requested Home0), 13271 (Home1), 14668
  (Settings), 15827 (Home0), 18199 (GPU floor=0), 19698
  (floor=450), 20736 (failed cut). Since `hi_cmd` returned EPERM,
  these are **user-intended** stages, NOT confirmed software phase markers;
  independent FPS/CA evidence below substantiates visible differences.
  Updated `tools/s9-home-idle-report.ps1` supports this historical EPERM
  fallback with a warning.

## Strong independent source attribution

SpringBoard registers seven `CADisplayLink` target groups in this boot.
`link group=3` is `SBHClockApplicationIconImageView._timerFired:`,
requested `fps_req=10`. It fires **51–52 times each approximately 5 seconds**
of stable Home0. It stops during idle Home1 and Settings, then resumes upon
returning to Home0. This is **strong, scene-conditional temporal correlation**
with the roughly 10 presented frames/s. It is **not yet a successful
single-target knockout**: the current one-boot image never actually stopped
this callback, so this evidence does not establish its necessary/unique
causal role.

The user's earlier removal of a Clock Home icon is not contradicted as a
reported action. Presence of an `SBHClockApplicationIconImageView` target
proves the view/callback **exists in SpringBoard**, not that an actual Clock
icon remains visibly on Home0. A rebuilt/rewritten USERDATA layout, retained
offscreen icon view, scene transition, or an unexpected icon elsewhere are
hypotheses, not proven explanations. A layer/view hierarchy snapshot or a
30-second targeted knockout is needed.

Approximate stabilized visual windows read from serial line boundaries:

| Scene (log lines) | Uptime samples | Frames | backboardd | Clock 10Hz | Sum of `commitId` increases per 5s | Mali |
|---|---:|---:|---:|---:|---:|---:|
| Home0 (16000–18180) | 532–624 s | 1001 in 100 one-second FPS reports (~10/report; frames span navigation noise at endpoints) | 20.2% one CPU | ~51.4 ticks/5s | ~515 | 455 MHz |
| Home1 (13280–14180) | 366–439 s | **1 in 73 s** | ~7.4% one CPU | **0** (no link=3 sample) | ~0.7 | settles to 260 MHz |
| Settings (14750–15395) | 465–511 s | **2 in 46 s** | ~4.6% one CPU | **0** | ~1.2 | 260 MHz |
| Home0, frame floor 0 (18230–19680) | 635–701 s | ~10 FPS continuously | ~20.0% one CPU | ~51.3 ticks/5s | ~514 | **260 MHz** |
| Home0, floor restored 450 (19720–20700) | 712–748 s | ~10 FPS continuously | ~20.2% one CPU | ~51.4 ticks/5s | ~515 | **455 MHz** |

Important: the `hi: commits_5s` number is the **sum over around ten
SpringBoard window contexts**, not unique GPU frames or a proof of 515 distinct
CA transactions. It clearly becomes near zero when leaving Home0. First
and last seconds of each manually identified scene may contain transition
bursts. Printed `fps:` samples become sparse during idle (do not average
only the available nonzero lines to infer idle FPS).

## GPU and charging implications

- `sysctl kern.hakios.mali_gov_frame=0` succeeded (serial EPERM does **not**
  affect this preexisting sysctl), and the GPU fell from 455 to 260 MHz.
  `backboardd` remained ~20% CPU and the GPU/compositor still presented
  10 frames/s. The frame floor **magnifies** power cost but is not the
  upstream source generating unnecessary Home0 frames.
- `max77705: fg:` ran every 10s, but the device was close to full charge,
  the current was low/noisy/signed, and it reflects battery fuel-gauge current
  rather than an isolated USB input-watt measurement. Cannot establish
  a causal mA or W saving from this log.
- No verified PridePosterExtension SIGSEGV loop appeared in this new run.
  Therefore the steady 10 FPS Home issue does not require the earlier
  distinct PridePoster crash storm.

## Fix and minimal next causal test

The next `s9/home-idle-oneboot-ab` code revision:
- adds `CTLFLAG_KERN` to `kern.hakios.hi_cmd`, so PuTTY's
  `usb> sysctl` can reach the mailbox;
- preserves the static-animation guard for all other link target classes,
  but allows **only** exactly `SBHClockApplicationIconImageView._timerFired:`
  to be selected for a manual 30s stop/forward-ratio intervention; touch and
  lost visibility must fail open and one timer callback/s is still forwarded;
- updates the report utility and CI with EPERM log regression tests.

For verification, **do not repeat the full P6 suite**. Keep brightness and
Home setup unchanged; in the next matching BOOT+SYSTEM build, wait for idle
Home0 and read `hi: link group=N ... SBHClock...`. Mark main with
`sysctl kern.hakios.hi_cmd=1` and verify **`hi: mark=main`** (not EPERM).
Then `sysctl kern.hakios.hi_cmd=100+N` (e.g. 103 if N=3) and verify
`hi: gate=armed ... clock_override=1`. Observe FPS, `backboardd`,
GPU, current and user input for ≤30s, automatic rollback and
`sysctl kern.hakios.hi_cmd=201`. If cutting that one callback collapses
the 10FPS load on Home0 with UI responsive, optimize the invalidation path
or pause the *offscreen* Clock live icon only under proper visibility rules.
If it does not, inspect another CA source/context and do not blindly suppress
presented frames.
