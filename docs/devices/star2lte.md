<!-- Made by tools/docs-gen.py from devices/star2lte/device.conf. Do not edit: change that, then run tools/docs-gen.py. -->


# Galaxy S9+ (`star2lte`)

| | |
|---|---|
| Vendor | Samsung |
| SoC | `exynos9810` |
| Configuration | `make star2lte_defconfig` |
| Maintainer | Scion (README.md) |
| Repository | [scion-star2lte](https://github.com/Scion-Project/scion-star2lte) |

## What works

| Part | State | |
|---|---|---|
| display | works | the S6E3HA8 (star2 A3 S0 panel, ID 911443) with its own gamma tables from Apollo (scion-kernel 4c4bf73), brightness from iOS (brightness_bridge=1), 1440 x 2960; SpringBoard drawn by the GPU (gpu below) |
| cpu | works | all eight cores (cpus=8 bootcpu=2); the M3s at Apollo's limits with the S2MPS18 OCP (cpufreq_ocp=1): 2314 MHz for 1-2 online, 2106/1924 MHz for more (cpufreq_gov_big_*), A55s to 1950 MHz; MIF at Samsung's starting rates (bus_max=1), never above Apollo's |
| touch | works | S6SY761 (sec_ts, y761_star2) at 0x48 on USI03's HSI2C (s6sy761=1): the phone is used by touch |
| buttons | partly | power gpa2-4, volume gpa0-3/gpa0-4, Bixby gpa0-6 (exynos_keys=1): read at every boot (released); each button's action in iOS not checked one by one |
| storage | works | UFS through the loader (ufs=2), written (ufs_write=1): the system image on SYSTEM (system_apfs=ufs_SYSTEM), /private/var an HFS Plus volume in USERDATA kept between boots (var_hfs=ufs_USERDATA_data), flushed every 5 s (var_sync) |
| gpu | partly | Mali-G72 MP18 up to step 9 (HakiosMali); the window server draws with it through Mesa's panfrost (userland GL=1, lib/libhakios.gl.dylib, Mesa in the system image); occasional drawing artefacts seen; no Metal |
| audio | partly | ABOX and the two MAX98512s (0x39/0x3A, revision 0x41): the speakers play, without ABOX's CPU (calliope) and without DSM, so little bass; the S9+'s own DSM tuning is in firmware/ for when it is; microphone, earpiece and headset not checked |
| battery | works | the MAX77705's gauge and charger, read only (max77705=1): this phone's 3303 mAh battery, level, current and temperature in iOS, charging seen |
| wifi | works | BCM4361 on PCIe (wifi=1, firmware in fw/): scans, joins, WPA2, and joins known networks by itself after a boot (scion-kernel 06d88e6: keybagd's first-unlock notice; 1bd1bcd: IO80211's RSSI bounds); SSH over it |
| bluetooth | partly | BCM4361's Bluetooth on UART_BT as on the S9 (bt=7, CONFIG_BT=7): Broadcom's patch, 4 Mbaud, the EFS address, seen on an S9+ (SM-G965N), 12 November; iOS (userland BT=1, the default since scion-kernel 6effc65): BlueTool's D22AP boot and init scripts through com.apple.uart.bluetooth (result 0), bluetoothd's stack STACK_READY and powered on, an active LE scan hearing advertisers, seen on that phone with the jobs loaded by hand, 12 November; Settings, pairing and audio not yet seen on an S9+ (../starlte/BLUETOOTH.md); since build 28 in the image itself (BT=1 with the chip's patch in fw/), discoverable as the device's name ("iPhone", src/devname.c); pairing and audio not yet seen |
| cellular | no |  |
| haptic | partly | the MAX77705's motor for iOS (/dev/hakios-haptic, max77705_haptic=1); backboardd's button haptics come up; strength not tuned for this motor |
| thermal | works | the TMU publishing kern.hakios.temp every second (tmu=3), the run ending at 110 C or 60 C on the battery |
| camera | partly | rear S5K2L3 (0x10, HSI2C0), front S5K3H1 and telephoto S5K3M3 (0x2D, I2C_CAM2) stream (cam=5); S9+ colour and focus (AK737x); the telephoto at 2x; OIS not driven; no ISP-quality processing |
| torch | partly | the S2MPB02's flash block (torch=3), shared with the camera's rails: driven from iOS, brightness steps not checked |
| sensors | partly | the sensor hub in the BCM4775 (shub=6): accelerometer at position 7 and gyro with this phone's EFS calibration (shub_gyro_cal), the hub set up at every boot; light, magnetometer, barometer and steps not finished; GNSS in the same chip not running yet (docs/gnss.md) |
| usb | works | USB serial log and console (usb_gadget=5 max77705_usbsw=1): every boot's log is read this way |

The state is what has been seen on this device, written down in `devices/star2lte/device.conf`; a part the table does not name has not been looked at.
