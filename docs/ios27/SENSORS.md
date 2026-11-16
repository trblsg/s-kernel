# The Galaxy S9's motion sensors on iOS 27

The S9's accelerometer and gyroscope (and its magnetometer, barometer, light and proximity sensors) sit behind the
sensor hub inside its Broadcom BCM4775 GNSS chip. `SENSORS.md` of the device's repository (`devices/starlte/`)
has the hardware and the protocol, and the iOS 16.5 line's run. This page is what the ios-27 tree adds, what was seen on
a phone on 12 November 2026, and what is still open.

## The pieces

| Piece | Where | Switched on by |
|---|---|---|
| the hub's driver (the 16.5 line's, shared) | `drivers/input/ssp/hakios.ssp.c`, `hakios.bcm_shub.h`, the SPI master `drivers/soc/exynos/hakios.exynos_spi.c`: copied into the tree by `profiles/ios-27.copy` | `shub=6` |
| its place in the tree | the fork (`Scion-Project/xnu`): `bsd/conf/files`, `bsd/sys/hakios.sensors.h`, `bsd/sys/hakios.exynos_spi.h`, the call in `IOKitBSDInit.cpp`, the PMIC's registers through `starlte_pmic_read/write` | |
| the hub's firmware | not in any repository (below); the fork's `bsd/dev/starlte_fw.c` hands it to the driver from the device tree | `tools/ios27/adt-firmware.py` when the image is packed |
| the accelerometer and the gyroscope for iOS | the fork's `iokit/Kernel/arm/hid/SweetHIDSensors.cpp`: two HID event services of Apple's vendor page (0xFF00, usages 3 and 9), fed by the driver's `hakios_ssp_tap()` | `shub_hid=3` |
| the device's orientation | `tools/ios27/sensors/orientation.c`, a library in backboardd (`/usr/lib/o.dylib`) | `;/usr/lib/o.dylib,backboardd` added to `sweetinject=` |

Nothing of this runs without its argument: no `shub=`, nothing mapped or powered; no `shub_hid=`, no HID services; the
library does nothing when `/dev/hakios-sensors` is not there, and is not loaded at all unless `sweetinject=` names it.

## The firmware

The hub runs a RAM image that the driver loads through the chip's ROM at every boot (`bcm_sensorhub_a1.bin`, 413864
bytes). It is Broadcom's and Samsung's and is in no repository of ours: `devices/starlte/tools/fetch-firmware.sh` makes
it from your own copy or from the pinned public tree and checks it against `SHA256SUMS`
(`devices/starlte/firmware/sensorhub/README.md`).

On the 16.5 line it travels in the small image's `fw/` directory. An ios-27 boot image has no RAM disk, so it travels in
the device tree instead:

    tools/ios27/adt-firmware.py PLAIN.adt WITH-FW.adt devices/starlte/firmware/sensorhub/bcm_sensorhub_a1.bin

adds a node `/scion-firmware` with one property for each file (the property's name is the file's name), and the image
is packed with `WITH-FW.adt`. The kernel's `hakios_image_file("fw", NAME)` (the fork's `starlte_fw.c`) answers from the
flat tree, which stays in memory. Keep such an image private: the firmware is not ours to pass on.

## Boot arguments

`shub=`, `shub_sensors=`, `shub_ms=` and the others are the 16.5 line's ([boot-args.md](../boot-args.md)). New with this work:

| Argument | Does |
|---|---|
| `shub_say=N` | every N seconds two lines: the accelerometer's mean (counts and mm/s2) and the gyroscope's (counts and thousandths of a degree a second), the samples, the longest step between two, and how far the gyroscope says the phone has turned |
| `shub_selfread=N` | 30 s after the boot, `/dev/hakios-sensors` read for N seconds from inside the kernel, as a program would: each sensor's rate, the longest step, what the queue lost. `sysctl kern.hakios.sensors_selfread=N` again |
| `shub_hid=N` (the fork) | 1 the accelerometer's HID service, 2 the gyroscope's, 4 send from the start instead of waiting for a client's `ReportInterval`, 8 log what the event system sets |

A line of text written to `/dev/hakios-sensors` is put in the kernel's log with the writer's name; the orientation
library says what it does that way, so it shows on the USB console.

## Units

The hub gives Android's frame: x to the right of the screen, y to its top, z out of it. iOS's frame is the same with
the accelerometer's sign turned (lying face up: Android +1 g on z, iOS -1 g).

