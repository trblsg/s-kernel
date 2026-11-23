# S9 Home Idle: high-resolution single-build investigation (hi=2)

**Recommended Windows fully automatic COM runner:** see
[`docs/s9-home-idle-serial-auto.md`](s9-home-idle-serial-auto.md).
It replaces PuTTY, does not require any manual usb> commands, and
produces raw .log plus .report.txt. The manual steps below are fallback only.

This is **a test branch, not a permanent battery/power fix**. It follows the
real 2026-11-22 22,027-line `putty-S9-home-idle-oneboot.log`: primary Home
draws at 10 FPS / backboardd ~20%; secondary Home and Settings draw almost
nothing; `SBHClockApplicationIconImageView._timerFired:` fires ~10Hz **only in
the primary scene**; total SpringBoard window-context `commitId` advances
about 515 per 5s there. `mali_gov_frame=0` lowered GPU 455->260MHz but
did **not** stop the 10 FPS. The previous `hi_cmd` requests all failed
`EPERM` due missing `CTLFLAG_KERN`, and the old broad animation guard
would have refused the test even if sysctl worked.

## What this new one boot collects, autonomously

Set **`hi=2`**, NOT `hi=1`, for this build. The new mode collects:

1. **Exact timer callback** grouped by target class/action: ticks per 5s,
   forwarded vs cut, requested FPS, group ID. Boot-time creation callsites
   appear for the Clock when present.
2. **Each visible SpringBoard window-context separately:** `hi: ctx`
   includes window class, context ID, commit increase per 5s and current
   commit. Prior totals alone counted ~10 contexts in aggregate. Context
   commits alone do not prove frame pixel changes or identify a stack trace.
3. **Actual Clock icon view's UIKit hierarchy**, once per ~10s when the
   callback fires: `hi: clock_view` includes whether attached to a
   `UIWindow`, whether its projected bounds intersect the visible window
   rectangle, hidden ancestors, effective alpha and view ancestry. An icon
   might have been removed from Home UI but have a **retained offscreen
   ticking view**. Window intersection is NOT proof of visual visibility or
   occlusion, only bounded geometry.
4. **Home window's CA animation keys and layer delegates** every 10s,
   capped to 18 lines to avoid destabilizing SpringBoard. `hi: anim` and
   `hi: anim_summary` distinguish persistent animation metadata from a
   genuinely changing context.
5. **Direct mailbox self-check** at SpringBoard startup:
   `hi: ctl_selftest read_rc=0 value=0 write_rc=0 readback_rc=0
   readback=0` is the desired userland round-trip. The USB kernel route
   is separately fixed by `CTLFLAG_KERN` and verified in CI source;
   a physical USB console success can be confirmed by
   `sysctl kern.hakios.hi_cmd` but is **not required** for the auto test.
   If either control path fails, `hi=2` still records and runs its
   autonomous A/B when the screen is eligible.

### Autonomous causal A/B: no PuTTY commands

After kernel uptime >=180s, with Home visible, display link Clock firing
at least 42 times per 5s in TWO consecutive 5s samples, event hook active
and input quiet for at least 8s, one automatic test begins:

| Serial marker | Time | Clock callback | All other timers/frames |
|---|---:|---|---|
| `hi: auto=START` | automatic | no change | no change |
| `hi: auto=baseline` | 20s | 10Hz original | normal |
| `hi: auto=cut1` | 20s | **at least 1Hz**, the rest dropped | normal |
| `hi: auto=recover1` | 20s | normal 10Hz | normal |
| `hi: auto=cut2` | 20s | **at least 1Hz**, the rest dropped | normal |
| `hi: auto=recover2` | 20s | normal 10Hz | normal |
| `hi: auto=FINISHED` | done | normal, fully restored | normal |

A touch, app/lockscreen transition, loss of Home, stopped Clock timer or
deadline aborts and forwards callbacks normally. If interrupted, at most
two retries are possible after the next stable interval, then passive
diagnostics continue. A code-level 20s deadline ALWAYS permits callbacks
again, even if SpringBoard's periodic sampler stalls. This is an
**experimental bounded callback intervention**, not a safe general-purpose
frame-skipping feature. Other display links and GL/present are never
globally throttled. The user can physically press or open an app at any
time; it cancels the test.

## Build: same two paired repositories

- Kernel: `trblsg/s-kernel` branch **`s9/home-idle-oneboot-ab`**,
  and confirm the CI commit indicated in the assistant's response.
