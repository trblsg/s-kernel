# S9 P10 Lock Screen: Windows COM serial A/B debugger

> **Superseded for natural idle (2026-11-24):** This old tool assumes
> Lock Screen remains illuminated for fixed 16–18-second windows. Real iOS
> blanks it automatically, and the 03:29 host capture lost an OFF event
> between preflight and A0. Do **not** use the older A/B report for power
> attribution or ask the user to repeatedly keep the screen awake. Use
> [P11 natural screen lifecycle](s9-p11-natural-screen-lifecycle.md) instead.
> P10 source code remains available for later targeted profiling with
> valid lock proof; this supersession is host-only and needs no reflash.

This is **not** the original P9 `-P9Fix` physical screen-OFF GPU scenario.
It is a new P10-only source timer A/B/C analysis, reusing the same tested
Windows serial transport and its COM echo/ACK/exact-readback checks.

**Windows files to update from the P10 host-only branch:**

- `tools/s9-home-idle-serial-auto.ps1` — P7/P8/P9 still work
- `tools/s9-p10-lockscreen-serial.inc.ps1` — NEW companion for P10

Keep `s9-p7-power-ab.inc.ps1`, `s9-p8-unified-serial.inc.ps1` and
`s9-p9-power-fix.inc.ps1` alongside them. The P10 companion is only
loaded if P10 flags are supplied, so the P9 path is backward-compatible.

**Firmware pairing:** unchanged P10 XNU + SIOS build pinned to kernel
commit `113658ac950e77707fc59d74a2051238b1731ef7`. You do not
need to build or flash again to update two Windows script files.
New host branch: `s9/p10-lockscreen-serial-ab`.

## Run on a P10 device

First close PuTTY; COM ports are exclusive. Put the phone in its **actual
illuminated Lock Screen** (not Home, not display OFF). Ensure P9.1 remains
stable with `p9_panel_mode=0` and the successful real GPU power gate.
The A/B will not touch physical display, CPU or GPU power.

```powershell
cd C:\Users\Admin\Downloads\ioss9-data2
powershell -NoProfile -ExecutionPolicy Bypass -File .\s9-home-idle-serial-auto.ps1 -P10Lock -Port COM125
powershell -NoProfile -ExecutionPolicy Bypass -File .\s9-home-idle-serial-auto.ps1 -P10Lock -Port COM125 -Execute
```

First invocation is **preview only**. Execute performs one source-level
`A0=policy0 baseline → B1=policy1 exact Clock → A1=policy0 recovery`,
default 16 seconds per phase. Policy1 forwards at least one Clock
callback/second on a proven stable Lock Screen; it does not skip generic
CA, GPU jobs, GL present or DECON updates. Run user-visible A/B without
touch/notifications/animations in order to capture a meaningful contrast.
If the display turns OFF the tool fails closed, prints a reason and
requests `hi_lock_policy=0`. Set a sufficiently long Lock Screen
timeout beforehand if needed.

Optional extra phase for the conservative 15 Hz UIKit animation
timer policy in the **same** capture (not another flash):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\s9-home-idle-serial-auto.ps1 -P10Lock -P10Mode2 -P10WindowSeconds 18 -Port COM125 -Execute
```

This runs `A0/B1/A1/B2/A2`. Mode2 requires zero animations and zero
context commit changes for at least eight seconds and may legitimately
**refuse** to cut any animation callbacks. A zero cut count means
**not proven**, not a secretly successful optimization.

P10-only rollback (P9.1 power policies remain intact):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\s9-home-idle-serial-auto.ps1 -P10Restore -Port COM125 -Execute
```

Or, using PuTTY after closing the script's COM session:
`sysctl kern.hakios.hi_lock_policy=0`.

## Send all three files

The host automatically writes unique filenames:

- `putty-S9-p10-lock-YYYYMMDD-HHMMSS.log` — verbatim serial bytes
- `putty-S9-p10-lock-YYYYMMDD-HHMMSS.events.txt` — host A/B events
- `putty-S9-p10-lock-YYYYMMDD-HHMMSS.p10-report.txt` — source metrics

The report gives per-phase actual `SBUIPreciseClockTimer` callbacks
and `p10_lock_clock_cut`, optional
`UIViewInProcessAnimationManager` and
`p10_quiet_anim_cut`, screen/cover proof, fps presented and refreshes,
passive original WindowServer CA ignored/accepted update counts, and
sampled thermal peak. It does not infer new battery-current savings;
P9.1 previously measured ~0.46 A while screen OFF versus ~0.56 A
before it (user measurement, supply and workload comparability not
independently proven).

**Safety:** the P10 tool does not call `button lock` or enable DCS
sleep, turn off the GPU, park CPUs or write voltages. It validates
`hi_display_requested=1` and panel
`mode=0 panel_sleep=0 panel_asleep=0 display_requested=1`
before starting. It requires matching P10 firmware; the old P9-only
image has no `hi_lock_policy` or `hi_display_requested` sysctl and
must not be used for this source-timer test.
