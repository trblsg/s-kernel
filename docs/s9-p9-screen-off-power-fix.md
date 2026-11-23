# S9 P9: screen-off hardware fixes (one paired BOOT+SYSTEM, runtime toggles)

Branch: `trblsg/s-kernel` `s9/p9-screenoff-power-render`.
P8 CPU fault interlocks are retained. **Do not use P7 CPU_OFF / sleep_cores=1**.

## 2026-11-23 real-device wake regression and immediate P9.1 HOST-ONLY mitigation

The P9 12:16:37 COM run succeeded at real GPU PMU+PLL OFF for **22,682ms**,
physical GPU wake in **973us**, 0 GPU faults, all 8 CPU cores online.
Fallback touch and USB polling dropped from approximately 250/493 to
31/125 callbacks per second when screen OFF. However, **the user saw a
blurred flash followed by upper-corner tearing during display ON**.
The RAW log proves DCS `0x11/0x29` completed **before the first GPU
post-OFF job**; after wake, the first rendered interval missed 27/57
refreshes with the longest gap 67.6ms. S6E3HA8 driver still relies on
the bootloader's DSC/TE configuration and does not reinitialize the
complete panel state after sleep-in. Whether the corner tearing is due
to retained TE/DSC state, prematurely shown stale GRAM or compositor
frame timing is not yet individually proven.

**Mitigation requiring NO NEW BUILD OR FLASH:** replace ONLY the Windows
host files `tools/s9-home-idle-serial-auto.ps1` and
`tools/s9-p9-power-fix.inc.ps1` from the updated P9 branch. Run
`-P9Restore -Execute` with panel requested ON first, because the prior
P9 test left ALL P9 policies active. This updated script first disables
panel DCS, then restores GPU/touch/USB/Clock. Next `-P9Fix -Execute`
keeps `p9_panel_mode=0` **by default**, verifies the panel is awake,
and tests real GPU OFF/ON with DCS 28/10/11/29 completely disabled.
It rejects unexpected DCS logs. Device must be cool, 8 cores online,
and COM exclusive. On-screen visual verification is still required.
Do NOT manually set `p9_panel_mode=1` on this hardware until the wake
pipeline is fixed and tested with a safe first-frame latch/TE sequence.
This mitigation preserves GPU gating benefit but deliberately does NOT
claim panel power-down. If tear persists with DCS off, next target is
DECON VGF0/TE shadow latch and first-frame presentation, not CPU idle.


## What this *changes*, beyond P8 passive logging

1. **Real automatic G3D OFF.** `kern.hakios.p9_auto_gpu=1` activates the
   existing P7-validated Mali power OFF/ON sequence, but only when the
   GenericARMPE display request is **really OFF**, GPU job AND frame ages
   are >=10000ms and the worker queue is empty. The power decision checks
   display state before **and after** taking the GPU job lock, and keeps
   voltage at the proven 950mV; `g3d_ensure_on()` restores SMC/DTZPC,
   PLL, MMU, GPU cores before a new job. Mode0 verifies physical GPU ON
   before clearing the gate. P7 proved two real OFF/wake transitions,
   47,317ms total OFF and zero faults in one lab window; P9 long-run
   reliability/power savings remain unverified on actual hardware.
   `g3d_lab_gate=0`, `g3d_sleep=0`, or `g3d_lab_reset=1` override the
   P9 policy deliberately. Avoid P8 runner on a P9 active policy because
   its cleanup intentionally sets `g3d_lab_gate=0`.

2. **Experimental Samsung panel sleep/wake, now disabled in the safe host script.**
   `kern.hakios.p9_panel_mode=1` requires initialized `panel=1`.
   iOS backboardd's existing display request triggers DCS
   `0x28 -> 0x10` (screen OFF), `0x11 -> 0x29` (ON), with the
   existing S6E3HA8 key/brightness/delay sequence. `p9_panel_stats`
   distinguishes requested display state and driver's `panel_asleep`.
   A runtime disable is refused while display requested OFF, avoiding
   accidental illumination; turn display ON then disable. This is
   **not** proven DECON/DSIM clocks OFF or panel power rails OFF: do
   not write unverified registers or claim full display power-down.
   **Real device produced a blurred flash and upper-corner tear on wake.**
   Safe P9.1 Windows host now forces `p9_panel_mode=0` before GPU gating.
   On missing display/black screen, use COM rollback and known-good image.

3. **Stop needless periodic work.**
   `p9_touch_off_ms=32` changes S6SY761's *fallback* polling from
   4ms (250/s) to 32ms (~31/s) only while iOS asks display OFF and
   no finger is down; display ON and active touches keep 4ms.
   The pre-existing `s6sy761_irq=1` hardware IRQ path is preserved but
   NOT silently enabled: its electrical wake path needs independent
   board verification. `p9_usb_off_ms=8` relaxes USB polling only if
   display OFF, connected and **no queued log bytes or screenshot**
   (normal USB console remains responsive). Disconnected USB already
   sleeps 20ms. This does not assert true CPU wakeup rates.

