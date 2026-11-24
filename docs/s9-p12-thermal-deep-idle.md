# P12 — S9 temperature during rendering + physical idle: integrated experiment

Status 2026-11-24: kernel + matched userland SOURCE changes, with Windows host controller and portable regressions. Not a physical-device pass, and NOT full CPU deep suspend. No flash until matched build/installer and wake tests.

## Track A: smooth, less heat

An opt-in, reversible sysctl named kern.hakios.p12_cpu_mode controls the BIG CPU Scion governor (requires gov_policy=2). Mode0 is stock; mode1 only changes the normalized target from 60% to 70% after at least 30 uninterrupted 11–22ms real presenter intervals, no finger down or recent touch, no recent late frame, display requested ON, and fresh sane BIG TMU 68–83°C. Any 22–50ms late frame holds the old governor target for three seconds. Touch immediately cancels P12 eligibility. Existing frame/touch boost, thermal limits, current limits and ACPM voltage control remain unchanged. It never forces a refresh rate, frame skip, GL/CA suppression or core power-off.

The alternative target might or might not reduce actual heat; only same-gesture A0/B/A1 with sufficient ON samples, FPS and p95/p99 frame interval can establish that. Read-only kern.hakios.p12_cpu_stats reports mode, stable frame streak, eligible/scaled windows, and bypass counters. Scaled count only counts a lower demand request, not measured thermal/power savings.

Backboardd userland injection still calls orig_render_update unconditionally. P12 records real per-batch mean and maximum render cost every 256 forwarded calls, via p12-ca lines, without changing GL/present behavior.

## Track B: real idle power where proven

Optional boot arg p12_gpu_boot_auto=1: after Mali has completed every STEPS stage and its governor has started, P12 requests the already validated P9 automatic screen-OFF G3D gate. It refuses to arm unless the GPU governor is ready, GPU domain awake, memory/protection safe, no competing Lab/voltage/DVS policy, and no latched fault. The existing P9 gate still waits for display OFF, no frame or job for at least 10 seconds and an empty queue, followed by PMU verification, then full restoration before first GPU job. No voltage adjustment, panel DCS or CPU core-off is issued. Boot arg defaults 0; this is an opt-in hardware experiment, not automatic without choosing the boot arg.

The independent G3D Lab monitor continues to sample PMU/PLL even with no GPU jobs. Read only kern.hakios.p12_gpu_boot_stats and kern.hakios.g3d_lab_stats. Genuine physical GPU OFF requires domain=0, pll=0, increasing off_total_ms and wake_fail=0; absence of Mali logs is NOT GPU OFF.

The XNU original cpu_idle WFI/retention instruction is augmented with opt-in passive per-core timestamps, disabled by default. kern.hakios.p12_wfi_probe=1 observes actual WFI entries and accumulated time for CPU0..7. kern.hakios.p12_wfi_stats reports these data with explicit WFI_ONLY, CPU_SUSPEND=0, CPD=0, SICD=0, SYSTEM_SLEEP=0, CORE_POWER_OFF=0. This is NOT deep CPU power-off. CPU3 restart had failed under P7 CPU_OFF, so that path is NOT re-enabled. System suspend and Exynos CPU_SUSPEND/C2, CPD and SICD remain unimplemented; they need validated PSCI resume vector, GIC/IRQ and ACPM/timer handoff before any hardware activation. Do not use sleep_cores=1, syssleep=1 or p9_panel_mode=1 as shortcuts.

## One-build dual host test

Keep existing known P9.1 boot args, cpufreq_gov=1, gov_policy=2 and mali_gov=1. For a GPU auto-off boot test, additionally select p12_gpu_boot_auto=1 before building the matched kernel; do not enable any other dangerous power option. CPU mode1 and WFI observer are toggled at runtime from the same kernel build, not separate flashes.

Windows working directory: C:\Users\Admin\Downloads\ioss9-data2. Two scripts must be downloaded from the SAME P12 kernel commit into that directory:
- tools/s9-p12-dual-serial.ps1
- tools/s9-screen-lifecycle-serial.ps1

PowerShell preview:
    powershell -NoProfile -ExecutionPolicy Bypass -File .\s9-p12-dual-serial.ps1 -Port COM125

Actual run (PuTTY must be CLOSED; COM port exclusive):
    powershell -NoProfile -ExecutionPolicy Bypass -File .\s9-p12-dual-serial.ps1 -Port COM125 -ActiveWindowSeconds 25 -IdleWindowSeconds 120 -OffStableSeconds 25 -WfiProbe -Execute

Natural A0 window: interact with the phone (repeatable touch/swipe).
Natural B window: same gesture, CPU mode1; during a finger down expect fallback to stock, not forced savings.
Natural A1 window: same gesture under stock mode0.
NATURAL_OFF: stop interacting, let iOS auto-blank, leave screen OFF. Host polls read-only GPU PMU/PLL/total off time and optional WFI counters every ~20 seconds once OFF_STABLE. No fake button or fixed Lock Screen-on requirement.

Host emits raw .log, .events.txt, .samples.csv, .lifecycle-report.txt and .p12-report.txt. It explicitly rejects absent P12 kernel replies; host cleanup attempts CPU mode0 and WFI probe0 with echo+readback, even after thermal abort. This is best effort and cannot restore controls if the serial cable loses communication; reboot resets P12 runtime CPU mode and WFI probe to defaults.

