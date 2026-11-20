# S9 G3D Power Lab — one kernel build, many runtime sysctl trials

Branch: s9/g3d-power-lab (parent s9/ioshash4-p1mps-p3power, commit 7023684).
Scope: kernel Mali driver ONLY. The tested P0 IOSurface checksum, P1/P3
userland, Metal, Home Screen and postponed P2 GLES draw/frame_end are unchanged.

## Safety and design

All Lab controls default OFF. BOOT unchanged until the user installs it.
Existing Mali governor, frame floor, cooling/thermal caps 85/95C remain.

The screen-on Home Screen makes about 10 frames/sec. Power-cycling the
entire G3D domain after every frame is too costly and potentially unsafe:
OFF requires GPU reset + Samsung DTZPC SAVE + PMU OFF; waking restores
supply/PLL, DTZPC, GPU cores and MMU. Real gate mode3 only permits
power OFF when there have been no GPU job AND no frame for at least
1500ms, plus 10sec from the last wake. Mode2 is intended for screen-OFF
with 10sec no-job/no-frame threshold.

The worker double-checks queue, job timestamp and frame timestamp
AFTER acquiring mali_job_lock, before OFF. A failed GPU reset now
REFUSES OFF. A failed power-down with the domain still ON triggers
reinitialization before GPU submission resumes. GPU submissions are
blocked if safe recovery is not verified; g3d_lab_reset=1 or reboot
is then necessary. PMU ON alone is insufficient: DTZPC RESTORE and
G3D bus dividers must also be verified before GPU MMIO or job submission.
The independent monitor reads PMU/CMU outside the power domain, never
powered-off GPU registers.

The Exynos9810 APM voltage table advertises 260/600mV,
455/662.5mV, 572/700mV and 598/706.25mV but those are NOT
validated stable minimums on this iOS port. Source warns of an earlier
low-voltage GPU access hang. Therefore the Lab only accepts
950, 925, 900mV, 25mV apart, with serialized PMIC readback.
Reducing voltage is blocked if GPU is OFF, gating mode is active or
automatic DVS is enabled. GPU job errors while undervolted
trigger a 950mV restore attempt and fault latch.

## Controls under kern.hakios

- g3d_lab_gate: 0 normal always ON; 1 read-only shadow candidates;
  2 actual screen-off quiet gate; 3 actual long-quiet gate
- g3d_lab_idle_ms: 500..60000; suggested 10000 in mode2; mode3
  enforces >=1500 no-frame/no-job interval regardless of input
- g3d_lab_mv: only 950,925,900, max25mV per transition; OFF by default
- g3d_lab_rail_probe: 0|1, optional B6CTRL/B6OUT read at most once/30s
- g3d_lab_phase: 0..99, test marker only, changes no hardware state
- g3d_lab_stats: READ ONLY, includes real PMU domain, GPU safe, TZ verified,
  cumulative OFF time, number of OFF/wakes, last/max/mean wake latency,
  rejected/shadow opportunities, faults and latest voltage readback
- g3d_lab_reset: WRITE 1 to power ON if needed, verify GPU core/MMU
  if needed, write+verify 950mV and disable gate. Failure is reported
  as error, never silently considered success

The independent five-second Lab status sampler is a SLEEPING kernel
thread, started when any Lab mode/phase/probe first enabled; it
continues reporting while the G3D job worker and governor are asleep.
Each line includes phase=, PMU domain=, PLL state, target/actual
voltage, last wake latency, OFF counters and error state.

Existing and already proven on S9:
    sysctl kern.hakios.mali_gov_frame=450
That stores 450MHz and drives the supported 455MHz hardware level.

## Single build, single serial log

For trblsg/sios workflow s9-installer.yml use workflow branch
fix/s9-installer-device-selection and kernel_ref=s9/g3d-power-lab;
retain metal=true, userdata_blocks=starlte=13917184, time_limit=0
and all previously functional settings.

