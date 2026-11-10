# iOS 27 on the Galaxy S9: audio and Bluetooth, the iOS side

The kernel side is in the fork (Scion-Project/xnu, branch `wip/ios27-s9-audio-bt`): ABOX + MAX98512 (`abox=N`,
`max98512=N`, `abox_rmem=1`) and the BCM4361's Bluetooth (`bt=N`). This directory is what iOS 27's root needs on top,
as one idempotent root step for ios27-fullroot's build (`tools/steps.d/`), OFF unless `AUDIO_BT=1`.

    tools/ios27/audio-bt/build.sh                       # -> build/ios27-audio-bt/files (needs Xcode + iPhoneOS SDK)
    AUDIO_BT=1 OS_MNT=<IPSW OS image mounted> HCD=<the phone's own bcm4361B2_semco.hcd> \
        tools/ios27/audio-bt/60-audio-bt.sh <mounted iOS 27 root>

What the step does:
- `/System/Library/Audio` from the user's own IPSW if the root lacks it (stage-ios-root.sh leaves it out; it has the
  HAL plug-ins, among them Apple's `BTAudioHALPlugin.driver` for A2DP/HFP, the tunings and the UI sounds).
- `HakiosAudio.driver` (Scion's AudioServerPlugIn, `userland/src/audio_plugin.c`, unchanged source, built for arm64e) in
  `/System/Library/Audio/Plug-Ins/HAL/`: the "Speaker" device writing to `/dev/hakios-audio` (kernel `abox=8`).
- The jobs back into `launchd.plist`: `com.apple.audiomxd` (iOS 27's audio server, which hosts the HAL),
  `com.apple.bluetoothd` with `BT_HCI_TRANSPORT=H4:POSIX:com.apple.uart.bluetooth,4000000` (the kernel's UART control at
  `bt=7`), and `com.apple.BlueTool`; bluetoothd and BlueTool start through `/usr/libexec/hakios-inject` with their
  library (launchd drops DYLD_ variables and the 608-byte command line has no room for more `sweetinject`).
- BlueTool's scripts for the S9 in `/etc/bluetool/` (`N104AP.*.script`: the iOS 27 root's HW model), the phone's own
  Broadcom patch in `/private/var/hakios/bt/` (read by the kernel at `bt=6`; never in git: it is the phone's vendor file).
- `AUDIO_BT_LOG=1` (tests): each of the three jobs' stderr into `/private/var/local/oslog/<name>.err`, with
  `OS_ACTIVITY_DT_MODE=YES` so their os_log lines go there too (logd is off on this root).

Status (9 November 2026, phone 1, silent runs at night):
- Kernel: `abox=7 max98512=3 abox_rmem=1` + console `abox tone 3` played through both speakers (heard by the owner).
- With this step and `abox=8 bt=7 cs47l93=6 abox_mic=1`: iOS 27's `audiomxd` runs (no crash loop on this kernel) and
  loads `HakiosAudio.driver`; VirtualAudio makes the Speaker (`pspk`), Hawking, Actuator and Codec ports from it and
  activates routes on them. `hal_probe` (as the root's `/private/var/herolte/autorun`) loads the plug-in, sees its four
  devices and runs 3 s of output and 3 s of microphone input through it. That run had no `/dev/hakios-audio` (a kernel
  bug, fixed in scion-kernel `fork/ios27-audio-bt`) and no codec power (PMIC calls stubbed; fixed in the fork).
- Bluetooth: kernel `bt=1..7` all pass on iOS 27 (the phone's own SEMCO patch from `/private/var/hakios/bt/`, 4 Mbaud);
  BlueTool runs `N104AP.boot/init.script` (result 0); bluetoothd reaches STACK_READY, and Apple's BTAudioHALPlugin in
  audiomxd connects to it; bluetoothd then restarts (reason 701) when the chip rejects one of Apple's LE-scan vendor
  commands: kernel boot-arg `bt_vsc_ok=` (scion-kernel `fork/ios27-audio-bt`) answers those.
