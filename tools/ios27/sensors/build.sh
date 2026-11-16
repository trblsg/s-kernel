#!/bin/sh
# SPDX-License-Identifier: MIT-0
# tools/ios27/sensors/build.sh [OUT]: the orientation library for the iOS 27 root (orientation.c) -> OUT (o.dylib here).
# Needs Xcode's iPhoneOS SDK (DEVELOPER_DIR). arm64, signed ad hoc, as the other inserted libraries of this root.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$HERE/o.dylib}
xcrun --sdk iphoneos clang -arch arm64 -mios-version-min=16.0 -O2 -Wall -Wextra -dynamiclib -install_name /usr/lib/o.dylib \
	-framework CoreFoundation -lobjc "$HERE/orientation.c" -o "$OUT"
codesign -f -s - "$OUT"
shasum -a 256 "$OUT"
