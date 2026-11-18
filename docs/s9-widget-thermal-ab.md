# Galaxy S9: WidgetKit/PosterBoard thermal A/B

This experiment only changes `userland/src/poster_retry.c`. It does **not** turn off
chronod, widgets, GPU thermal protection, FairPlay, voicemail, or telephony.

## Why

The 2026-11-18 S9 boot log showed `poster_retry=2` restarting `chronod` even
though PosterBoard already had surviving poster extensions. After `chronod`
reappeared, a second widget-extension launch coincided with another large CPU
load spike. That association is not proof of causality; test on the phone.

## Boot argument (new branch `s9/thermal-widget-phase1`)

| Value | Behaviour |
|---|---|
| `poster_retry=0` | Turn off wallpaper recovery; diagnostic only: wallpaper may remain blank |
| `poster_retry=1` | Default: recover wallpaper if PosterBoard has fewer than two surviving poster processes; never deliberately restart chronod |
| `poster_retry=2` | As 1, but restart chronod **once** if the posters are missing; when posters survive, leave chronod running |
| `poster_retry=3` | As 1, but deliberately restart chronod once even when posters survive; old mode-2 behaviour, for diagnosing blank widget timelines |

Mode 2 and 3 restart chronod at most once during a poster_retry run. The actual
background load of each widget is not fixed here; this isolates one potential
trigger of a second launch storm. There is no change to CPU/GPU frequencies.

## Reproducible boot comparison

The `trblsg/sios` GitHub Actions `s9-installer` workflow accepts `kernel_ref`
and `boot_args` independently: use `kernel_ref=s9/thermal-widget-phase1`.
Keep display, network, brightness, CPU cores, battery/charging state, other
boot arguments, installed widgets, and collection interval as close as possible.

1. Control: `boot_args` includes `poster_retry=1`.
2. Candidate: change **only** that argument to `poster_retry=2`.
3. Diagnostic only, if widgets are blank: compare `poster_retry=3`.

Record at least 10 minutes per boot. Compare `cputop:`, `chronod` PID changes,
widget extension launches and launchd XPC check-in timeouts, BIG/LITTLE/G3D
TMU temperatures, throttled CPU frequencies, FPS and wallpaper/widget presence.
Mode 2 with two or more poster processes should print
`poster_retry: chronod left running`; it must not print a restart line from
poster_retry. Mode 3 deliberately incurs another widget start wave.

**Rollback:** build from `s9/anan-modem` to restore the exact old mode-2
behaviour, or use `poster_retry=1` to avoid deliberate chronod restarts on both
branches. Do not flash or format the phone as part of a code-only experiment.