This patch is KERNEL ONLY, with no userland ABI modification.
BOOT-only installation is the intended approach, but only after
checking that BOOT and installed root/SYSTEM versions are compatible
and that restore/backup has been verified. A full installer
FORMATS USERDATA: never use that as the default way of toggling a
sysctl. Do NOT flash until kernel build, CI and image are verified.
Keep known-good BOOT/EFS/USERDATA backups and rollback image.
No Lab boot arguments are needed; long boot-args were previously
truncated, so use runtime sysctl.

Start PuTTY ALL SESSION output BEFORE boot. Screen ON brightness70,
same wallpaper, charger, WiFi, ambient and initial temperature.
Wait for mach uptime >=420s to pass the initial Poster storm.

A1 baseline 950mV: three minutes stationary, 0 touch.
    sysctl kern.hakios.mali_gov_frame=450
    sysctl kern.hakios.g3d_lab_phase=1
    sysctl kern.hakios.g3d_lab_rail_probe=1
    sysctl kern.hakios.g3d_lab_stats

B 925mV, 3min:
    sysctl kern.hakios.g3d_lab_mv=925
    sysctl kern.hakios.g3d_lab_phase=2

C1 900mV, 3min:
    sysctl kern.hakios.g3d_lab_mv=900
    sysctl kern.hakios.g3d_lab_phase=3

A2 950mV, 3min (use intermediate 925):
    sysctl kern.hakios.g3d_lab_mv=925
    sysctl kern.hakios.g3d_lab_mv=950
    sysctl kern.hakios.g3d_lab_phase=4

C2 900mV, 3min:
    sysctl kern.hakios.g3d_lab_mv=925
    sysctl kern.hakios.g3d_lab_mv=900
    sysctl kern.hakios.g3d_lab_phase=5

A3 950mV, 3min:
    sysctl kern.hakios.g3d_lab_mv=925
    sysctl kern.hakios.g3d_lab_mv=950
    sysctl kern.hakios.g3d_lab_phase=6

D screen-OFF shadow (does NOT switch power, ~2min):
    sysctl kern.hakios.g3d_lab_idle_ms=10000
    sysctl kern.hakios.g3d_lab_gate=1
    sysctl kern.hakios.g3d_lab_phase=7

Turn screen OFF 45-60sec then back ON; shadow count can grow,
PMU domain stays ON.

E actual screen-OFF gate (~2min):
    sysctl kern.hakios.g3d_lab_gate=2
    sysctl kern.hakios.g3d_lab_phase=8

Turn screen OFF 45-60sec then back ON. Observe PMU domain=0
while OFF, return domain=1, wake count, GPU job success and display.

F screen-ON long-quiet gate (~2min):
    sysctl kern.hakios.g3d_lab_gate=0
    sysctl kern.hakios.g3d_lab_idle_ms=1500
    sysctl kern.hakios.g3d_lab_gate=3
    sysctl kern.hakios.g3d_lab_phase=9

Keep screen ON and static. With ~10fps there should be NO OFF
transitions. Stop and recover:
    sysctl kern.hakios.g3d_lab_reset=1
    sysctl kern.hakios.g3d_lab_phase=10
    sysctl kern.hakios.g3d_lab_stats

Save putty-G3DPowerLab-AB.log. A/B all runs in ONE kernel build;
phase tags allow automatic parsing without reboot or reflashing.

## Stop rules

If G3D domain is partial, fault!=0, gpu_safe=0, voltage readback
differs from target, frame/texture corruption, job timeout, MMU fault,
blank display, or exceptional thermal rise: immediately stop the
current test, issue g3d_lab_reset=1 and verify domain ON,
gpu_safe=1, voltage950, gate0. If not verified (including tz_ok=1 when GPU is ON), reboot/rollback.
Never weaken GPU thermal caps and never intentionally power-cycle
at screen-on 10fps. Power savings in watts remain unproven without
an external current/power measurement.
