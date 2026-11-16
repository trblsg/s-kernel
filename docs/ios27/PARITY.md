# iOS 27 (xnu-12377, STARLTE) against Scion's iOS 16.5 (xnu-8796) on the Galaxy S9: parity

Written 8 November 2026. Everything this work added is behind a boot-argument and **off by default**; none of patches
`0420`-`0424` was booted on a phone when this was written (they were built).

| Feature | Scion 16.5 (S9) | iOS 27 here | State |
|---|---|---|---|
| RAM | 3218 MB: a 2 GB window at `0x880000000` plus 1170 MB of the low bank (`xnuloader` `extra_ram`, patch 1512) | 1536 MB: the window only | `0420` ports patch 1512 to 12377 (the VM radix's `pmap_first_pnum` lowered), ADT `/chosen` `hakios-extra-ram` with seven low-bank ranges (1532 MB), `extra_ram=1` (or `=N` MB, `extra_ram_mask=`) for about 3.0 GB; `tools/ios27/adt-extra-ram.py` writes the property; an ADT with a 1792 MB window gives about 3.3 GB |
| CPU clocks | governor `cpufreq_gov=1`: load, touch and frame boost, TMU ceilings | fixed A55 1690 MHz, M3 1794 MHz, set once | `0421`: Scion's governor on Starbite's single ACPM client |
| Touch | polled 4 ms at priority 92 (`BASEPRI_PREEMPT`), boosts the governor | polled 5 ms at the default priority | `0420`: `startouch=92`, `startouch_ms=4`, and the boost hook |
| Wi-Fi | BCM4361 on PCIe: link, then brcmfmac on Scion's Linux layer, Apple80211 | none | `0422`: stage 1, `wifi_pcie=1..5` (link and the 14e4:441f identity); stage 2 (brcmfmac on the Linux layer, en0, the station, Apple80211 with iOS 27's layouts) built, `wifi_pcie=6 brcmfmac=N`, not yet run: [WIFI.md](WIFI.md) |
| Temperatures | `tmu=3`: read, publish, stop at 110 C | none | `0423`: steps 1 and 2, reads only, wired to the governor |
| Display | DPU scan-out service (`fb_flip`, `fb_dpp` scaler, `fb_zc`, `fb_psr`) | copied by the CPU, about 48 ms a frame | `0424`: `hakios_scanout_*`, `dpu_probe=1`; the display carve-out `0xD8000000`-`0xDFFFFFFF` |
| GPU clock | `mali_gov=1` | Mali-G72 fixed at 572 MHz, 950 mV set by hand | not ported on purpose: an ACPM rate change would let the APM lower the voltage under the hand-set 950 mV |
| GPU compositing | Mesa Panfrost behind CoreAnimation | `tools/ios27/ca-gpu` (sbca) | the GPU track's |
| Not started | Bluetooth (kernel half under way elsewhere), audio, camera, sensors, haptics, RTC, MFC video | | |

Cores: 16.5 runs 8 (`cpus=8`); iOS 27 runs the four A55s. The M3s need LSE and RCpc emulation (patches `0342`-`0344`) and iOS 27's
arm64e user space needs pointer-authentication emulation; whether `cpus=8` pays is decided by the trap cost once RAM and the governor are in.
