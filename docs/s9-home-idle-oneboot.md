# S9 Home Idle — one boot, three scenes, reversible source cut

Source branch: `s9/home-idle-oneboot-ab` (parent `s9/p6-unified-wake-thermal-display-lab`, 0ec127b).
Goal: determine WHY primary Home keeps ~10 GLES frames/s, while secondary Home and Settings draw almost none, **after** removing widgets, Smart Stacks and Clock. No assertion that a specific icon, panel or GPU is the cause.

## What is new / what is NOT changed

- `hi=1` (exact, opt-in) wraps SpringBoard's `CADisplayLink` targets in a retaining forwarding proxy. It counts each **callback actually invoked**, grouped by target class/action, once per 5s in `hi: link=` lines. It forwards *every* call unless a manually selected 30-second causal cut is in effect. This wrapper is experimental and must be validated on the phone; `hi=0` / no argument leaves all hooks out.
- The same 1s sampler (5s print) reads *visible* SpringBoard window `_boundContext.commitId` deltas, counts root Home layer `animationKeys`, and prints the stage label. The stage is **manually marked**, never inferred from a screenshot. `commits_5s` is not a count of GPU frames or a guarantee that final pixels changed. Hosted/remote CA contexts are not completely visible from SpringBoard.
- `kern.hakios.hi_cmd` is a tiny kernel sysctl mailbox. It **does not alter kernel rendering, CPU, GPU, timers, PMU or thermal policy**. It exists because `usb>` accepts `sysctl` but not `rwtouch notify`/shell commands. SpringBoard polls it only with `hi=1`.
- The optional causal cut forwards at least one callback per second from **one manually chosen target group**. It is permitted only when marked *main*, Home visible/unlocked and an event-input hook installed. All targets require **no animation keys** EXCEPT the exact observed `SBHClockApplicationIconImageView._timerFired:` source, which may be manually gated despite 6–9 persistent Home layer animation keys. Every other group still fails open. A touch, an animation, switching apps or 30s deadline fails **OPEN** to normal callbacks. It is **NOT** production-ready; unobserved remote animations are a limitation. Never use it to silence arbitrary UI work indefinitely.
- No global `present()` skip, framebuffer hash assumption, display power gate, CPU idle change, GPU undervolt or safety-cap bypass. The G3D `mali_gov_frame` is an **existing, separately reversible** sysctl used for the clock-floor A/B.
- PridePosterExtension's delayed MPSImage/SIGSEGV retry burst can still occur; its crash cause is separate. Record it, do not turn it into a kernel voltage theory.

## GitHub build and byte budget

Build `trblsg/sios` workflow `s9-installer` from the paired feature branch **`fix/s9-homeidle-bootarg-guard`** (keeps the old installer branch untouched), and set:

| Input | Value |
|---|---|
| `kernel_ref` | `s9/home-idle-oneboot-ab` |
| `metal` | `true` (Metal apps; backboardd remains GLES) |
| `userdata_blocks` | `starlte=13917184` |
| `time_limit` | `0` |
| `boot_args` | the **single compact line** below |

```text
hi=1 mali_gov=1 mali_gov_frame=450 mali_probe=1 p3power=1 poster_probe=1 max77705_fg_s=10 max77705_diag=1 max77705_diag_s=30 glstage=1 glcost=1 p0hash=1 p0verify=1
```

This is **163 bytes** of user `boot_args`. The workflow appends/prepends `shub=6 bt=7 fps_log=1 metal=2 camtl=0` where absent; final `-a` extras are about **201 bytes**, versus a new conservative 280-byte maximum. The prior boot's fixed prefix before `shub=6` was 300 bytes; the new estimate is ~501 bytes, leaving ~106 bytes below the 607-byte effective cap. The workflow now **FAILS before expensive setup AND before package creation** if the after-auto-injection `-a` string exceeds 280 bytes. Defaults may change, so the **actual boot log** is still authoritative: demand `hi: enabled`, `max77705: fg:`, and `mali-probe:` before interpreting results. Do not paste the previous long P6 diagnostics line after this one.

**Flash warning:** `s9-installer` normally builds a ZIP that **formats USERDATA**, erasing user data. Back up BOOT, SYSTEM, EFS and USERDATA outside the phone. Match both BOOT **and** SYSTEM to the new branch because the new `inject_sb.c` and `.inc` are in the userland image. BOOT-only flashing leaves the source profiler absent. Do not install until compatible artifact and backups are verified.

