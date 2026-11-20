# Galaxy S9: one-build P1.2 + G2.1 + GLES/IOSurface + Poster diagnosis

Branch: s9/p12-g21-unified-diagnostics, based on
s9/p11-g2-safe-bind-if-redundant (kernel source g3370cc8a).

Goal: one new SYSTEM/root+BOOT installation and one PuTTY log carrying
independent diagnostics, without enabling speculative rendering changes.

## Ground truth

The previous putty-P11G2-455.log had 11 PridePosterExtension SIGSEGVs
following TWO successful NBCommandBuffer.userDictionary calls per PID.
The first call stack goes through MPSCore+0x2b38, MPSImage+0xc044,
MPSImage+0x3b58 and PridePoster+0x5048. The second through
MPSCore+0x41b28, MPSImage+0x21c0/+0x1d400/+0x3c1c,
PridePoster+0x5048; SIGSEGV follows without commit/finish/dealloc
breadcrumbs. We still lack the crashing thread's PC/FAR and binary UUID.

The earlier G2 profiler attempted about 235k texture binds and skipped
ZERO. The normal forwarded driver call also increments the invalidation
counter by design: invalidations near attempted do not prove a bug.
The idle EAGL path has near-zero texture refills but surface_changed()
still sometimes hashes IOSurface contents. That check may cost CPU even
if the texture is not uploaded.

## New opt-in steps in the same binary

P1.2: p12_diag=1. Log native-metal-p12 per buffer: dictionary read
sequence, age since creation, item count, command buffer commit status,
queue pointer and thread. No dictionary values, no Metal changes, no
signal handler. Must keep p1_userdict=1 and p11_trace=1 if observing
PridePoster. This is a diagnostic only, NOT a SIGSEGV fix.

G2.1: g21_diag=1. Every 60 frames print hakios-gl-g21 with attempted
texture binds, client_same/diff, cached native same/diff/invalid,
raw internal binds, forwarded external binds, texture deletes and
outside-target/unit counts. This works with g2_bind=0, so it can
characterize possible redundancy WITHOUT skipping any driver call.
The old G2 algorithm remains opt-in via g2_bind=1 but is NOT recommended
for the initial run.

GL path: glpath=1. Sample one in sixteen glBindTexture and
glDrawElements calls. Report native GL bind duration, image_of lookup,
IOSurface seed/content check, actual upload, native draw duration,
bridge-side draw preparation/cleanup, and the already measured raw
glFlush duration. All are CPU wall-clock, never GPU execution or watts.
No new GL commands, fences, readbacks or extra render work.

IOSurface checksum: glhash=1. Sample one in sixteen actual checksum
passes of surface_changed, report bytes, elapsed time, unchanged and
changed hashes and early skips. No changes to hashing or copy decisions.
This specifically tests why glBindTexture can be slow with zero refills.

Metal texture layout requests: p_layout_probe=1. Log the first and
every 64th private layout request with descriptor size, format, storage
and caller image offset. With metal_layout=0 the method STILL RETURNS
NIL, but old identical per-call error logs are coalesced to reduce UART
pressure. Since logging volume changes, PosterBoard CPU comparisons
with old builds cannot prove P1.2 caused the difference.

Already active and retained in this build: poster_probe=1 (5s process
samples to 25 minutes), mali_probe=1 (5s governor decision summaries),
glstage=1, glcost=1, fps_log=1, CPU governor and TMU. No extra
always-on profiling thread or new thermal/IRQ changes.

## Installer

Use trblsg/sios workflow s9-installer.yml on workflow branch
fix/s9-installer-device-selection, kernel_ref:
s9/p12-g21-unified-diagnostics, metal=true,
userdata_blocks=starlte=13917184, time_limit=0. Retain all previously
working device and FPS settings.

Check iOS SDK syntax CI AND the full installer build before flashing.
This changes EAGL and Metal userland binaries: BOOT-only cannot install
it. Inspect userland.log for location in root APFS, or promoted SYSTEM
(for files over 4MiB). Full installer FORMATS USERDATA: preserve a verified
off-device backup of BOOT/SYSTEM/EFS/USERDATA and a rollback plan.
Do not repeatedly full-flash only to change boot flags.

## One recommended 15-minute capture

Combine this as one space-separated boot-argument string:

    poster_retry=2 widget_nice=0 poster_probe=1 metal_layout=0
    p1_userdict=1 p11_trace=1 p12_diag=1 p_layout_probe=1
    glstage=1 glcost=1 glpath=1 glhash=1
    g2_bind=0 g2_verify=0 g21_diag=1
    mali_probe=1 mali_gov=1 mali_max=598 mali_dvs=0 mali_gov_frame=455

Use the same wallpaper and Home Screen, panel ON, brightness70, WiFi,
charge and ambient as the prior P11G2 test. Aim for SoC starting
temperature within 2C and battery within 1C of the previous log.

PuTTY -> Logging -> All session output, start before boot.
Do not touch/change the display until absolute mach uptime >=930 seconds
(15.5 minutes), then save putty-P12G21-unified.log. For a rapid answer
the first 720s already cover the Poster burst and GLES idle phases;
the extra 210s give a heat/cooldown tail. Absolutely no gestures during
this run: the old 421-touch burst at t817-847 caused hard CPU/GPU
thermal caps and would confound the comparison.

Check for: native-metal-p11, native-metal-p12, native-metal-playout,
hakios-gl-g21, hakios-gl-path, hakios-gl-cost, hakios-gl-stage,
mali-probe, poster_cost and tmu/cputop/fps. The g2_bind=0 control is
important: G2.1 measures reason counts but never skips real binds.
Run read-only tool:
tools/s9-diagnostic-bundle-report.ps1 -Log putty-P12G21-unified.log

Compare fixed uptime phases 120-270s idle, 295-340s delayed Poster,
340-420s recovery, 420-600s steady, 600-900s late idle. CPU governor
uptime in old logs lagged absolute mach uptime ~15 seconds; align
governor samples accordingly. Actual conditions can vary per boot.

## Crucial external artifact for P1.2

Serial normal-call traces cannot identify the SIGSEGV instruction.
Please locate PridePosterExtension*.ips from CrashReporter if available,
commonly under /private/var/mobile/Library/Logs/CrashReporter or
/private/var/logs/CrashReporter. Actual location/availability varies.
Need crashed thread, exception subtype/FAR, PC, and binary UUID to
symbolicate MPSImage/MPSCore. Do not install global SIGSEGV trapping or
disable AMFI/thermal protection. Share only the relevant diagnostic
crash report, not unrelated personal data.

## How to interpret the result

If client_same is high but cache_invalid is high: look at raw internal
binding counts before modifying cache lifetime. If both client_same
and cache_same are low: textures truly alternate and skipping binds
has little potential benefit. If GL bind native time dominates, profile
the driver texture switch; if seedCheck/GLhash dominates while uploads
remain zero, optimize IOSurface checksum policy instead. If GL draw
bridge overhead dominates, inspect vibrant/group preparation.
No direct removal of glFinish, glFlush, real draw calls, thermal caps,
or poster providers based solely on CPU timer data.
