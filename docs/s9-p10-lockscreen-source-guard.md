# S9 P10: Lock Screen source-level timer guard (one matched P10 BOOT+SYSTEM)

**Built on the physically verified P9.1 safe baseline**, kernel
`s9/p10-lockscreen-source-guard`, parent commit `44f4cec`. No
CPU_OFF/C2/CPD/SICD, PMIC undervolt, GPU gate or panel DCS changes.
The P9.1 panel mode stays **0** on this device; re-enabling DCS sleep
was associated with blurred wake and upper-corner tearing. The actual
P9.1 wake after bypass was normal by visual inspection.

## User-supplied screen-off current, 2026-11-23

Measured current with the screen OFF before P9.1: **0.55–0.57 A**.
New measured screen-OFF current after P9.1 GPU gating + touch/USB
polling policy: **0.45–0.47 A**. Midpoint 0.56 -> 0.46 A is
**0.10 A (100 mA, ~17.9%) less**, assuming comparable measurement
conditions. Different endpoint pairings yield approximately 80–120 mA.
This is an **observed current comparison**, not a battery-runtime
prediction, proof of which subsystem saved each mA, or an electrical
power measurement without the actual measurement voltage. USB-console
connections, supply voltage and battery charge state matter. For
sustained verified battery-energy results measure voltage and current
under the same conditions and log the actual energy over time.

Physical P9.1 success (PuTTY 20261123-123137): GPU PMU+PLL really
OFF for 22,379ms; GPU wake 942us, wake_count=1, wake_fail=0, fault=0,
tz_ok=1, 8/8 CPUs online; no DCS 28/10/11/29 executed;
touch fallback callbacks decreased ~249->31/s and USB ~494->125/s
when display OFF; user reports no tearing and normal wake. After
render is stable, the locked display can nevertheless drive 60/60
frame flips and GPU ~40% busy; backboardd and CollectionsPoster
continue to request work. The screen-off reduction does NOT mean
full CPU deep sleep; scheduler-idle samples cannot prove C2.

## Changes in P10

P10 introduces **two exact, independent source-level modes**, both
initially OFF (`hi_lock_policy=0`):

- **Mode 1** (`kern.hakios.hi_lock_policy=1`): intercept only
  `SBUIPreciseClockTimer._onDisplayLink:` seen at ~15Hz on Lock
  Screen, allowing at least one callback per second **only if**:
  iOS GenericARMPE says the screen ON; a verified
  `SBLockScreenManager.isUILocked` getter says YES; exactly one real,
  non-hidden, opaque `SBCoverSheetWindow` is present; SpringBoard's
  UIKit input hook is healthy; lockscreen proof is <1.5s old; scene
  has been stable >=5s; user input has been quiet >=3s. Recheck the
  getter and scene before skipping any callback. Unknown ABI/state,
  button/touch/input or unlock instantly forwards. A fresh second
  must always get at least one Clock update.
- **Mode 2** (`kern.hakios.hi_lock_policy=2`): includes mode 1 and
  adds only `UIViewInProcessAnimationManager._displayLinkFire:`,
  forwarding at least 15 callbacks/second (not 0) and only if a
  complete scan of all visible UIWindow CA subtrees reports ZERO
  `animationKeys`, ZERO window context commit changes for at least
  8s, all Lock Screen conditions above remain true, and the input
  path is quiet. A live poster, animated layers, uncertain scan, new
  notification, gesture or transition fails OPEN. This is a
  cautious experimental extension. Expect it to refuse on dynamic
  wallpaper, rather than breaking the UI.

`kern.hakios.hi_display_requested` exposes the real iOS display
request read-only to SpringBoard. The kernel refuses policy values
outside 0..2. The new `hi: p10_lock` and
`hi: link=...p10_lock_clock_cut=... p10_quiet_anim_cut=...` logs
identify scene proofs, forwarded callbacks and actual reductions.
The backboardd CA hook adds **read-only** `p10-ca` provenance: when
policy >0, each 256 actual render-update calls report how many
updates Apple's own `ignore_update_p` accepted/rejected, but
`orig_render_update` and the original ignore result are always
forwarded unchanged.

**Not changed:** no skip of any generic `CA::WindowServer`
`render_update`, `GL present`, `fb_swapwait`, `DECON` shadow
latch, GPU jobs, notifications, animations, lock-to-home transition
or power wake. Static lockscreen **60 FPS may remain** if another
source (notably `CollectionsPoster`) drives the frames; this must
be measured rather than falsely reported as solved by a Clock
timer guard. More aggressive poster/frame changes need verified
damage/animation ownership and a safe first-frame restore, not
surface identity or GPU texture upload counts alone.

## Automatic Windows COM A/B capture for P10

The old `-P9Fix` script is **not** the P10 Lock Screen callback debugger.
For the new P10-specific `-P10Lock`, optional `-P10Mode2`, source-level
A/B counters and the generated `.p10-report.txt`, see
[`s9-p10-serial-debug.md`](s9-p10-serial-debug.md). This P10
companion is host-only and can run against the already pinned P10
BOOT+SYSTEM image with **no rebuild or reflash**. It restores
`hi_lock_policy=0` and leaves the verified P9.1 power policy unchanged.

## One paired firmware and runtime A/B, no repeated flash

Build **one matched P10 kernel+SIOS installer**; keep the P9.1
boot args `hi=2 panel=1 mali_gov=1` and never set
`panel_sleep=1`, `sleep_cores=1`, or `p9_panel_mode=1`.
The installer must pin the P10 kernel commit, check its
`hi_lock_policy`/source code and carry `s9_p10_lock_scene.inc`
and `s9_p10_lock_policy.h` as part of SpringBoard injection.
Full installer can format USERDATA: backup BOOT/SYSTEM/EFS/USERDATA.

After boot, close the P9 host script/PuTTY as needed to obtain
exclusive COM125 (do not run two serial clients simultaneously).
With P9.1 controls already restored after reboot as before and the
display ON:

```text
sysctl kern.hakios.hi_display_requested
sysctl kern.hakios.hi_lock_policy=0
sysctl kern.hakios.hi_lock_policy=1
```

Leave the device on an **unmoving Lock Screen** for one normal scene
sample period and observe `hi: p10_lock`, the precise timer's
`p10_lock_clock_cut`, `p10-ca`, fps, temperature and visible
seconds/notices. A source refuser is not a rendering failure;
inspect `cover_locked`, `stable_ms` and method proof before
interpreting no cuts. Compare mode 0 versus mode 1 on the SAME
firmware without flashing again. Mode 2 is an optional separate
run only after mode 1 is visually correct.

**Emergency return to baseline (no reboot):**

```text
sysctl kern.hakios.hi_lock_policy=0
```

If any clock, unlock animation, touch, banner or wallpaper freezes,
set mode 0 and retain the log. Never cover a display wake fault by
forcing render/present skips. P9.1 `p9_panel_mode=0` must remain
unchanged throughout.
