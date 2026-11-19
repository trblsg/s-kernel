# Galaxy S9: GLES compositor and Mali frame-floor A/B

Branch s9/glstage-mali-frame-ab, based on s9/poster-texture-layout-probe.
**Keep the currently working wallpaper unchanged.** The S9 wallpaper/photo picker
does not currently work. No wallpaper or poster service modification is needed.

## Changes

All changes are opt-in and independent. The patch does NOT alter the
GLES synchronization path, thermal limits or BUCK6 voltage.

- glstage=1: one hakios-gl-stage log per 60 GLES frames, with monotonic
  boot uptime, mean draw/presentation/sync/copy/flush/total times in us,
  p95 time bucket, slow (>16.667 ms and >33.333 ms) counts, max frame
  and presented count. No additional GLES/GL fence commands. Intervals are
  CPU wall-clock time, NOT GPU execution time.
- mali_probe=1: one mali-probe kernel log per ~5s while Mali governor runs,
  showing monotonic uptime, time-weighted GPU busy, number of 16ms governor
  windows with frames, windows where frame floor overruled load, windows
  with thermal caps, requested floor MHz, current GPU clock, temperature,
  switches and whether DVS is active.
- mali_gov_frame=N is an EXISTING governor parameter. When mali_max=598 is
  active, omitting frame floor selects the fastest 598MHz level by default.
  We will test explicit top-floor 598 against 455, with 572 as fallback.
  The governor can raise above 455 when jobs need it, and 85/95C G3D
  thermal caps still override both A and B.
- poster_probe=1 from the previous branch continues to sample process CPU
  for backboardd, SpringBoard, two CollectionsPoster instances and more.
  Its own relative t= does not align to governor uptime: use its up=.

Do not combine the test with mali_dvs=1, g3d_sleep=1, mali_irq=1,
metal_layout=1, camtl=1, glwait=0 or glsync=0.

## Build

In trblsg/sios Actions > s9-installer.yml choose workflow branch
fix/s9-installer-device-selection. Set kernel_ref=s9/glstage-mali-frame-ab,
metal=true, userdata_blocks=starlte=13917184, time_limit=0 and retain all
other known-good settings including fps_log=1.

The patch changes the KERNEL and userland GLES shim. The new root APFS
and matching kernel must be present: BOOT-only cannot install the new
GLES instrument unless the updated root binary has already been installed.
Files over 4MiB in the root can be promoted to SYSTEM; inspect build
userland.log for placement. The full installer FORMATS USERDATA and can
erase user files. Back up BOOT, SYSTEM, EFS and USERDATA off-device;
do NOT repeatedly flash the full ZIP to change boot args.

## A/B boot args — ONLY the GPU frame floor differs

Baseline A:
poster_retry=2 widget_nice=0 poster_probe=1 metal_layout=0 glstage=1 mali_probe=1 mali_gov=1 mali_max=598 mali_dvs=0 mali_gov_frame=598

Candidate B:
poster_retry=2 widget_nice=0 poster_probe=1 metal_layout=0 glstage=1 mali_probe=1 mali_gov=1 mali_max=598 mali_dvs=0 mali_gov_frame=455

Fallback C (only if B causes lag or missed refresh):
poster_retry=2 widget_nice=0 poster_probe=1 metal_layout=0 glstage=1 mali_probe=1 mali_gov=1 mali_max=598 mali_dvs=0 mali_gov_frame=572

After installing compatible new root+kernel once, only boot args change
between A and B; BOOT-only flash can be used when paired images match.
No need to reinstall SYSTEM or format USERDATA merely to switch floor.

## Fast log protocol (approximately 6 minutes per trial)

1. Keep wallpaper, background apps, brightness, Wi-Fi, charger, room and
   physical display state the same. Initial BIG/G3D temp within 2C and
   battery temp within 1C. Same battery charger state. Warm boot both.
2. PuTTY > Session > Logging > All session output, start capture BEFORE
   boot and retain the raw log. No input/touch for governor uptime 0-270s.
   0-120s is warm-up; compare the clean steady interval 120-270s.
3. Optional separate interaction window at uptime 270-330s: perform
   identical gestures (e.g. ten home-screen swipes at 2s intervals).
   Record the gesture start and end in text. Compare ONLY equal workloads.
4. Keep logging to at least governor uptime 345s. Save A as
   putty-glgpu-A598.log, B as putty-glgpu-B455.log; C as C572 if needed.
   For causal confidence, use A-B-B-A order when practical.
5. Confirm matching kernel SHA and brightness, intended boot args and
   logs: hakios-gl-stage: enabled, hakios-gl-stage: up=,
   mali-probe: enabled, mali-probe: up=, poster_cost: enabled,
   mali_gov=1 and mali-probe framefloor values matching A/B.
   If governor fails to initialize or probes absent, reject run.

## How to compare

- backboardd CPU/RSS via poster_cost, two CollectionsPoster PIDs summed,
  log process respawns; use poster_cost field up= for uptime alignment.
- GPU MHz samples 598/572/455/260, busy%, thermal capped windows from
  mali-probe; recognize that G3D>=85C can force 455 independently.
- glstage mean draw/present/sync/copy/total wall time; p95bin is an
  APPROXIMATE bucket, not an exact p95. Bucket boundaries in microseconds:
  [5000,8000,12000,16667,20000,33333,50000, infinity].
- CPU BIG/LITTLE and G3D average/peak, hard caps BIG650/LITTLE455,
  fps missed refresh events divided by presented frame count
  (NOT true number of unique lost frames), CPU render thread timing,
  actual touch events, screen state and brightness.
- FPS telemetry timestamps are approximate using adjacent governor
  lines; the new glstage/mali-probe report true monotonic uptime.
- Log overhead is the same in A and B: every 60 frames, every ~5s.
  Avoid glprof during A/B because it suspends the drawing thread.

Stop and restore A598 if B causes flicker, black/partial frames,
GPU faults, repeated extension restarts or significant touch-matched
FPS deterioration. **Never weaken thermal protection for the test.**
A-B-B-A can distinguish random thermal/workload variation from a
persistent performance effect. A single unmatched A/B is inconclusive.

The first iteration is instrumented and uses the EXISTING governor
tuning; it is not yet a change to GL rendering correctness or a
verified reduction in device watts. The stage measurements are intended
to justify a later, focused GLES optimization.