- Accelerometer: `range / 32768` g a count; the hub's range is 8 g, so 4096 counts are 1 g.
- Gyroscope: a count is a thousandth of the chip's 35 mdps step at its 1000 dps range: 28571 counts are one degree a
  second. The hub says so itself in its log (`Sensor_Gyro-5,-29,14,-0.003,-0.018,-0.009`: steps and rad/s, beside
  `SSP Gyro Send 1 1 4465 -29150 14825`: the same sample in counts). `userland/src/inject.c` of the 16.5 line assumed
  16400 counts a degree a second, which is 1.74 times too fast; it is left as it was there (not run again on 16.5).
  The hub subtracts its own gyroscope calibration a few seconds after the start.

## Seen on the phone (12 November 2026, one Galaxy S9, iOS 27.0.1)

1. **The hub**: CHUB's domain on, LDO37 on (CTRL 0xEC), ASIC 004775b0, ROM 0004527c, the 452 writes in 3.2 s, the
   application's ROM 0006431f, WHOAMI 0x55, the sensors found 0x1fff07f6bf, firmware revision 21031900, range 8 g.
   The accelerometer with the phone standing on a long edge: -9675 -19 258 mm/s2 (gravity on -x), and after the phone
   was turned over, +x; in a hand, screen up: -775 3466 8997 (most on +z). The gyroscope at rest: 0, -5, -1 mdps.
2. **To user space**: 3114 samples read through `/dev/hakios-sensors` in 20.0 s; the accelerometer, the gyroscope and
   the magnetometer 50.0 a second each, no step longer than 20 ms, none lost, none out of order.
3. **iOS**: backboardd's event system opens both HID services. With the library, backboardd logs "Updating device
   orientation from CoreMotion to: landscapeLeft (3)" and "Sending orientation: landscapeLeft (3) event to
   SpringBoard"; SpringBoard: "[EmbeddedPipeline][Accelerometer] did change device orientation to: Ll". With the phone
   in a hand the orientation went landscape left, face up, landscape left, face up as it was turned.
4. **The screen**: with Notes in front and the phone on its side SpringBoard turns the interface ("Active
   interface orientation changing from portrait (1) to landscapeRight (3)") and turns it back when the phone is
   upright again.
   - With the OpenGL ES compositor (no `sbgl=0`, backboardd not among the root's Metal clients) Notes is drawn in
     landscape on the panel, goes back to portrait and to landscape again. The way back was driven by the console's
     `sensors fake 0 4096 0 14` (nobody was at the phone to turn it); the landscape was the phone's real position.
   - With the Metal compositor the turned interface is not drawn: the Metal plug-in refuses the texture CoreAnimation
     asks for over the panel's surface (`BufferTexture(RowPitch { bytes_per_row: 3328, row_bytes: 7168 })`: a
     1792-pixel-wide target over the 832-pixel-wide portrait surface), sixty times a turn, and the panel keeps its last
     portrait picture until the interface is portrait again. That is the compositor's work, not the sensors'.

## Why the library is needed

The iOS 27 root runs as an iPhone 11. For that model CoreMotion's `CLPlatformInfo::queryMotionCaps()` says the
orientation comes from the motion coprocessor, so `CLOrientationNotifier` opens the coprocessor's HID device and never
starts its own detector that works from the accelerometer (`CLOrientationDetector`). The S9 has no such coprocessor.
The library replaces `CMDeviceOrientationManager`'s `startDeviceOrientationUpdatesToQueue:withHandler:`,
`stopDeviceOrientationUpdates`, `deviceOrientationBlocking` and `isDeviceOrientationAvailable` in backboardd, and works
the orientation out from the accelerometer (the axis with more than 0.75 g, held for 300 ms), as `inject.c` does on 16.5.

Build and install:

    tools/ios27/sensors/build.sh o.dylib        # arm64, signed ad hoc
    # copy it to /usr/lib/o.dylib of the root (mode 755); on the kernel's line:
    sweetinject=<the line's other libraries>;/usr/lib/o.dylib,backboardd

To remove it, take `;/usr/lib/o.dylib,backboardd` off the line (the file is then never loaded) and delete the file.

## Open

- A turned interface with the Metal compositor (above); a turn by hand with an application in front (the turns by hand
  were on the home screen and in Settings, which do not turn on a phone of this size).
- An application reading raw values through CoreMotion: nothing on the test root asked for them (`ReportInterval` was
  never set on the services), so the events' units are checked only against the hub's own figures.
- The magnetometer, barometer, light and proximity sensors reach `/dev/hakios-sensors` but have no HID service here.
- The hub's record 0x0B (its gyroscope calibration, once after the start) is not parsed: one frame a boot is counted bad.
- The hub thread polls the hub's request pin every 4 ms (Samsung uses an interrupt).
