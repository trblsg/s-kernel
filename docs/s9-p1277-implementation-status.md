# P1277 / Rev7 — local implementation checkpoint

Status: **NATIVE CI CANDIDATE / NOT HARDWARE-QUALIFIED**. Based on kernel 2cfcf2845f556526fd69d8145625cfbef8fd46f2 and installer 3e79a0080852bafaaf4fffb8a2081172f301ba98. Publish separately on s9/p12-4-guarded-thermal-rev7 and fix/s9-p1277-installer. Native XNU/userland compilation and concurrency/device review remain required before deployment; source publication is not native build success. P12754 audit ABI remains 6; new separate p1277_revision reports 7.

## Implementation and controls

| Area | Source state | Control |
|---|---|---|
| .1 automatic idle | Separate automatic lease, stages 949/832/715/598 MHz with two measured clock/VSEL samples per stage, stock restore before advancing, ABOX stop/idle/resume witness; no assignment to interactive gate qualification bits | p1277_auto_idle=1; kern.hakios.p1277_auto_idle; p1277_auto_stats |
| .2 panel | Persistent opt-in uses Rev6 fresh-frame wake; disarm after fallback or latch/DCS fault, refuse new OFF on unresolved wake | p1277_panel=1; kern.hakios.p1277_panel; p1277_panel_stats |
| .3 USB | Keep real power/link notification; no new suppression | existing Rev6 observations |
| .4 active | Lower-only 1456 MHz touch floor, max 150 ms touch boost and 60 ms M3 burst, existing frame/load/thermal behavior; GPU IRQ default with profile, explicit mali_irq overrides | p1277_active=1; kern.hakios.p1277_active; mali_irq |
| .5 hybrid G3D | Fresh BUCK6 read before wake, 950 mV for reset and OFF, rebase before live publication; 850 mV pilot only when all OPPs fit verified table, checked fallback and sticky retirement | p1277_gpu_dvs=1 grants permission only; kern.hakios.p1277_gpu_dvs=1 explicitly arms; p1277_gpu_dvs_stats |
| .6 G2 | Separate runtime control on existing context-verified bind path, p122 mode 0/hash 0 only, mismatch latches context fault | p1277_g2=1 permission; kern.hakios.p1277_g2=1 explicitly arms |
| .7 and .10 | New one-shot layer-2 coordinator for previously qualified MFC, ISPPRE, DISP200/134; 20s active, 22s driver expiry, 5s restore escalation. One owner only; panel+DISP prohibited. Existing BT/Wi-Fi pilots remain isolated | kern.hakios.p1277_layer2=1..4, 0 cancel; p1277_layer2_stats |
| .8 | Existing read-only ECT/FVMAP/DTS and CMU evidence retained; MIF 2094 vs 2093 unresolved | no new writes |
| .9 | One optional read-only PSCI version and FEATURES(CPU_SUSPEND64) query, cached raw/signed result. EL2/Carbon may answer; does not demonstrate a working resume path | p1277_psci=1; kern.hakios.p1277_psci_stats |
| .11 | Explicit 120s wait for sustained OFF CPU sum >=150 for 250ms, revoke own lease and verify restore before a bounded 10s existing PID/TID recount; one capture per boot, no pretrigger data | kern.hakios.p1277_burst_trace=1, 0 cancel; p1277_burst_stats; existing ThermalTraceDump |
| .12 | Frozen cursor reconnect exact COM, same boot and phase check before retry; additional observed_applied, rollback_observed, qualification_complete fields; typed Rev7Status and offline manifest-driven independent A/B/A aggregation; legacy strict criteria unchanged | s9-p125-timer-serial.ps1; s9-p1277-report.ps1 |

All new boot flags default OFF. Auto idle blocks old manual qualification/audits while enabled or restoration is pending. ABOX automatic evidence is driver stop/resume, NOT a new audible functional certification. Do not infer power savings. The existing LITTLE455, MIF SET, DATA_INIT/SET_CONST, CPU_OFF, CPU_SUSPEND restrictions are not relaxed. Panel cannot be disabled while asleep; wake/restore first. Disable automatic idle and wait for own_lease=0 before old diagnostics. Runtime DVS disable does not force a verified OFF domain awake. Legacy mali_dvs=1 must not be combined with the hybrid permission.

