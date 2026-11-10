#!/bin/bash
# tools/ios27/audio-bt/build.sh: the iOS 27 audio and Bluetooth userland pieces for the Galaxy S9, into OUT (default
# build/ios27-audio-bt/files): arm64e, PAC rewritten for the S9's cores (tools/ios27/a64e/a64e-rewrite.py), ad-hoc signed.
#   HakiosAudio.driver       Scion's CoreAudio HAL plug-in (userland/src/audio_plugin.c): the speaker, through /dev/hakios-audio
#   hakios-inject            runs a launchd job's program with a library inserted (src/hakios_inject.c)
#   hakios.bluetool.dylib    BlueTool runs /etc/bluetool/<HW model>.*.script (src/hakios_bluetool.c)
#   hakios.bluetoothd.dylib  bluetoothd's iAP PTY non-blocking (src/hakios_bluetoothd.c)
#   bluetool/                the S9's BlueTool scripts (N104AP, the iOS 27 root's HW model; D22AP as on 16.5)
#   hal_probe                (tests) loads a HAL plug-in as the HAL does and plays/records through it (src/hal_probe.c)
# Env: DEVELOPER_DIR (an Xcode with the iPhoneOS SDK), PY (a python with numpy; default build/venv/bin/python), OUT.
# Then 60-audio-bt.sh puts them on a root (FILES=OUT).
set -euo pipefail
H=$(cd "$(dirname "$0")" && pwd); SK=$(cd "$H/../../.." && pwd)
PY=${PY:-$SK/build/venv/bin/python}; O=${OUT:-$SK/build/ios27-audio-bt/files}
export DEVELOPER_DIR=${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}
CC="xcrun -sdk iphoneos clang -arch arm64e -mios-version-min=18.0 -O2 -Wall"
DY="-dynamiclib -Wl,-not_for_dyld_shared_cache"
rm -rf "$O"; mkdir -p "$O/HakiosAudio.driver"
$CC -bundle -framework CoreFoundation -I "$SK/userland/src" -o "$O/HakiosAudio.driver/HakiosAudio" "$SK/userland/src/audio_plugin.c"
cp "$SK/userland/src/audio_plugin.plist" "$O/HakiosAudio.driver/Info.plist"
$CC -o "$O/hakios-inject" "$H/src/hakios_inject.c"
$CC $DY -install_name /usr/lib/hakios.bluetool.dylib -framework CoreFoundation -lMobileGestalt -o "$O/hakios.bluetool.dylib" "$H/src/hakios_bluetool.c"
$CC $DY -install_name /usr/lib/hakios.bluetoothd.dylib -o "$O/hakios.bluetoothd.dylib" "$H/src/hakios_bluetoothd.c"
$CC -framework CoreFoundation -framework AudioToolbox -o "$O/hal_probe" "$H/src/hal_probe.c"
for f in "$O/HakiosAudio.driver/HakiosAudio" "$O/hakios-inject" "$O/hakios.bluetool.dylib" "$O/hakios.bluetoothd.dylib" "$O/hal_probe"; do
	"$PY" -I "$SK/tools/ios27/a64e/a64e-rewrite.py" macho "$f" | tail -1
done
codesign -s - -f "$O/HakiosAudio.driver"
codesign -s - -f -i com.scion.hakios-inject "$O/hakios-inject"
codesign -s - -f -i com.scion.hal-probe "$O/hal_probe"
codesign -s - -f "$O/hakios.bluetool.dylib" "$O/hakios.bluetoothd.dylib"
cp -R "$H/bluetool" "$O/bluetool"
(cd "$O" && find . -type f | sort | xargs shasum -a 256)
