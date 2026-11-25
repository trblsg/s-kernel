# S9 P12.7 — bounded driver and energy qualification

Based on P12.6 Test1 071530: BIG IRQ27 4849/3606/5011 per second, wait timeout -30.72%, delayed thread calls ~520/s. These are not electrical measurements.

## Source pairing
Use s9/p12-4-guarded-thermal and fix/s9-p124-thermal-installer; BOOT, SYSTEM, brcmfmac object and host scripts must match. Preserve previous boot arguments; append max77705_fg_s=10. Do not boot with fb_psr=100, brcmfmac_poll=0, CPU_OFF, panel_sleep or new floor overrides. P127 mode 0 by default. brcmfmac jiffies reads monotonic time even outside trial; stock 2ms ticker stays unless selected. The pinned FreeBSD jiffies header is patched exactly and rejects drift. No blanket leeway, CPU_SUSPEND, core offlining, CPD/SICD, recommendation-mask, GNSS ESW or sensor policy changes.

## Modules and boundaries
Each phase keeps P126 COMBINED (15) active via P127 base bit1; one additional module only, no stacking until functional qualification. Lease 420s for BASE or USB, 120s for new hardware/ticker/floor. No renewal. Display-on/input/thermal failure revokes; old conflicting leases refused. P125 aggregate capture <=360s, clipped to policy lease.

| Host module | Mask | Experiment |
| --- | ---: | --- |
| baseline | 1 | P126 COMBINED |
| USB | 3 | existing quiet connected condition, 8ms; count actual 2/8/20ms and bypass |
| ABOX | 5 | empty playback, no amp/headphones/capture, quiet tail; stop AP RDMA0, confirm progress stopped, UAIF1 off; keep ring; event + 10ms watchdog; prime/start/readback before normal 2ms |
| BT | 257 | live owned UART, AFC required; RX/error IRQ, TX wake, 8ms watchdog, return stock 1ms on errors/missed IRQ/full FIFO/empty IRQ storm |
| WIFI | 129 | SPI253 INTx, serialized worker + 20ms watchdog, fallback to stock poll on missed or storm; attach/detach counters |
| TICKER | 513 | monotonic jiffies accessor, nearest registered deadline, 100ms watchdog, mutation wakes, bounded callback burst |
| PSR | 9 | runtime OFF-only VGF0 TE trigger mask; no DCS, no DPU power gate; check shadow/frame race and unmask |
| LITTLE949/832/715/598/455 | 17/33/49/65/81 | lower-only governor floor; all8 online, UFS safe, input quiet, OPP present, busy/load bypass; restore 1053 |

UART source: Scion-Project/android_kernel_samsung_exynos9810 lineage-24.0 arch/arm64/boot/dts/exynos/exynos9810-starlte_eur_open_26.dts, uart@10840000 interrupts=<0 422 0>, FIFO256; include/linux/serial_s3c.h and drivers/tty/serial/samsung.c. GIC INTID454, level. Register settings not hardware qualified. Watchdog fallback does not prove no bytes were lost before detection. ABOX stops only RDMA0/UAIF1, NOT shared AUD power domain or clocks. If DMA stop/resume ambiguous, retain ring and fail closed; cannot promise seamless recovery. PSR masks refresh trigger only, not proof scanout/rail off. INTx attach is not proof traffic delivered. Actual active functionality must be checked.

## Host
Use matched s9-p125-timer-serial.ps1, s9-p12-dual-serial.ps1, s9-screen-lifecycle-serial.ps1. Close other COM clients. No symbol map required in these aggregate trials.

First short qualification (select only desired modules):
```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\s9-p125-timer-serial.ps1 -Scenario EnergyTrials -EnergyModules USB,ABOX,BT,WIFI,TICKER,PSR,LITTLE949 -EnergySeconds 30 -Port COM125 -Execute
```
A0/B/A1 with A1 shared as next A0. Each phase re-arms independent bounded lease. No manual interaction once OFF, except press POWER once when instructed if currently ON. Stop if abnormal. APPLIED means evidence of application, not functional qualification. NO_ELIGIBLE_PATH_OR_FALLBACK is not a pass. Check pairing/HCI/BLE audio, WiFi traffic, audio playback/record/routing, panel wake/flip after short trials. Never combine unqualified modes. Lower floors one at a time after reviewing previous readbacks and storage safety, explicit -AllowLowerFloor; do not jump straight to455. 120s lease, <=60s short host windows.

Long power baseline and USB isolation:
```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\s9-p125-timer-serial.ps1 -Scenario EnergyOffline -EnergySeconds 180 -Port COM125 -Execute
```
Connected A0, unplug B on prompt, reconnect on prompt, connected A1. All hold P126 baseline only; no unqualified hardware changes offline. USB link-down remains 20ms polling, not zero. Kernel guards while disconnected; no automatic renewal. Alternative USB-only A/B/A with -Scenario EnergyTrials -EnergyModules USB -EnergySeconds 180. Long windows 60–300s. Charging state must be matched for quantitative power comparisons; connected/unplug changes input source.

## Evidence and limits
Counts CSV, WFI CSV (7 bins each core, n and ticks, SGI/PPI/SPI, BIG logical3–6), FG CSV (64-entry RAM ring, seq, tick, signed current/avg in uA, voltage uV, external input flag, charger detail, raw QH), report, frozen counter snapshots, raw/host events. WFI bins <100us,100–499us,500–999us,1–1.999ms,2–3.499ms,3.5–9.999ms,>=10ms; report counts AND duration shares. Boundary-crossing waits excluded; WFI observation overhead and scheduled wait duration not electrical residency. FG ring only records active P125/P127 capture; no sample means no power evidence. Signed net battery W = uA*uV/1e12, negative discharge. USB meter input watts and net battery watts differ while charging. QH uncalibrated. No 2.2W static attribution. Cached bus rates (MIF INT INTCAM CAM DISP AUD) read via ACPM every10s on governor, valid bits + sample tick; no rail/domain state claims or bus writes. Sensor enabled mask, applied interval and counts for IDs0/1/4/5/7/9; no client attribution or automatic disables. Driver transition counters include setup/readback outside exact capture; WFI/FG keyed to capture. USB cadence counts are scheduled callbacks not durations. No guarantee target IRQ<1000 or temperature35–45C. Further physical deep idle needs separate resume qualification.

## Validation
Node source/model, Python pinned-header overlay, Windows mock A/B/A, frozen readback and preflight. Native XNU/brcmfmac/installer and hardware tests still required.
