# S9 P12.6 bounded driver idle experiments

Evidence: P12.5.1 055221, exact matched symbol map. BIG WAIT_EXPIRE
290179/60.116s, known kernel 249724. Top entries: lx_thread_runs 91545,
bt_tty_pump 53580, haptic_thread 29635, abox_thread 29459, shub_thread
16904, touch_thread 13895. PID9 likely gnssd 28155 (same boot cputop,
not task-generation proof). S9 haptic is MAX77705, not POCO haptic.c.

## Scope / safety
Default mask 0. Independent, nonrenewable 120s OFF-only kernel lease,
same P125 TMU freshness and early thermal thresholds (BIG76/LITTLE70/G3D72,
battery47). Screen ON revokes; expiry checked at use and governor guard.
No CPU_OFF, CPU_SUSPEND, CPD/SICD, voltage, IRQ routing or CPU recommendation
mask changes. No global leeway. Existing runtime/host safety interlocks
remain. A pending wait can last up to its bounded watchdog before stock
cadence resumes; never requires a host or new event for rollback.

| Mask | Trial | Actual implementation |
|---:|---|---|
| 1 | HAPTIC | MAX77705 idle event wait, 100ms watchdog, active 2ms and 30ms tail/1000ms cap retained; atomic publish + registered wait/recheck |
| 2 | WIFI | LinuxKPI empty workqueue waits for existing enqueue wake, 100ms watchdog instead of 5ms; lock unchanged |
| 4 | GNSS | matched kernel/userland readiness ioctl under shub_lock, bounded 1–100ms interruptible wait, existing nonblocking record parsing and monotonic deadline, legacy/disabled fallback 2ms (capability retry 250ms) |
| 8 | TOUCH | 16ms fallback only screen OFF, no fingers, IRQ pin high; stock 4ms on ON/active; existing p9 knob otherwise unchanged |
| 16 | LEEWAY | 1ms leeway only OFF, no fingers/pin high touch fallback; no global timer change |
| 32 | SHUB | 8ms instead of 4ms only ordinary runtime pump, no enabled sensor, GNSS reader or pending command/rate; firmware/active protocol timing retained |
| 15 | COMBINED | 1+2+4+8 |

The LinuxKPI ticker stays 2ms: its jiffies is a globally read variable
updated only by that ticker, imported FreeBSD/Linux driver header uses not
fully audited. Log TID/name/actual entry once on creation, then distinguish
ticker, IRQ poller, workers. No switch of brcmfmac INTx routing at runtime.
BT retains original 1ms fallback, no new UART IRQ register writes: source
has 256-byte FIFO at up to 4Mbps and no verified RX IRQ route yet. Collect
idle/busy, RX, TTY dequeued TX and existing UART error branch counters.
ABOX retains 2ms DMA cadence; HAL StartIO/StopIO sends a telemetry-only
hint (0 unknown, 1 idle, 2 running), not a trusted policy input. Observe
eligible quiet ticks (speaker hint idle, empty software ring, amp/headphone
off, mic not wanted, quiet tail elapsed). Never stop RDMA using untrusted
hint. No scheduler recommendation experiment yet; requires validated
mask ownership and separate latency/binding/WFI checks. These omissions
are intentional; do not claim full 55–65% IRQ reduction or deep idle.
Driver hits are eligibility/application counts, not watts or successful
wake counts.

## Build
Same paired branches and boot args as P12.5.1. BOOT/SYSTEM must use same
kernel checkout including gnssd/audio HAL; no kernel-only substitution.
No extra boot argument. Installer preflight runs P126 source contract.
Matching new symbol artifact for detail captures. Local tests are static
source and host mock tests, not native XNU or hardware qualification.

## Trial
Download matched s9-p125-timer-serial.ps1, s9-p12-dual-serial.ps1,
s9-screen-lifecycle-serial.ps1. Close other serial clients. Preview by
omitting Execute. Keep screen OFF; turn off once when prompted, no swipe.
First use:
```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\s9-p125-timer-serial.ps1 -Scenario DriverTrials -DriverModules COMBINED -DriverSeconds 30 -Port COM125 -Execute
```
Three aggregate phases A0/B/A1; default 25s OFF-stable, 30s each phase,
small gaps for guard/readback. No detailed page reads, no symbol map
required. Per CPU counts and coverage, p126_diag cumulative per phase,
p126_status hits and restored mask. If individual attribution needed:
```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\s9-p125-timer-serial.ps1 -Scenario DriverTrials -DriverModules HAPTIC,WIFI,GNSS,TOUCH -DriverSeconds 30 -Port COM125 -Execute
```
Shared stock A1 becomes next A0; select only needed modules. LEEWAY/SHUB
separate optional; NO_ELIGIBLE_PATH is not a pass. COMBINED applied=True
means at least one path hit; inspect all hit counts. Hits/diagnostic deltas
include short boundaries outside P125 probe; IRQ rate uses kernel lease
duration. Lease revocation or safety abort invalidates B; cleanup restores
mode 0; if disconnected wait bounded expiry. Check actual Wi-Fi traffic,
GNSS fix/start/stop, touch after wake, haptics; screen ON policy revoked.
Functional verification required before permanent enable.
Retain all putty-S9-p125-timer-* files and compare per-CPU IRQ,
wait_armed, setpop bins, temperature and functional telemetry.

## Page readback recovery
P125 detail unchanged except 80ms pacing between each select/read and
after each read. Save each verified page to phase.pages.txt, scalar checkpoint
phase.frozen.clixml. If read fails, reconnect once; no new probe or reboot:
```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\s9-p125-timer-serial.ps1 -Scenario FrozenDump -ResumeSnapshot .\putty-S9-p125-timer-YYYYMMDD-HHMMSS.A0-END.frozen.clixml -Port COM125 -Execute
```
Requires same stopped probe identity, counters, bitmaps and coverage;
only missing pages queried. Copy both checkpoint and pages file together.
Does not recreate original A/B delta without original baseline. No automatic
rearm, no hidden retry of failed USB commands. Detail optional; 055221
already identified major sources. No new detailed capture required for
initial driver trials.
