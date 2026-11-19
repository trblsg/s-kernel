# Galaxy S9: no-widget / no-AegirPoster SYSTEM ablation

This is a deliberately **destructive-to-the-built-SYSTEM-image** comparison
branch, NOT a production release. Parent:
`s9/thermal-widget-phase2-qos` with `widget_nice=0`.
It retains phase 1 (`poster_retry=2`) but disables plugin availability.

## Exactly what this test image changes

At build time, after the iOS root is copied into the isolated
`userland/sysroot` directory, `userland/strip_widgets.py` examines
extension bundle Info.plists and paths. It removes:

- every selected WidgetKit or legacy Today `.appex` bundle (including
  WeatherWidget, CalendarWidgetExtension, and other Apple widget extensions);
- the `AegirPoster` bundle, if present.

It also filters these paths from a **copy** of `system.manifest` so that
`mkapfs.py` cannot recreate their empty bundle directories. The build
**refuses to publish an image** if it cannot identify at least one widget
extension AND an AegirPoster bundle. Inspect build logs for
`widget_ablation: READY` and counts.

It intentionally leaves Weather.app, Calendar.app, other standalone apps,
`chronod`, `PosterBoard`, `SpringBoard`, wallpaper infrastructure,
modem, camera, GPU drivers and all temperature safeguards installed.
It does NOT forcibly kill extensions, inject a daemon, or alter USERDATA.

**Important limitations**:

- Existing Home Screen Smart Stacks and widget tiles live in the persisted
  USERDATA / SpringBoard layout. This build does **not** delete layout data
  or reset icon arrangements. Remove all widget tiles, including Weather
  and Calendar Smart Stacks, **in the UI** before using the ablation image.
- Set a static built-in wallpaper instead of Aegir/astronomy/animated
  wallpaper in the UI **before** the test. Otherwise a saved wallpaper
  referencing AegirPoster can make PosterBoard retry an unavailable bundle.
- Widgets supplied by later-installed apps on USERDATA are not in SYSTEM
  and thus cannot be removed from this staging image. Uninstall/unregister
  their widgets manually, or they may still spawn. The log is the check.
- Other Apple framework helpers may still perform non-widget background
  work. No-widget means no *extension bundles of those kinds*, not zero
  total background work.

## Build matrix

Use `trblsg/sios` workflow `s9-installer.yml` from branch
`fix/s9-installer-device-selection`. Keep `metal=true`,
`userdata_blocks=starlte=13917184`, `time_limit=0` and other
inputs identical.

| Run | kernel_ref | boot_args |
|---|---|---|
| A baseline | `s9/thermal-widget-phase2-qos` | `poster_retry=2 widget_nice=0` |
| B ablation | `s9/widgetless-aegir-ablation` | `poster_retry=2 widget_nice=0` |

Unlike phase 2, **SYSTEM differs** between these two runs: the B image
contains no selected widget extension bundles. **Flashing only BOOT
cannot activate this test.** The sios installer ZIP also FORMATS
USERDATA and will ERASE apps/photos/accounts if installed as-is.
Back up BOOT, SYSTEM, EFS and USERDATA on external storage. Do not
flash an A/B installer repeatedly without a restore plan.

For a precise A/B with persisted USERDATA, preserve an identical
USERDATA backup (after UI removal of stacks and selection of a static
wallpaper) and restore the exact same snapshot before A and B as
appropriate. The SYSTEM image must match its corresponding BOOT image.
If unable to safely replace SYSTEM without erasing USERDATA, do
a useful **UI-only first experiment** instead: remove widgets/Smart
Stacks, choose static wallpaper, keep the current stable kernel and
collect a new warm-boot log. This tests the visible-widget contribution
without reinstalling anything.

## Measurement protocol

1. Keep room temperature, brightness, radio/network state, charging
   status, installed apps, startup temperature and USERDATA the same.
   Start within 1–2 Celsius of the other trial if possible.
2. Record a new PuTTY `All session output` log from before power-on
   through >=600 seconds on each run. The control and ablation must
   use the same display-on/off schedule. Do not compare 300 seconds
   screen-on to 300 seconds screen-off.
3. Require `poster_retry: chronod left running` and no
   `XPC_EXIT_REASON_EXTENSION_CHECK_IN_TIMEOUT` storm. Reject the
   ablation run if `cputop:` shows WeatherWidget,
   CalendarWidgetExtension or another widget service running.
   Reject if `AegirPoster[` spawns; inspect missing source path
   rather than hiding logs or adding kill loops.
4. Compare `cputop:` in 0-90, 90-180, 180-300, 300-600 seconds;
   BIG/LITTLE/G3D die temperatures, battery sensor, frequency caps
   (BIG650/LITTLE455), GPU MHz/busy, FPS render CPU time and missed
   refreshes. Inspect Home Screen for blank tiles and wallpaper errors.
5. Repeat in A-B-B-A order if possible; **only one independent change
   per pair**. Record initial temperatures, exact kernel SHAs and
   screen-state timestamps.

**Rollback**: restore paired BOOT+SYSTEM images and saved USERDATA from
the original baseline, or use a full backup. The source branches are
untouched; rebuilding the parent does not delete any Apple bundles.

The kernel power-management problem and GPU floor at 598 MHz are not
addressed by this experiment. It isolates the cost of widget/animated
wallpaper extensions and their launch paths.
