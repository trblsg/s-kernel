# Galaxy S9: Metal texture-layout + Poster process-cost A/B

Experimental branch on top of s9/widgetless-aegir-ablation.

IMPORTANT: the S9's change-wallpaper/photo picker does not currently work.
These tests DO NOT require or attempt wallpaper changes. Preserve the existing
wallpaper and Home Screen layout on every run. Do not disable PosterBoard,
CollectionsPoster or chronod.

## Features, separate boot arguments

metal_layout=0 (default): both layout SPI and texture-with-raw-bytes stay
disabled, returning nil exactly as before. metal_layout=1: optional 2D-only
SweetTextureLayout-style SPI and no-copy texture import for supported formats.
Unknown texture formats, mipmaps, arrays, 3D and MSAA are refused. ASTC and
ETC block layouts use a copy into an owned GPU texture; ordinary formats use
S9's existing no-copy NBBuffer/textureOnBuffer path. This is experimental:
unit-tested arithmetic is not proof of working Metal/PanVK runtime behavior.
Ported design ideas from DenialOfGravity/ios-redfin, not Adreno drivers.

poster_probe=0 (default): poster_cost exits silently; nothing sampled.
poster_probe=1: rc.jobs starts /redwood/bin/poster_cost 5 120, which logs
5-second PID-specific CPU %, RSS, virtual size, faults, pageins, context
switches and syscall deltas for CollectionsPoster, PosterBoard, backboardd,
SpringBoard and chronod, plus the sampler itself (poster_cost) to estimate
measurement overhead. Each line includes monotonic uptime for alignment.
PID absence and respawns are reported. This is
TOTAL PROCESS CPU time, NOT isolated render-thread time, GPU time, or watts.
Use fps:, cpufreq_gov:, cputop: and process-labeled HakiosMetal logging to
correlate visible render activity. Mali busy/MHz is whole-device telemetry.

## Build

For trblsg/sios workflow s9-installer.yml select workflow branch
fix/s9-installer-device-selection, kernel_ref=s9/poster-texture-layout-probe,
metal=true, userdata_blocks=starlte=13917184, time_limit=0, with other
inputs unchanged from the working widgetless build.

Control A: boot_args = poster_retry=2 widget_nice=0 poster_probe=1 metal_layout=0
Test B:    boot_args = poster_retry=2 widget_nice=0 poster_probe=1 metal_layout=1

Both builds must use the SAME new source and SYSTEM. Installing this new
frontend and sampler initially REQUIRES changing SYSTEM, not just BOOT.
Once installed, a subsequent boot-arg change can be tested by BOOT-only
flashing if paired kernel and SYSTEM remain compatible. THE EXISTING
INSTALLER FORMATS USERDATA. Back up BOOT, SYSTEM, EFS and USERDATA outside
the phone and verify recovery before any full installer run. Do not
repeatedly flash full ZIPs to change one boot argument.

## Measurement protocol; no wallpaper manipulation

1. Use the same unchanged home/lock wallpaper in every run. Match brightness,
   ambient/charging/network conditions and SoC temperature within 1-2 C.
   Prefer warm boots after the install/first activation have settled.
2. Keep the display schedule identical. Record 620+ s PuTTY All session
   output per boot. Screen idle 0-600 s first, then optionally separately
   collect a fixed identical unlock/swipe/Control-Center interaction run.
3. Name logs putty-poster-layout0.log and putty-poster-layout1.log.
   In both require poster_cost: enabled and process data. In B, when
   relevant Metal clients actually invoke texture-layout SPI, expect
   native-metal: S9 texture layout enabled. If absent, the port WAS NOT
   EXERCISED by that run; do not credit performance changes to it.
4. Compare 0-90, 90-180, 180-300, 300-600 s CPU% and RSS for each PID;
   fps render CPU time, missed refresh, GPU MHz/busy, BIG/LITTLE/G3D
   temperature and frequency cap durations. Process CPU is a proxy for
   render workload and includes non-render work; report this limitation.
5. Reject a run with widget extension check-in storms, PosterBoard or
   chronod restart loops, display blackouts, GPU faults or new failures.
   If B regresses, revert the boot argument to metal_layout=0; don't
   delete poster services. Repeat A-B-B-A when possible.
6. Test that standalone Weather, Calendar, Camera, notifications, lock
   screen and Settings still behave as they do on the stable widgetless
   baseline.

This test leaves AegirPoster and WidgetKit bundles absent as in the
working widgetless system. Porting the SPI does NOT restore them.
