# S9 P11 — Natural screen lifecycle and power telemetry (host-only)

**Use this instead of the old P10 18-second Lock Screen A/B harness for normal idle.**
The old P10 host A/B expected the Lock Screen to remain illuminated for all phases. Real iOS blanks it automatically. At 03:29 on 2026-11-24, display OFF happened BEFORE the A0 phase; the old host missed that OFF transition. Its zero cut count does not establish whether P10 power optimization is effective.

P11 uses the existing matched P10 firmware: **NO kernel change, boot arg, new installer, build, or flash.** It is a standalone Windows PowerShell script; existing P7/P8/P9/P10 host files are left untouched.

## Capture naturally

Only one file is needed: tools/s9-screen-lifecycle-serial.ps1 from branch s9/p11-natural-screen-lifecycle.

Close PuTTY because COM125 is exclusive. Save the new script to C:\Users\Admin\Downloads\ioss9-data2 and run:

~~~powershell
cd C:\Users\Admin\Downloads\ioss9-data2
powershell -NoProfile -ExecutionPolicy Bypass -File .\s9-screen-lifecycle-serial.ps1 -Port COM125
powershell -NoProfile -ExecutionPolicy Bypass -File .\s9-screen-lifecycle-serial.ps1 -Port COM125 -DurationSeconds 180 -OffStableSeconds 25 -Execute
~~~

The first command previews. The second collects **180 seconds of normal device activity**, not 18 seconds forced screen-ON per phase. Duration range is 60–1800 seconds; OFF_STABLE threshold 5–120 seconds (default 25).

1. Start on an already booted, preferably cool phone.
2. Optionally illuminate the Lock Screen **once** with physical power button, then leave it untouched. **Allow iOS to turn it OFF** naturally.
3. Keep it OFF for ~40–60 seconds; optionally press the power button **once** to log a normal wake. Keeping OFF the whole capture is also useful for sustained idle; report marks missing wake.
4. Never tap repeatedly to force it to stay ON. Do not change auto-lock duration, send serial button commands or enable P10Mode2.

Host first drains historical boot/USB output and sends **ONE READ-ONLY SYSCTL QUERY**: sysctl kern.hakios.hi_display_requested. Its serial echo and device reply mark the live fence. Older boot lines are excluded from metrics and an OFF event just after the fence is retained, rather than cleared by a later phase setup.

It observes GenericARMPE display on/off requests, classifies ON_HOME, ON_NOT_HOME, ON_UNKNOWN, OFF_EARLY, OFF_STABLE. ON_NOT_HOME is not automatically a verified Lock Screen; a CoverSheetWindow alone does not prove a visible opaque lock. Exact P10 lock proof, if produced by already installed firmware, is separately retained.

## Output files

Four files are automatically generated alongside the new script:

- putty-S9-lifecycle-YYYYMMDD-HHMMSS.log — raw unmodified serial bytes, including any pre-fence historical logs
- putty-S9-lifecycle-YYYYMMDD-HHMMSS.events.txt — capture timestamps, ON/OFF transitions, OFF-STABLE and errors
- putty-S9-lifecycle-YYYYMMDD-HHMMSS.lifecycle-report.txt — segment durations, state buckets and source-specific metrics
- putty-S9-lifecycle-YYYYMMDD-HHMMSS.samples.csv — per-sample readings (timestamp, state, source, CPU, GPU, battery, compositor)

Telemetry tracked: BIG/LITTLE temperature, requested frequency, online core counts and load; mali-power GPU domain/PLL/clock/frame/job ages; mali-probe busy and G3D temperature; max77705 signed battery current; frames and refreshes; backboardd/SpringBoard/locationd/wifid/gnssd CPU when they appear in cputop; CollectionsPoster CPU and RSS; sensor/Wi-Fi/location diagnostic line counts; visible CoverSheet observations and P10 clock/animation proof when present. Presence or volume of sensor/network log lines is **not** direct proof that a component consumes significant power. The report flags overlapping legacy HI2 clock-cut phases and unexpected panel DCS events.

## Interpretation and limitations

- All timestamps are **host serial receive times**, not guaranteed kernel event times. Per-source snapshots occur roughly every 1s/5s/10s, subject to traffic and logger scheduling.
- OFF_STABLE domain OFF observations are *samples*, not proven continuous GPU PMU residency. Report flags fewer than two independent OFF_STABLE PMU samples.
- Missing telemetry is **n/a**, NOT 0 degrees C, 0 mA, or powered OFF. Zero P10 callback cuts is NOT proof of optimization.
- max77705 fuel gauge current is the **battery current**, potentially affected by USB charging. Do not directly equate it to external bench current previously observed at approximately 0.45–0.47 A while OFF. Record the external current separately during the same stable OFF interval if available.
- Safety: default stop at BIG 82 C, LITTLE 75 C, G3D 77 C, battery 47 C. Partial captures and raw logs are kept on abort; do not raise limits just to complete a trial.
- Script **never writes kernel policy**, sends synthetic buttons or changes GPU, CPU, panel DCS, voltage, CPD or SICD. No CPU_OFF. The kernel P9.1 and P10 policies stay as they were.
- Matching P10 image is required: P9-only firmware lacks the read-only display_requested kernel sysctl.

## Next-stage acceptance criteria

Best evidence includes 2+ GPU domain/PLL snapshots, 3+ fuel gauge samples and CPU/compositor samples in OFF_STABLE, plus natural ON->OFF transition. Physical wake is optional. Determine what remains busy while panel is OFF before considering any new firmware changes. In particular, distinguish real GPU PMU OFF from mere low GPU utilization.

Offline Windows regression:

~~~powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\tests\test-s9-p11-lifecycle-serial.ps1
~~~
