# Galaxy S9 P1 and G1 combined test

Branch: s9/pride-userdict-glcost-p1g1, based on s9/glstage-mali-frame-ab.
The installed widgetless wallpaper and poster providers remain unchanged.
No new shaders, GL draws, fences, GPU voltage or thermal-cap changes.

## Evidence motivating the patch

The old putty-A.log and putty-B.log show a no-touch CPU surge near absolute
kernel uptime 305-330s. PridePosterExtension repeatedly aborts with
NSInvalidArgumentException, missing NBCommandBuffer userDictionary.
Launchd logs six explicit SIGABRT entries in A (crash counter reached seven)
and ten in B (counter ten). Separately, PosterBoard calls an unavailable
ImageIO Metal texture layout SPI 312 times (A) or 347 (B) with
metal_layout=0. Neither log proves the activity recurs every five minutes:
A ran ~14 minutes and showed no second similar surge.

## P1: opt-in private command buffer dictionary

Boot argument p1_userdict=1 enables NBCommandBuffer.userDictionary,
returning a mutable, strongly retained NSMutableDictionary allocated only
on its first request and unique to that command buffer. Repeated calls
on the same buffer return the same dictionary. Initialization is guarded
by an Objective-C synchronization lock. The GPU command buffer's existing
encoding and completion behavior is not modified. The 1st, 100th and
1000th dictionary creations emit a native-metal: P1 userDictionary SPI
line, with creation/read counters.

Default (p1_userdict absent or 0) preserves the prior missing-selector
failure by calling doesNotRecognizeSelector; do not use that setting for
normal phone operation because the old PridePoster crashes are expected.
This is one private Metal SPI only. PridePoster may expose a different
missing method after P1 works: do not consider one missing exception gone
proof that the entire extension is fully functional.

## G1: opt-in GLES slot call-cost profiling

Boot argument glcost=1 adds monotonically timed enter/leave accounting
to the existing EAGL slot callbacks. No GL commands or barriers are added
or skipped. A per-context, per-60-frame histogram reports:

  hakios-gl-cost: up=...s frames=... calls=N measured=...us
    draw=TIME/COUNT texture=... uniform=... shader=... state=...
    vertex=... target=... other=...
  hakios-gl-cost-top: up=...s rank=1 slot=... name=... calls=... us=...
  (and up to two more heaviest slot lines)

These are inclusive CPU wall-clock durations across calls to the GLES
bridge, NOT GPU execution time, thread CPU or physical scanout time.
This report includes calls between frames; its total is not exactly
equal to the glstage draw duration. glcost adds two timer readings per
slot only when enabled, so small observer overhead is possible.
glstage=1 and mali_probe=1 continue working.

The prior poster_cost sampler extends from 5s x120 to 5s x300 samples
(25 minutes) when poster_probe=1 to detect any later repeating bursts.

## Build

In trblsg/sios GitHub Actions use workflow s9-installer.yml, workflow
branch fix/s9-installer-device-selection, kernel_ref
s9/pride-userdict-glcost-p1g1, metal=true,
userdata_blocks=starlte=13917184, time_limit=0. Retain the other
known-good S9 settings. Check CI iOS syntax and full installer build
logs before flashing; portable arithmetic tests alone are insufficient.

The patch changes the Metal and GLES userland frontends and rc.jobs.
A new BOOT/kernel alone cannot install the updated userland. The matching
root APFS must be updated; files larger than 4 MiB may have been moved to
SYSTEM under /usr/local/redwood by userland/build.sh. Inspect userland.log
and image contents to determine actual placement. The existing full
installer FORMATS USERDATA: make verified off-device backups of BOOT,
SYSTEM, EFS and USERDATA before any full install. Never reflash the full
installer repeatedly merely to switch one boot flag.

## One recommended log (compare against existing putty-B.log)

Keep the same known-good 455MHz GPU floor and keep Texture Layout OFF:

  poster_retry=2 widget_nice=0 poster_probe=1 metal_layout=0 p1_userdict=1 glstage=1 glcost=1 mali_probe=1 mali_gov=1 mali_max=598 mali_dvs=0 mali_gov_frame=455

Same wallpaper, physical screen on, brightness 70, Wi-Fi, ambient,
charging conditions and apps as the old B log. Capture PuTTY
All session output BEFORE boot and continue until absolute uptime
at least 1530 seconds. This is one ~25-minute log and does not
require reflashing an A baseline. Leave device untouched until
900 seconds; optional ten consistent home-screen swipes at 900-960s;
then idle to the end. If gestures interfere with cooling diagnosis,
do not touch it for the entire run; tell the analyst which choice.

Save as putty-P1G1-455.log. Require P1 positive marker in
PridePosterExtension (if it reaches that path), hakios-gl-cost:
enabled plus repeated cost/top lines, glstage: up=,
mali-probe: up= and poster_cost: enabled with samples beyond 1000s.

Compare against the older B455 log for:
- Number of PridePosterExtension SIGABRT and service restart events;
  new missing-selector exceptions if the userDictionary blocker moves.
- PosterBoard, CollectionsPoster, mediaserverd CPU bursts near
  absolute uptime ~305-340s and any later repeated bursts.
- The highest-time GLES function slots and draw/texture/uniform/
  shader/state/vertex/target/other group counts in clean idle,
  burst and late steady windows.
- BIG, LITTLE, G3D and battery temperatures, CPU caps, Mali busy
  and actual GPU clock, FPS missed refresh per presented frame.

Uptime caveat: cpufreq_gov: Ns is since the governor started, about
15 seconds later than kernel mach uptime used by
hakios-gl-cost: up=, mali-probe: up= and poster_cost: up=.
Do not compare uncorrected timestamps.

This first iteration is a crash-compatibility patch plus diagnostics,
not evidence of solved recurrent overheating. The matching control
p1_userdict=0 deliberately reactivates the old exception and is
not recommended unless an explicit controlled regression is necessary.

Do NOT bypass thermal protection, unconditionally skip GLES draws,
remove glFinish/fences, disable core PosterBoard, or change wallpaper
to improve the metrics.