## Candidate boot profile (not an instruction to flash)

The prior six-flag example exceeds the 280-byte S9 payload budget. New compact `p1277=1` is **permission only**, not a boot-time request to apply Auto Idle, panel sleep, active thermal, GPU IRQ/DVS, or G2. The individual kernel flags still exist for local development; the draft installer explicitly rejects those boot-time optimization flags. Optional `p1277_psci=1` separately enables only the read-only PSCI query. With the existing Rev6 flags, real shell tests confirm 258/280 bytes (271/280 including PSCI). Keep P12754 ABI at 6 and report P1277 revision separately in the source manifest.

Draft installer accepts only exact profile with all Rev6 prerequisites, checks new staged header and linked kernel markers, and requires `tools/p1277-kernel.sha` matching the future published exact kernel SHA. The separate Rev7 lock is generated from the exact candidate kernel commit for the user's manual native Action. The existing Rev6 source lock and remote branches remain untouched. Check the actual Action result and matched manifest before flash.

## Further safety review

- Shared sleepable admission mutex across legacy policy arm paths and Auto Idle enable. Exact stopped-lease identity is required before release of the ownership fence. UFS busy revokes the lease; ABOX failure/thermal and prior experiment guards retained.
- GPU job-error reset now retires the DVS pilot and verifies 950 mV before reset while retaining the job lock. A partial PMU-off state cannot proceed to PLL shutdown/lower rail; uncertain restore keeps MMIO/submit fenced. VSEL margin >=25mV required.
- Panel sleep witness is set only after DCS 28/10 success; wake uncertainty retained until DCS 11/29 and brightness restoration. Pending fresh frame, retired or unverified panel conflicts with additional DISP owner. These are software witnesses, not visual proof.
- Layer2 result 1 = completed bounded window with >=20 live observations and verified driver end, NOT energy/functional qualification; 2 aborted, 3 refused, 4 uncertain rollback (sticky auto fault and retained fence). Runtime prerequisites include Auto Idle active and driver-specific same-boot Rev6 qualification. No automatic pilot stacking.
- Burst recorder requires Auto Idle permission/enabled, one explicit trigger request and existing trace state 0. A short burst that ends during restore may yield no capture; no synthetic attribution or pretrigger claim. Existing trace dump includes coverage/drop evidence.
- Rev7Status is read-only, checks boot nonce before/after, reports unavailable module explicitly. Offline report expects manifest `{"version":1,"trials":[{"A0":"a.json","B":"b.json","A1":"c.json"}]}`; paths relative to manifest, optional qch_review/disp_review. No automatic power measurement for the new combined owner, no additive savings.

## Verification

- Existing Node source contracts P127, P1273, P12753, P12754, P127545; new P1277 source contract: pass.
- Boot argument catalogue 741, including compact permission profile. Real installer shell guard tests: 104 PASS, including payload, duplicate/forbidden controls, and independent source lock. Driver map unchanged from prior 144 files/33 subsystems.
- Actual new auto-idle header compiled and executed with fake UFS, clock, PMIC and leases: delayed entry, all four stages, ABOX, lease renewal, display revoke, bad table/PMIC refusal, partial-stage rejection pass. Not a native XNU compile or concurrency/hardware proof.
- Existing PowerShell P12753/P12754/serial-recovery fixtures and new frozen-row reconnect/boot mismatch fixture: pass. No hardware I/O in tests.
- Generated TSV ends in an empty final column; validate exact generated catalogue rather than trimming those tabs.

## Release gate remaining

Native kernel and userland build, concurrency review and actual on-device behavior remain unverified. Layer2 and burst coordinator compiled and exercised in deterministic host mock; typed status and offline aggregate fixtures passed. Real G3D rail/panel, ABOX audible functionality, independent layer2 qualification and measured energy remain to be tested. No native Linux XNU SDK toolchain in the Windows workspace. MIF/INT remains read-only; no CPU suspend/off or LITTLE455. Host aggregate is offline validation, not unattended orchestration of measurements. BT/Wi-Fi remain isolated Rev6 pilots; no automatic optional radio stacking. Candidate handoff: publish exact kernel, separate installer pin and matched host kit including s9-p1277-report.ps1 for a manually dispatched native CI. Check native results and device qualification separately. Do not infer deployment from source publication.
