# iOS 27 on a Galaxy S9: build it and install it yourself

This page is for someone outside the project who has a Galaxy S9, a Mac and an iPhone restore image downloaded from Apple, and wants
to see iOS 27 start on the phone. It is experimental. Read "What works" and "Known problems" before you spend an evening on it.

**It erases the phone.** The iOS system is written over Android's data partition (`USERDATA`). Everything Android stored there is
gone. Android's own system partition and the recovery (TWRP) are not touched, and the boot loader is never written.

Nothing of Apple's and nothing of Samsung's is in this repository, and nothing may be added to it. You make the kernel from source
and everything else from **your own** restore image on **your own** Mac. Do not upload or share what you build: the boot image, the
root image and the device tree carry your restore image's identity and Apple's files.

Written 12 November 2026. Each step below says whether it was run from a fresh clone of the public repositories ("clean run") or
is written down from how the project's own test phones were made.

## What you need

| | |
|---|---|
| Phone | Galaxy S9 **SM-G960F** (Exynos 9810, "starlte"). Not the Snapdragon models (G960U/W/0), not the S9+ (untested on this line). OEM unlock done and **TWRP installed in recovery** the way Samsung and TWRP document it; that step is yours and is not part of this page. Charge it. |
| Mac | Apple silicon. The build machine ran macOS 27.0.1 with 8 cores and 16 GB. About **110 GB free**: the restore image (9 GB), its three unpacked images (20 GB), the kernel build (4 GB), the root build (wants 62 GB free when it starts). |
| Xcode | **Xcode 16.4**, from Apple's developer downloads (`Xcode_16.4.xip`). It need not be the selected one: the commands take `DEVELOPER_DIR`. |
| Kernel Debug Kit | **Kernel Debug Kit 26.5, build 25F71** (`Kernel_Debug_Kit_26.5_build_25F71.dmg`), from Apple's developer downloads (a free account is enough). Unpack it with `pkgutil --expand-full` and point `KDKROOT` at the `.kdk` directory. |
| Restore image | The **iPhone 11** image **iPhone12,1, iOS 27.0.1, build 24A446**, from Apple's own servers (Finder saves it, or `ipsw download ipsw --device iPhone12,1 --build 24A446`, which reads Apple's catalogue and downloads from Apple). No other build works: the tools check addresses inside this one. Do not use an image from anywhere else. |
| Tools | `jq`, `gum`, `xcodes`, `cmake`, `ninja` (Homebrew has all five), `git`, Android's `adb` (platform-tools), blacktop's [`ipsw`](https://github.com/blacktop/ipsw) (3.1.743 was used), and a Python 3 environment with `numpy` and `ziglang` (`python3 -m venv venv && venv/bin/pip install numpy ziglang`). |
| A USB cable | The phone's console is a serial port over USB-C. It is how you see the log and how you ask the phone to go back to TWRP. |

In the commands below `$PY` is the Python of that environment, `$SERIAL` is your phone's serial as `adb devices` prints it.

    export DEVELOPER_DIR=/Applications/Xcode_16.4.app/Contents/Developer
    export KDKROOT=$HOME/kdk/KDK_26.5_25F71.kdk
    export PY=$PWD/venv/bin/python
    git clone https://github.com/Scion-Project/scion-kernel.git && cd scion-kernel

## 1. The kernel (about 5 minutes; clean run)

    OS_PROFILE=ios-27 make kernel JOBS=4          # -> ./hakios-kernel

This fetches the kernel source (the `scion/xnu-12377` branch of Scion-Project/xnu at the commit `profiles/ios-27.conf` names),
blacktop's darwin-xnu-build at a fixed commit, and Apple's open-source pieces it needs, and builds an iOS-target XNU for the S9.
A clean run on the build machine took 258 seconds at `JOBS=3`. The result has the same 93,206 symbols and the same 2,620,499
instruction words as the kernel the test phones run; the only differences are the build's time stamp and the path of the build
directory in 258 assertion strings, and the addresses that shift because of them.

## 2. Your inputs from the restore image (about 15 minutes; all but the decryption was a clean run)

    tools/ios27/s9/fetch-apple-inputs.sh ~/Downloads/iPhone12,1_27.0.1_24A446_Restore.ipsw inputs

It reads the image's manifest, checks the build, and writes `inputs/os.dmg`, `inputs/systemos.dmg`, `inputs/appos.dmg` and
`inputs/n104ap.adt` (the iPhone 11's device tree). Two of the three images are Apple Encrypted Archives; the key for each is the
one Apple's own key service returns for the address in the archive's header, and macOS's `aea` does the decryption. The project's
phones were made with these same commands run by hand. The script was run in the clean room on a stand-in image holding the real
manifest and device tree: it reads the manifest, unpacks, and its `n104ap.adt` is the one the test images were made from. The
decryption of the two archives was not repeated (no room for a second copy of the images on the build machine).

## 3. The loader and the boot image (under a minute; clean run)

    tools/ios27/s9/build-loader.sh                                     # zig as the cross compiler: no Linux VM
    tools/ios27/s9/flash.sh $SERIAL pull-template inputs/recovery.img  # the phone in TWRP: a copy of ITS recovery partition
    tools/ios27/s9/flash.sh $SERIAL backup-boot  inputs/android-boot.img   # keep this file: it is how Android boots
    tools/ios27/s9/make-boot-image.sh --kernel hakios-kernel --donor-adt inputs/n104ap.adt \
        --template inputs/recovery.img                                 # -> out/xnu-boot-ios27.img

The boot image is an Android boot image whose "kernel" is the loader; the loader carries the XNU kernel, the S9's device tree in
Apple's format (made from your iPhone 11 tree and the S9's own memory map) and a two-word command line (`sbgl=0
sweetspeed=140347391`; everything else is built into the kernel). The copy of your recovery partition is only a template: its
header and Samsung's device-tree block are kept so that the phone's boot loader accepts the image.

Clean run: the loader built this way is byte for byte the one in the test phones' boot image, and the script given the test
phones' kernel reproduces their boot image except for the 256 random bytes of the device tree's seed and the header's checksum.
On a phone (12 November 2026, a test phone that already held a project-made iOS 27 root): `pull-template`, `backup-boot`, the
boot image made by these commands from the clean-run kernel and loader (command line `sweetspeed=140347391`, the OpenGL ES
compositor that root was set up for), `flash.sh boot ... reboot`: the USB console came up 14 seconds after the flash, the first
picture after 30 seconds, and SpringBoard's lock screen was on the panel with no panic. `to-recovery.sh` brought it back to TWRP.

## 4. The userland the root needs (hours; NOT a clean run, see below)

iOS draws through Apple's GPU drivers, which the S9 does not have. The root therefore gets a few libraries of ours that are built
from source: `sbca` (CoreAnimation's renderer through OpenGL ES on the Mali-G72), `libsbgl` and Mesa's `libOSMesa` under it, and
for the Metal compositor `StarbiteMetal.bundle` with Mesa's PanVK. Their build scripts are in this repository and each says what
it needs:

| Piece | Script | Needs |
|---|---|---|
| Mesa 25.0.7 (OSMesa + Panfrost) | `tools/ios27/mesa-ios/mk-mesa-ios.sh` | meson, mako, pyyaml (a venv), ninja, Homebrew `bison` and `flex` |
| `libsbgl`, `libOSMesa` staged and signed | `tools/ios27/perf/build-sbgl.sh` | the Mesa above |
| `sbca.dylib` | `tools/ios27/ca-gpu/build-sbca27.sh` | the Mesa above |
| Metal: Rust, the bridge, host tools, PanVK, the bundle | `tools/ios27/metal/rust.sh`, `bridge.sh`, `build-host.sh`, `mk-panvk27.sh`, `mk-bundle27.sh` | Rust 1.98, Homebrew LLVM 23, Mesa 26.2.6 (`docs/ios27/METAL.md`) |

Lay the results out like the root (`usr/lib/sbca.dylib`, `usr/lib/s.dylib` as a second copy of it, `usr/local/starbite/lib/libOSMesa.8.dylib`,
`usr/local/starbite/lib/libsbgl.dylib`, and for Metal `System/Library/Extensions/StarbiteMetal.bundle/`) in a directory, here `overlay/`.

**This is the weakest part of the page.** These scripts were written and run on the project's Mac over several days; nobody has
yet run them in order from a fresh clone, and there is no single command for them. To try, you need Homebrew with `bison`, `flex`,
`meson` and LLVM 23, Rust through `rustup` (the script installs it under the build directory), about 10 GB and some hours. The
Metal bundle on the test phones also has two later changes that are only on a draft branch (scion-kernel pull request 281); the
OpenGL ES compositor needs neither and is the one to start with. Three files the test phones also carry have no
public build recipe yet: `m.dylib` (a static wallpaper stand-in; its source is `tools/ios27/metal/sbwall.m`), `b.dylib` (the
brightness slider) and the 24 stock wallpaper layers converted from HEIC to PNG. Without them the phone still starts; expect a
black wallpaper and a brightness slider that does nothing. Without `overlay/` altogether the root builds and the kernel boots it,
but nothing is drawn after the boot console.

## 5. The root image (about an hour; the cache rewrite is a clean run, the rest is not)

    hdiutil attach -readonly -nobrowse -imagekey diskimage-class=CRawDiskImage -mountpoint /tmp/sysos inputs/systemos.dmg
    tools/ios27/s9/prepare-root-inputs.sh /tmp/sysos/System/Library/Caches/com.apple.dyld overlay
    hdiutil detach /tmp/sysos
    sudo PY=$PY tools/ios27/s9/build-root.sh inputs        # -> tools/ios27/fullroot/out/root-full.img

iPhone software since iOS 17 is built for processors the S9 does not have (pointer authentication, and instructions newer than
its ARMv8.0 big cores). The kernel emulates what traps; to make the phone usable the build rewrites those instructions ahead of
time in a copy of your system's shared library cache and in every program of the root (`docs/ios27/AOT-REWRITE.md`,
`docs/ios27/TEXT-DATA.md`). It also applies five one-word changes to that cache (`tools/ios27/s9/patches-24A446.txt`, each
checked against your cache first), removes the launchd jobs for hardware the port does not drive, marks Setup as done, and
installs `overlay/`. The result is a raw HFS+ volume image, 32 GiB sparse with about 20 GiB used.

`prepare-root-inputs.sh` makes the build's private directory (`tools/ios27/fullroot/private/`, never in git): instruction tables
from your cache, the checked patch list, and one script of wardevsa's fetched from his public repository at a fixed commit.

One library is left exactly as Apple built it: `libcorecrypto` (and its two variants). It checks a hash of its own code when it
starts, so rewriting it makes its self-test fail; the project's own phones run a rewritten copy with a local stand-in for the
self-test's helper, and neither the stand-in nor a way to make one is published (`docs/ios27/SPEED.md`, "Not published"). Your root
does not need it: the kernel emulates the pointer-authentication instructions of that library as they trap, which is how every
root booted before that local step existed (the home screens of 8 November and the morning of the 9th). What you pay is time in cryptographic
calls; how much has not been measured on its own.

Clean run: `prepare-root-inputs.sh` (81 seconds) and the whole cache rewrite (`pipeline/cache-pipeline.sh`, 8 minutes), unprivileged,
with their checks passing (the mapping replay prints `FAILS 0`, no protected data word changed). Given the same inputs as the
project's own build of 10 November, the clean run's 79 cache files are that build's byte for byte, except inside the three
`libcorecrypto` images. With your inputs it differs from that build in three stated ways: the CoreImage word of the patch list;
no private review list (145 places that might be data are left as they are, 105 of them small functions that keep trapping); and
instruction tables made from the untouched cache, which protect 1,613 more words.

**Not run from a clean clone:** the copy into the image, the root steps and the offline checks, that is everything
`build-full-root.sh build` does after the cache. It needs an administrator password (`sudo`: the files must keep their owners) and
62 GB free; the build machine could give this work neither. The project's own roots were made by the same scripts in their
private layout, and several mistakes of the move to the public layout were found and fixed while writing this page. Expect to find
another one here, and please report it.

## 6. Install (about half an hour) and first boot

The phone in TWRP (hold Volume Up + Bixby + Power from off), on USB:

    tools/ios27/s9/flash.sh $SERIAL info
    tools/ios27/s9/flash.sh $SERIAL root tools/ios27/fullroot/out/root-full.img    # asks for the word ERASE; erases Android's data
    tools/ios27/s9/flash.sh $SERIAL boot out/xnu-boot-ios27.img reboot

Every write is read back and compared. Only `BOOT` and `USERDATA` are written. The first boot of a fresh root takes about two
minutes to the first picture (later boots 20 to 40 seconds). The log is on the USB console: `screen /dev/cu.usbmodem*` on the Mac.

Of this sequence, `info` and the `boot` line were run on a phone with an image built by this page's commands (section 3). The
`root` line was not: no root image has been built from a clean clone yet (section 5), and the test phones' roots were written
by the same `fastflash-root.py` from the project's private layout.

## Getting back

- **To TWRP from iOS:** `tools/ios27/s9/to-recovery.sh` (it writes the word `return` to the USB console), or Samsung's forced restart
  (Volume Down + Power for about ten seconds) and at once Volume Up + Bixby + Power, held until TWRP shows. The kernel also returns to TWRP by
  itself 24 hours after every boot; that is deliberate while the port can hang.
- **Another try:** from TWRP, flash a new boot image or root image with `flash.sh`.
- **Back to Android:** `tools/ios27/s9/flash.sh $SERIAL boot inputs/android-boot.img`, then in TWRP Wipe > Format Data (Android's
  data was erased by the install; Android starts as after a factory reset). Written down from how the partitions are used; not
  re-tested for this page.

## What works on iOS 27 today, and what does not

| | |
|---|---|
| Works | Boot to the home screen; the display at 60 Hz; touch; the Mali-G72 GPU (OpenGL ES compositor, or CoreAnimation's Metal renderer with Liquid Glass through PanVK); storage (the system on the phone's UFS, read-write); all eight cores; about 3.6 GB of RAM; the panel's brightness (with the brightness library of section 4); the USB console; Settings and the stock apps start (those that want a network have none). |
| In the kernel only | The front and rear cameras: sensor, lens, focus, stabiliser and iris are driven and frames can be read from `/dev/hakios-camera` (boot-arguments, off by default). No iOS app sees a camera, and **the Camera app does not appear on the home screen yet**. |
| Works with extra steps | **The accelerometer and the gyroscope, and screen rotation** ([SENSORS.md](SENSORS.md)): the sensor hub's driver and two HID services in the kernel (`shub=6 shub_hid=3`, off by default) and a small library in backboardd (`tools/ios27/sensors/`, added to the root and to `sweetinject=` by hand). Seen on a phone on 12 November: the orientation followed the hand, and Notes turned to landscape and back. **Rotation works with the OpenGL ES compositor; with the Metal compositor a turn freezes the screen** until that is fixed, so do not use the two together. The magnetometer, barometer, light and proximity sensors are read by the kernel only (`/dev/hakios-sensors`); iOS does not get them. |
| Built in, off, not part of this page's build | **Wi-Fi** ([WIFI.md](WIFI.md)): the kernel has the S9's Wi-Fi since 12 November behind `wifi_pcie=6 brcmfmac=N`, but the radio driver itself (Linux's brcmfmac, fetched and built by `tools/mk-brcmfmac.sh`) is linked in only if you build it before the kernel, and the phone's own firmware files must be put into the root (`tools/ios27/wifi/60-wifi-firmware.sh`). What a phone has done: in one boot on 10 November, on the work's own branch, the radio loaded its firmware, joined a WPA2 network, got an address by DHCP and answered `ping` and an HTTP request from the kernel's console. iOS itself did not use it: Settings showed Wi-Fi off (wifid is not started). The kernel as merged has been built, with and without the driver, and not booted with Wi-Fi on. |
| Not in this build | **Cellular** (no modem; the phone-call services are removed), **audio and Bluetooth** (for iOS 27 their drivers exist on unmerged branches only; nothing of them is in this kernel), a passcode (there is no Secure Enclave; the key store answers as a phone without a passcode). With no network there is no activation, no iCloud and no App Store; Setup is marked done by the build. Anything not named under "Works" should be taken as not working. |

## Known problems

- **It can hang hard.** On the test phones the kernel has a few times stopped dead some minutes into a session, console included,
  and the cause is not found. Samsung's forced restart (Volume Down + Power, about ten seconds) gets out of it. The 24-hour return
  to TWRP exists because of this.
- **It is slow in places.** Swiping the home screen with the Metal compositor keeps 43 to 67 % of frames on time; scrolling inside
  an app about 95 %. The first frame after boot takes 18 to 40 seconds.
- **Icons and Settings symbols** are right only with the rewrite's data-word guard and the CoreImage word in the patch list.
  That last word was proven on a phone only together with a kernel switch, bit 268435456 of `sweetspeed` (IOSurface tells iOS
  its largest sizes). The kernel this page builds has the switch since 12 November, off: the command line above
  (`sweetspeed=140347391`) does not set it, `sweetspeed=408782847` does. With it set, and the root's icon steps, the Clock,
  Settings and Safari icons were drawn on a phone on 12 November; what the switch alone changes has not been measured.
  Without it some Settings rows may have no symbol.
- **The lock screen's clock is not drawn** (an empty box where the digits should be; the date line in the test boot read
  "Wed Dec 31"), and the **wallpaper picker** does not work.
- `chronod` aborts once about 20 seconds after boot and comes back; four widget extensions (Calculator, World Clock, the code
  scanner, the launcher control) crash three times each at start and stay off.
- The command line and the patches are for build 24A446 only. Another restore image is refused.

## Who made what

The S9 machine layer of this kernel is the work of Project Starbite and the layers under it (ios-sweet, ios-a5xelte, wardevsa's
Galaxy S7 layer); `NOTICE` and `LICENSES.md` have the names and licences. The loader is Starbite's (GPL-2.0-only). The scripts of
`tools/ios27/s9/` are Scion's (MIT-0).