4. **Reduce a real upstream timer.**
   With `hi=2`, `hi_policy=2` forwards the exact allowlisted
   `SBHClockApplicationIconImageView._timerFired:` at >=1Hz instead
   of ~10Hz only while the Home window is NOT shown, touch/event input
   is quiet for >=3 seconds and the SpringBoard hook is healthy.
   It immediately forwards at normal rate when Home reappears,
   input occurs, or target/owner is unknown. This is NOT blanket
   suppression of CoreAnimation/vsync/GL, nor a proven cure for all
   animated lockscreen frames. A 60-frame / 2460 draw interval with
   0 texture uploads does NOT prove no transform/alpha changes.
   Existing verified-on-Home policy=1 remains unchanged.

5. **Bound the wake heat transient.**
   `p9_wake_guard=1` optionally caps BIG at <=1469000kHz for the
   first 4000ms after an observed display OFF->ON transition.
   It is lower-only, combined with existing P8 and thermal caps.
   It does NOT raise the 82C Windows safety stop, modify CPU voltage,
   park M3 cores or enable unsafe `CPU_SUSPEND`. Performance impact
   and efficacy need physical verification.

These are independent, reversible runtime policies, default 0. One
paired P9 kernel+userland image is sufficient for all trials. The
physical kernel root system-sleep path is still disabled; P9 does **not**
claim deep CPU C2/CPD/SICD. Enabling P7 `p7_cpu_park=1` stays EPERM
after the real M3 CPU_ON reentry failure on 2026-11-23.

## One boot + direct COM validation

Build SIOS on its P9 installer branch using the pinned P9 s-kernel SHA,
`metal=true`, `userdata_blocks=starlte=13917184`,
`time_limit=0`, and retain `hi=2 mali_gov=1`. Ensure panel driver
`panel=1` is present in the complete boot args (do not duplicate
conflicting defaults). No `sleep_cores=1` boot arg is required.
**Flashing a full installer may format USERDATA**: back up EFS, BOOT,
SYSTEM, USERDATA and verify your matching image before use.

On your Windows COM host copy these P9 branch files together into the
same directory: `s9-home-idle-serial-auto.ps1`,
`s9-p9-power-fix.inc.ps1`, plus the existing
`s9-p8-unified-serial.inc.ps1` and
`s9-p7-power-ab.inc.ps1`. Close PuTTY: COM125 must be exclusive.

Preview (does NOT touch COM/device):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\s9-home-idle-serial-auto.ps1 -P9Fix -Port COM125
```

When the device is cool (<75C BIG), screen ON and 8 CPUs online,
run one functional pass:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\s9-home-idle-serial-auto.ps1 -P9Fix -Port COM125 -Execute
```

The updated P9.1 runner confirms direct COM echo+kernel ACK+readback
for all controls, active 4+4 CPU cores, GPU domain+PLL+TZ safe, and an
initialized Samsung panel. It explicitly **disables DCS sleep**, checks
`mode=0 panel_sleep=0 panel_asleep=0`, then performs one OFF->ON
transition and validates real G3D PMU+PLL OFF then successful GPU
core/TZ wake with no fault. On ANY failure it tries
independent hardware restores, keeps all logs, and never marks success.
On complete success **policies stay on** (diagnostic rings stop).

To restore all policies (bring display ON first):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\s9-home-idle-serial-auto.ps1 -P9Restore -Port COM125 -Execute
```

If screen doesn't wake, GPU fault appears or physical display stays
black, stop further tests and use the known-good BOOT+SYSTEM/Recovery
rollback. A sysctl ACK, DCS packet log or PMU domain bit alone is not
an electrical watt measurement; externally measure USB/battery-only
current only after functional stability is proven.

## Success criteria vs. what is still open

- The success flag MUST include G3D PMU=OFF and PLL=OFF while display
  request=OFF and a verified GPU ON, safe=1, TZ=1, wake_fail=0 after ON.
- Safe P9.1 session must show `panel_sleep=0` and **no** DCS sleep/wake
  packets. Visually confirm that startup flash and corner tearing stop;
  PMU/console logs alone do not prove the picture is correct.
- During OFF, touch poll should fall from 250/s to approximately31/s
  if fallback mode used; 8 cores remain online, no XNU CPU_OFF.
- Static Home/lockscreen improvement from the exact Clock source is
  limited. Work still remains to prove the *backboardd* lockscreen
  dirty/CA animation source and implement safe upstream frame-on-change;
  no unsafe global frame suppression was added.
- Full deep sleep with C2/CPD/SICD and DECON/DSIM physical clock gating
  remain **unimplemented**, because neither has a proven resume path.
  This is a functional first fix, not a full system-suspend claim.
