# Galaxy S9: phase 2 CPU priority A/B

This experiment builds on the verified phase-1 poster_retry repair.
It tests temporary process scheduling priority, not deletion or delaying the
launch of WidgetKit extensions. This is deliberately opt-in.

## Mechanism and limits

On the S9, CalendarWidgetExtension and WeatherWidget can each consume
almost an entire CPU core during startup while SpringBoard is animating.
The new /redwood/bin/widget_boot process watches only those two names.
It waits 15 seconds after first detecting each PID (well beyond launchd's
5-second initial check-in deadline), then optionally sets a positive nice
value, and restores the previous value by 150 seconds after monitor startup.

It never kills, stops or alters chronod, SpringBoard, PosterBoard or any
poster extension. A scheduling priority change cannot guarantee lower
total CPU power, and may postpone widget timeline rendering. If permissions
prevent setpriority, it logs errno; a failed call is not an optimization.

## Boot-args (same image source, two test builds)

| Argument | Meaning |
|---|---|
| widget_nice=0 (or omitted) | Control: no priority changes |
| widget_nice=8 | Candidate: temporary nice 8 for two busy widget processes |
| poster_retry=2 | Keep the verified phase-1 chronod recovery behavior |
| fps_log=1 | Keep frame timings; sios Metal mode already enables this |

Only nice values 1 through 10 are permitted; other values disable tuning.

Logs should show:
- widget_boot: off (widget_nice=0/default) for the control;
- widget_boot: nice=8 ... for the candidate;
- widget_boot: CalendarWidgetExtension[PID] nice OLD -> 8 ...;
- widget_boot: WeatherWidget[PID] nice OLD -> 8 ...;
- widget_boot: ... restored, or a clear errno if restoration fails;
- widget_boot: finished ... at the end of the 150-second monitor window.

A missing applied line means no priority change was confirmed, even if
the candidate boot-arg was present.

## Accurate A/B on a persistent USERDATA partition

Build twice from s9/thermal-widget-phase2-qos, always using the same
trblsg/sios workflow branch fix/s9-installer-device-selection and
userdata_blocks=starlte=13917184. Change ONLY the boot_args
widget_nice=0 vs widget_nice=8. Keep poster_retry=2, Metal, Wi-Fi,
brightness and all other settings the same.

WARNING: sios s9-installer produces a USERDATA-FORMATTING ZIP. Flashing the
full ZIP erases the phone's data and forces a new first-boot workload.
For A/B, with a compatible installed SYSTEM image, extract
boot_starlte.img from each encrypted build, verify its checksum, back up
the BOOT partition, then flash only BOOT through TWRP Install Image ->
Boot. Do NOT select SYSTEM or USERDATA. If the installed SYSTEM differs,
do not mix the images: rebuild a matching pair first.

1. Put the phone into the same ambient temperature, charging state and
   Wi-Fi environment. Aim for initial BIG temperature within 3 C across
   runs; allow 15-30 minutes to cool between trials if needed.
2. Keep display ON continuously for 600 seconds at the same brightness.
   Choose an Auto-Lock setting that permits this without touch input.
   After reaching the home screen, do not swipe, open apps or interact.
3. Collect a fresh complete PuTTY log from boot to at least 660 seconds,
   using a new filename for each run. Raw serial data should be intact,
   and the phone must not reboot unexpectedly during the measurement.
4. Confirm the effective bootargs (widget_nice=0/8, poster_retry=2), the
   widget_boot log, chronod PID, poster_retry line and the actual
   screen displayState on/off transitions.
5. Compare cputop average and peak in 0-60, 60-120, 120-180, 180-300
   and 300-600s windows, the peak and average BIG/LITTLE/G3D temperatures,
   thermal clock caps (BIG 650MHz, LITTLE 455MHz), extension timeouts,
   FPS missed refreshes/CPU-render latency, and widget appearance.
6. Prefer paired A-B-B-A order using the same two BOOT images. If B
   causes blank widgets, stutter or a measurable regression, immediately
   return to A and preserve both logs for investigation.

The change targets contention with SpringBoard; if B shows identical CPU
utilization and temperature but lower render latency, that is a valid
responsiveness result, not proof of energy saving. If UI and heat both
stay unchanged, keep widget_nice=0 and investigate hot-thread stack traces
before attempting aggressive throttling.

Rollback: switch boot_args back to widget_nice=0, or boot the exact
phase-1 image from s9/thermal-widget-phase1. The experiment intentionally
writes no files to the persisted USERDATA volume.