## Single-boot protocol (PuTTY `usb>`, physical UI, no computer shell on phone)

Use PuTTY **All session output**, keep brightness at ~70, same charger/USB meter, Wi-Fi state, ambient. Give startup/background PosterBoard activity until roughly **kernel uptime 420 s** before baseline. The numbers `hi: phase=`, `fps:`, `mali-probe:`, `max77705: fg:`, `tmu:` together form one log. Use the physical screen for navigation: any USB input is possible with `swipe`, but manual visual confirmation is essential.

| Step | State to establish | PuTTY command | Wait / read |
|---|---|---|---|
| A | On **primary Home**, without Clock/widget/Stack | `sysctl kern.hakios.hi_cmd=1` | 60s, label `main`, backboardd, FPS, top `hi: link`, USB current |
| B | Swipe to **secondary Home** (screen still ON) | `sysctl kern.hakios.hi_cmd=2` | 60s, label `secondary` |
| C | Open **Settings** | `sysctl kern.hakios.hi_cmd=3` | 60s, label `settings` |
| A2 | Return to primary Home (still no widgets) | `sysctl kern.hakios.hi_cmd=1` | 60s, repeat primary reading |
| D | Same primary Home, only remove Mali's *frame* clock floor | `sysctl kern.hakios.mali_gov_frame=0` | 60s, compare GPU MHz, FPS, CPU and current without other changes |
| A3 | **Restore** previously proven 450 MHz floor | `sysctl kern.hakios.mali_gov_frame=450` | 60s and ensure no regressions |
| E (optional) | With page **main** marked and stationary, choose the **verified Clock group ID** from `hi: link group=N target=SBHClockApplicationIconImageView action=_timerFired:` (in 2026-11-22 first test, N=3) | `sysctl kern.hakios.hi_cmd=103` **only when N=3**; otherwise use `100+N` | Up to 30s, **automatic return**; watch `hi: gate=armed`, `hi: link=...`, FPS, responsiveness, current |
| End | Restore all runtime test knobs | `sysctl kern.hakios.hi_cmd=201` | Read `hi: gate=usb-stop`, keep `mali_gov_frame=450` |

`hi_cmd`: 0/201 cancel, 1 main, 2 secondary, 3 Settings, **101..148** target group 1..48 (e.g. 103 targets `hi: link=3`), 199 top link from the last 5-second sample, 200 dumps the existing `com.hakios.layers` tree once. If a verified targeted command is rejected, **do not force it**: that means the screen/app/input/animation guard did not prove the intervention safe. To reissue exactly the same `hi_cmd` value, first set it to 0 or another value and then back. The debug commands stay in effect until changed; the *cut* itself lasts 30s max.

Do **not** run cut E while moving pages, during an animation or while the lock screen shows. If any visual problem appears, use `sysctl kern.hakios.hi_cmd=201` immediately; if the kernel console has lost characters, wait for the automatic 30s expiry. The full image with no `hi=1` disables the experiment on the next boot.

## Interpretation / action after the one boot

- **Primary `hi: commits_5s` increases; secondary falls:** SpringBoard client CA transactions are the likely next source to trace. `hi: link` points to which actual display-link callback class/action fires at the relevant cadence.
- **Primary `commits_5s` near zero but FPS ~10:** do NOT block a SpringBoard timer blindly. Suspect CA server-side animation, hosted PosterBoard layer or another CA context. Use `hi_cmd=200` once at primary and once at secondary, compare `ANIMATIONS`, `HOST`, window activation/contexts. Remote contexts need separate attribution.
- **A manual cut lowers FPS/current in primary and makes no required UI stale:** that target is a causal candidate, **not** a production fix. Follow with source-specific suppression that preserves real content changes, touch, foreground transitions and lockscreen updates.
- **With `mali_gov_frame=0`, FPS remains ~10 but GPU drops 455→260 and measured current falls:** confirms part of load comes from the governor's 100ms frame-floor rule; safe permanent floor choices require checking swipe smoothness and thermal caps. Fixed 950mV rail means frequency reduction does not imply proportional voltage or watt savings.
- **No difference from either switch:** the heavy work is probably in backboardd compositor, not SpringBoard's frame scheduling or the GPU floor. The remaining fix belongs in scene/layer invalidation or compositing cost, **not** a blind full-frame skip.

A source-level recommendation will be made from this one dataset; neither code nor docs claim a measured watt reduction before a device trial.
