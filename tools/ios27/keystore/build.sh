#!/bin/sh
# Build dptest (docs/ios27/KEYSTORE.md) for arm64e and arm64, ad hoc signed with its entitlements. Needs Xcode: DEVELOPER_DIR.
set -e
cd "$(dirname "$0")"
: "${DEVELOPER_DIR:?set DEVELOPER_DIR to your Xcode (its iPhoneOS SDK is used)}"; export DEVELOPER_DIR
for A in arm64e arm64; do
  nice -n 10 xcrun --sdk iphoneos clang -arch $A -mios-version-min=16.0 -fobjc-arc -O1 -g -Wall -Wno-deprecated-declarations \
    -o dptest.$A dptest.m -framework Foundation -framework Security -framework IOKit -framework CoreFoundation
  codesign -f -s - --entitlements dptest.entitlements dptest.$A
done
cp dptest.arm64 dptest   # default installed binary = plain arm64 (what the other jobs here run)