Host stop temperatures: BIG82°C, LITTLE75°C, G3D77°C, battery47°C. DO NOT raise thermal stops. Battery fuel gauge when connected over USB cannot be equated to external bench current. Comparable swipe input, render counts, stable ON coverage, late frames, visual correctness and external current must be checked before claiming improvement.

## Acceptance / limitations

- Thermal: fail-open touch + late frame; no lowered FPS, no increased missed frames, no image corruption, lower peak temperature and delta-T under matched load. Current P12 is a conservative governor hypothesis, not removal of known redundant compositor work.
- GPU OFF: PMU/PLL actually domain0/pll0 for a long, stable OFF and returns domain1/pll1 without GPU faults or blurred/torn screen.
- Deep CPU: WFI per-core counters prove clock-idle only. Full PSCI CPU_SUSPEND/C2, CPD, SICD and system suspend are NOT functional here. Their safe wake implementation is a separate engineering gate. All critical always-on/RTC/PMIC/wake-controller domains cannot be OFF if wake is required.
- Build: kernel and userland must come from same P12 commit. No full firmware artifact or physical performance result is claimed from source/host tests alone.

## P12.1 safety after real thermal abort (2026-11-24)

During the second manual swipe run, the P12 host saw BIG84 C,
LITTLE80.9 C, G3D80 C and correctly stopped at the 77 C G3D
hard limit. CPU mode1 had NOT been enabled. The old host cleanup
rechecked already-hot samples while restoring mode0, causing a
misleading "restore not confirmed" error.

New P12.1 host behavior:

1. PASSIVE BY DEFAULT: No swipes, no touching, no automatic wake.
   Preflight requires >=3 live CPU and >=2 live GPU thermal samples
   over 12 s. Start gate BIG<=70 C, LITTLE<=68 C, G3D<=68 C;
   any missing sensor or hotter sample aborts before interactive
   phases. Immutable hard abort limits BIG82/LITTLE75/G3D77/BAT47.
   The observation window also uses early abort BIG76/LITTLE70/G3D72.
   Temperature telemetry delay can still allow overshoot.
2. ONLY in -InteractiveAB, the existing SpringBoard hi_cmd=201
   stops the independent hi=2 autonomous A/B, and a real
   SpringBoard "hi: gate=usb-stop" line is required. Without
   that ACK the interactive test fails rather than misattributing
   cut/recover windows. Cleanup clears hi_cmd=0; automatic
   crossover remains canceled until explicit rearm or reboot.
   Default PASSIVE mode never changes the HI2 policy.
3. Interactive A0/B/A1 requires an explicit -InteractiveAB switch
   AND passing the cool-start gate. DO NOT repeat the previous
   swipe-heavy workload on a hot device.
4. With iOS auto-lock disabled, physically press POWER once to
   switch screen OFF during the NATURAL_OFF window. No fake
   button events are generated. No OFF_STABLE -> failed run.
   Optional -WakeCheck requires the user to wake the device
   physically after >=60 s OFF and checks G3D domain=1, pll=1,
   new wake_count > its preflight value and wake_fail=0; requires
   domain=0, pll=0 and new off_count before that wake. Never equate
   a stale wake counter from a previous test to a current wake pass.
   Do not wake while device is hot.
5. Cleanup sends mode0/wfi_probe0/hi_cmd0 only if that mode
   was changed or was attempted. It bypasses the repeated
   thermal-stop exception only during bounded restore-to-zero
   USB echo/readback; UART failure remains UNSURE, never PASS.
6. A matching new kernel exposes read-only p12_idle_diag:
   per-core idle entry, denied, timer/WFE escape, WFI reached,
   WFI disabled, and idle exit, plus unknown core IDs. This
   distinguishes no scheduler idle from no WFI; NONE of these
   counters proves CPU_SUSPEND, C2, CPD, SICD or cluster power OFF.
7. When used on the already flashed P12 197d148 firmware,
   -LegacyFirmware skips the new diagnostic query and retains
   the host safety improvements. No new kernel flash needed
   just to use the safer host capture.

After the phone has cooled and PuTTY is closed, a SAFE PASSIVE test
on the EXISTING P12 kernel (with both P12.1 and P11 scripts in the
same Windows folder) is:

    powershell -NoProfile -ExecutionPolicy Bypass -File .\s9-p12-dual-serial.ps1 -Port COM125 -Execute -LegacyFirmware -ActiveWindowSeconds 25 -IdleWindowSeconds 120 -OffStableSeconds 25

On a matched new P12.1 kernel omit -LegacyFirmware and optionally
enable -WfiProbe. The P11 lifecycle report header now distinguishes
P11 read-only control from bounded P12.1 kernel mailbox/CPU/WFI
writes; do NOT infer power-domain OFF from a zero WFI count.

Important: This improves diagnosis and protection, not the underlying
touch/scroll thermal cause. No source here forces a GPU/CPU clock
cap, PMIC undervolt, frameskip, PSCI CPU_OFF, panel DCS sleep,
CPD, SICD or system suspend.