- Installer: `trblsg/sios` workflow `s9-installer` from branch
  **`fix/s9-homeidle-bootarg-guard`**, which checks boot-args byte
  budget BEFORE building and again at packaging.
- Set `kernel_ref=s9/home-idle-oneboot-ab`,
  `userdata_blocks=starlte=13917184`, `metal=true`,
  `time_limit=0`, and the compact `boot_args`:

```text
hi=2 mali_gov=1 mali_gov_frame=450 mali_probe=1 p3power=1 poster_probe=1 max77705_fg_s=10 max77705_diag=1 max77705_diag_s=30 glstage=1 glcost=1 p0hash=1 p0verify=1
```

The input is **163 bytes**; the `-a` suffix including
`shub=6 bt=7 fps_log=1 metal=2 camtl=0` is ~201 bytes against the
conservative 280-byte workflow cap, with the historical default prefix
~300 bytes (estimated total ~501 <607 useful bytes). The **actual kernel
boot line** must show the complete `hi=2` and final token
`p0verify=1`; the loader defaults may vary. Do **not** append old long
P6 arguments.

**Destructive installer warning:** the full installer formats USERDATA.
Back up BOOT, SYSTEM, EFS and USERDATA off-device, verify the compatible
BOOT+SYSTEM image. BOOT alone does not install the SpringBoard diagnostic
dylib. The debug experiment cannot be used on the prior `hi=1` system
image just by switching boot arguments.

## Simple on-device single-session protocol

1. Record **one full PuTTY serial file** and preserve the raw log.
   Keep brightness about 70, same USB input power and screen ON. Verify
   `hi: enabled ... policy=autonomous_cross_over` and `hi:
   ctl_selftest` (the latter is diagnostic, NOT a prerequisite).
2. Unlock and **leave Home main displayed** without touching it after
   launch. Keep the device still until you see
   `hi: auto=FINISHED`. It will not start before 180s kernel uptime;
   the full 20/20/20/20/20 cross-over is ~100s once it starts. No USB
   commands need to be sent.
3. Then swipe to **secondary Home** and stay ~60s; open **Settings**
   and stay ~60s. This supplies the independent scene comparison.
   Optional (only if the USB console works): send
   `sysctl kern.hakios.hi_cmd=2` when on secondary and
   `sysctl kern.hakios.hi_cmd=3` when on Settings;
   require `hi: ctl=RECEIVED` and `hi: mark=...`.
   These commands are **not necessary** for causal A/B.
4. If `hi: auto=WAIT_STABILITY`, stay still at Home main: it lists
   the failing input/home/hook/timer prerequisite. If
   `hi: auto=WAIT_NO_CLOCK_SOURCE`, do NOT assume success: use
   the passive per-context, view and animation logs instead. If
   `hi: auto=ABORT`, read the precise reason, do not force gating
   during navigation. No instructions require an extra firmware build
   just to diagnose an input command problem.
5. Record the external USB-meter current and whether the UI freezes or
   lags in the two CUT periods; SpringBoard's fuel gauge readings alone
   cannot quantify total USB input watts at ~100% SOC.

## Decisive outcomes

- `cut>0`, callbacks forwarded ~1Hz, **CA commits AND FPS both fall
  to near idle during BOTH cut periods**, then return in recoveries:
  Clock callback is a causal driver of the unnecessary compositor
  workload. Prioritize fixing offscreen/hidden Clock invalidation or
  active Home icon scheduling upstream, **not** GPU voltage hacks.
- Clock is outside the window (or has a hidden ancestor) while its timer
  is still ticking at 10Hz: strong evidence of **offscreen timer work**,
  worth a content visibility-specific source fix after confirming its
  causal effect.
- `cut>0` but CA commits/FPS remain 10Hz: Clock is not the only driver;
  use per-window `hi: ctx` lines, animation delegates and other link
  classes to pinpoint another source. Do NOT globally drop compositor
  frames or simply silence all SpringBoard timers.
- CA commits fall but FPS stays 10: a separate server-side CA animation
  or hosted context is probably drawing. Further attribution should
  target the CA server, not SpringBoard's app icon timer.
- No `hi: auto=START`: the log still contains context/animation/source
  evidence, and reports the exact eligibility conditions. Do not claim
  that an unperformed causal test passed.

Analyze the file with `tools/s9-home-idle-report.ps1 -Log
putty-S9-home-idle-hi2.log`; autonomous phase boundaries are parsed.
The old `hi=1` A/B and USB EPERM fixtures remain covered by CI.
