# The Exynos 9810's cameras: two drivers for now

The Galaxy S9 (`starlte`) and the Galaxy S9+ (`star2lte`) have the same camera hardware family, and this tree has two drivers
for it, written at the same time by different people on different phones. Both are built by the one configuration symbol
`CONFIG_HAKIOS_EXYNOS_CAMERA` and both offer the kernel the same three entry points (`hakios_exynos_camera_init`,
`_quiesce`, `_shot`) and `/dev/hakios-camera`. **A kernel has exactly one of them**: `tools/hakios-config.py` writes
`HAKIOS_DEVICE_IS_<DEVICE>` into the generated `dev/hakios.drivers.h`, and each driver's files compile to nothing in the other
phone's kernel. They cannot both claim the hardware.

| | the S9's (every configuration but `star2lte`; the ios-27 tree) | the S9+'s (`star2lte` only) |
|---|---|---|
| files | `hakios.exynos_camera.c`, `hakios.exynos_camera_rear.c`, `hakios.ak737x.c` | `hakios.exynos_camera_star2.c`, and two headers made at build time (below) |
| run on | a Galaxy S9, 12 November 2026 (front, then rear) | a Galaxy S9+, by Anan Jaser (front, rear, telephoto) |
| structure | one camera at a time chosen at boot: the front, or with `cam_rear=1` the rear | a table of three paths (front, rear, telephoto); the reader of `/dev/hakios-camera` picks one (`HAKIOS_CAMERA_SENSOR`) and the stream changes over |
| front, S5K3H1 on CSIS1 | `cam_s9=N` | `cam_s9=N`, the same steps (this is the file both grew from) |
| rear, S5K2L3 on CSIS0 | `cam_rear=1`: its own register sequence; 2016 x 1512 RAW10 at 30, four lanes at 1352 Mb/s, settle 66 | `cam_s9_rear=1` (and `cam_s9_rear_probe=N`): Samsung's setfile tables (`hakios.s5k2l3_setfiles.h`); the same mode, lanes, rate and settle |
| telephoto, S5K3M3 on CSIS2 | none (the S9 has no such camera) | `cam_s9_tele=1` (and `cam_s9_tele_probe=N`), settle 59 |
| the lens, AK737x at 0x0C | `cam_af=N` in `hakios.ak737x.c`: the module's EEPROM, a held position, autofocus by contrast | written by user space as a position; the telephoto's own AK737x at 0x0F |
| aperture, stabiliser | `cam_rear_ap=N`, `cam_rear_ois=N` | not driven |

## Where the two agree (checked when they were put side by side)

- The lens: address 0x0C, mode register 0x02 (0 active), first position 120, held 10 ms before active mode.
- The rear camera's link: CSIS0, four lanes, 1352 Mb/s, settle 66, a line of 815 ns, and its WDMA's input mux, `SYSREG_ISPPRE`
  `SC_CON2` (0x40C), set to 1.
- The dedicated camera HSI2C blocks have no USI selector: both made the same change to `hakios.exynos_hsi2c.c`, and the tree has
  it once.

## Where they differ

- The front camera's WDMA input mux (`SC_CON4`, 0x414): the S9's driver does not write it (it works with what the boot leaves);
  the S9+'s writes 1 when it is not already 1. `SC_CON6` (0x41C) is written only by the S9+'s, for the telephoto.
- The rear sensor's start-up: a hand-written sequence on one side, Samsung's setfile tables on the other. **These have not been
  compared register by register.**
- The S2MPB02's bus: the S9+'s driver takes a lock that the flashlight driver now offers (`hakios_s2mpb02_bus_lock`); the S9's
  rear driver does not take it.
- Boot-arguments have different names for the same things (`cam_rear` and `cam_s9_rear`).
- The sensors' RAM patches: the S9's rear driver brings the S5K2L3 up without the sensor's patch ("TnP", written through the
  indirect data port 0x6F12); the S9+'s driver writes it, and the S5K3M3's, as Samsung's driver does. Which the picture needs
  is open, and is to be settled when the two are made one.

## The S9+ driver's two made headers

`hakios.s5k2l3_setfiles.h` and `hakios.s5k3m3_setfiles.h` hold Samsung's register tables for the two rear sensors, the RAM
patches among them. A sensor's patch is firmware, and firmware is not committed to this tree, so the headers are not in it:
`tools/mk-star2-setfiles.sh` fetches three headers of Samsung's GPL-2.0 kernel from the Apollo kernel's public repository at a
pinned commit, checks their SHA-256 and runs `tools/extract-2l3-setfile.py` and `tools/extract-3m3-setfile.py` on them;
`xnu-build.sh` does that for a `star2lte` kernel and lays the result beside the driver in the kernel tree. Without a network
the script says which three files to put where.

## Why not one driver yet

Making one driver of the two means choosing one structure and carrying the other's features into it, across some five
thousand lines, and then running it on both phones. On the day the two met, neither phone could be run. Until both can, each
phone keeps the code that was seen working on it. The unification is a job of its own; this page is where it starts from.
