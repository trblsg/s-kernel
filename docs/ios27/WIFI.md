# Wi-Fi on iOS 27 (xnu-12377, STARLTE): Scion's S9 radio, stage 2

Written 9 November 2026. Everything here is **off by default** (boot-args `wifi_pcie=` and `brcmfmac=`). It is built and
linked, and the boot ladder below is how it is brought up on a phone, one step per boot. How far it has run is in the
table at the end.

The radio is the S9's Broadcom BCM4361 on PCIe channel 0. On iOS 16.5 Scion brings it up in two stages: the link, then
Linux's brcmfmac on Scion's Linux layer. With an Apple80211 surface on top, that build joined WPA2 networks, got DHCP
and answered pings (scion-starlte `WIFI.md`, stages a-e). The iOS 27 kernel uses the same drivers:

| What | Where |
|---|---|
| the drivers | `drivers/wifi/brcmfmac/hakios.exynos_pcie.c`, `hakios.brcmfmac.c`, `drivers/wifi/lx/hakios.lx.c`, `drivers/wifi/hakios.sta.c`, `hakios.apple80211.c`, `hakios.wlan.cpp`, `drivers/net/hakios.net.c`: copied into the fork's tree at build time (`profiles/ios-27.copy`) |
| brcmfmac itself | `tools/mk-brcmfmac.sh` (Linux 7.0.14's brcmfmac, ISC, on `wifi/lx`) makes `xnu-deps/build/brcmfmac/out/brcmfmac.o`; `tools/ios27/build-xnu-ios.sh` links it in (`HAKIOS_OBJECTS`) when it is there, its debug map stripped |
| what this tree lacks | the fork's `bsd/dev/starlte_wifi.c`: the radio's files from the root filesystem, polling instead of INTx, the late start |
| the files | `tools/ios27/wifi/60-wifi-firmware.sh ROOT` (a step of the root build): the phone's own `bcmdhd_sta.bin_b2`, `bcmdhd_clm.blob` and `nvram.txt_*` (from the phone's VENDOR, or fetched by scion-starlte `tools/fetch-firmware.sh`), checked against scion-starlte `firmware/wifi/SHA256SUMS`, into `/private/var/hakios/wifi/`. None of them is in any repository |

## How it differs from 16.5

- **Files.** iOS 27 has no small image and no VENDOR reader. The radio's files are read with `vnode_open()` from
  `/private/var/hakios/wifi/` (`wifi_dir=` for another directory) once the root filesystem is mounted. A request waits
  at most `wifi_root_wait=` seconds (120 by default) for it. `mac.info` (the phone's `/efs/wifi/.mac.info`) and `network`
  (two lines: name, passphrase) are read there too, if present. The root build copies them only on request
  (`WIFI_EXTRAS=1`), so a release root never carries a passphrase.
- **Interrupt.** This tree has no GIC SPI attach yet. `brcmfmac_poll` defaults to 1, so a thread runs brcmfmac's handler
  every `brcmfmac_poll_ms` (2 ms by default) instead of taking INTx (16.5's `brcmfmac=5`).
- **Start.** `starlte_wifi_init()` (from IOKitBSDInit) starts a thread. The thread waits for the root filesystem, then
  `wifi_delay=` seconds (60 by default), then brings the link up. That way the USB console is up, and every line of
  the PCIe bring-up reaches it as it is printed. `wifi_delay=0` starts at once.
- **Apple80211.** iOS 27.0.1's IO80211.framework still falls back to ioctls when the interface has no user client:
  `SIOCGA80211`/`SIOCSA80211`, the same request numbers and events, the same registry matching
  (`IO80211InterfaceRole` "Infrastructure", `IOInterfaceName`), and the same bind probe (CARD_CAPABILITIES). Three
  layouts changed:
  - SCAN_REQ is 5528 bytes; its first 5456 are the old request.
  - SCAN_RESULT and CURRENT_NETWORK are 2280 bytes, with 2048 bytes of elements.
  - ASSOCIATE is 656 bytes. Its authentication fields are 32 bits wide, which moves everything after them by 4: the SSID
    length is at 0x10, the key length at 0x40, the cipher at 0x44, the key at 0x4c.

  `hakios.apple80211.c` has both layouts under `HAKIOS_APPLE80211_27`, which the fork's ios-27 tree defines. The change
  is line-neutral, so the 16.5 object does not change.

## The ladder: one step per boot

Lines start with `wifi:`, `exynos_pcie:`, `brcmfmac:` or `lx:`. Watch the console from the moment the phone boots:
phone 1's console first appears as `/dev/cu.usbmodem4` and moves to `/dev/cu.usbmodem01` about 60 s later. The
built-in boot-args (fork commit c1cfa83) keep `sweetreboot`, so a panic resets into TWRP with the console's tail in
`/sys/fs/pstore/pmsg-ramoops-0`.

| Line (on top of the built-in args) | What | Expected |
|---|---|---|
| `wifi_pcie=1` | reads only: PMU, CMU_FSYS1 gates and Q-channels, the pins, as S-Boot left them | `exynos_pcie: 1: ...`; no gate `CLOSED` |
| `wifi_pcie=5` | WLAN_REG_ON, the PHY, the link, the endpoint's ID and BARs | `LINK UP ... Gen2 x1`, `1:00.0 is 14e4:441f` |
| `wifi_pcie=6 brcmfmac=1` | brcmfmac's probe: the chip and its cores through BAR0 | `BCM4361/5`, `the probe returns 0` |
| `wifi_pcie=6 brcmfmac=2` | the files found (needs the root's `/private/var/hakios/wifi/`) | `wifi: /private/var/hakios/wifi/bcmdhd_sta.bin_b2: N bytes` |
| `wifi_pcie=6 brcmfmac=4` | the firmware up, the first command over the rings (DMA) | `Firmware: BCM4361/5 wl0: ... 13.50.37` |
| `wifi_pcie=6 brcmfmac=7` | the station: en0, the registry objects, scans for Settings | `lx: a scan for the kernel: ... N access points`; console `wifi scan` |
| `wifi_pcie=6 brcmfmac=8` | joining from Settings (or `network` in the directory) | `lx: THE NETWORK IS JOINED`; console `wifi ip`, `wifi ping` |

## Where it is

| Step | State |
|---|---|
| kernel built (iOS target, STARLTE) with brcmfmac's object | done, 9 Nov |
| iOS 27 userland checked for the ioctl path and the layouts | done (disassembly, 9 Nov); layouts fixed |
| `wifi_pcie=1` on a phone (phone 1, bp root) | 9 Nov 22:02Z: clean. S-Boot leaves every PCIe gate open, the PHY isolated, WLAN_REG_ON and PERST# low |
| stage 1 to the chip's ID (phone 2, RC1 line) | 10 Nov 00:39Z: PLL locked, `LINK UP after 10 us: Gen1 x1`, `1:00.0 is 14e4:441f ... rev 05`, BARs placed; then `lx: no 46137344 bytes of memory in one piece` (the pool asked a minute after boot) |
| firmware, scan, join, DHCP, ping, HTTP (phone 2, one boot, `wifi_pcie=6 brcmfmac=8`, the pool taken at boot) | 10 Nov 00:46Z: NVRAM from the OTP (SEMCO 91 33: `nvram.txt_r02a_b2`), `Firmware: BCM4361/5 wl0: ... 13.50.37`, CLM; scan; WPA2 on 5 GHz, the handshake by `lx_wpa.c`, joined 225 ms after connect; configd's DHCP on en0; console `wifi ping` gateway 4/4 (about 5 ms), 1.1.1.1 4/4 (about 15 ms); `wifi tcp example.com`: `HTTP/1.1 200 OK` |
| Settings | wifid is not started: its launchd job has `_LimitLoadToDeviceTree` = `wlan` and the S9's ADT has no such node, so Settings shows Wi-Fi off. Two ways, both opt-in, for the next run: an image whose ADT has `/arm-io/wlan` (`tools/ios27/wifi/adt-wlan-node.py`), or the root step `tools/ios27/wifi/62-wifid-loadable.sh` (`WIFI_WIFID=1`), which drops the key from the job as Scion's 16.5 `userland/mkjobs.py` does. With wifid absent, opening Settings > Wi-Fi made Preferences and launchd spin (95% and 58% CPU) until the app was left |

Test images that join a network take it from the ADT (`tools/ios27/wifi/adt-wifi-network.py`; the kernel removes it from the
registry and never logs it); such images are private.
