#!/bin/bash
# step-lsd-extkit-cache.sh ROOT_MOUNTPOINT   (root image attached with -owners on, run as root via sudo)
# Idempotent. Makes LaunchServices register the framework-declared extension points
# (/System/Library/xpc/extensions.plist: com.apple.posterkit.provider, com.apple.widgetkit-extension, ... 126 points).
#
# Why: ExtensionFoundation (EXExtensionPointEnumerator, used by lsd) only accepts the xpc extension-point cache after
# verifying extensions.plist.sig against the AMFI trust cache, unless FeatureFlag ExtensionKit/enforce_xpc_cache_code_signing
# is off (or AMFI op 0x60 reports bootarg bits 0x86). The Scion/Starbite kernel answers op 0x60 with 0 and has no AMFI
# trust-cache service, so the cache was rejected and PosterBoard logged
#   extension point not found for query `EXQuery: extension point `Optional("com.apple.posterkit.provider")` platforms: 2`
# Fix: FeatureFlags override (libsystem_featureflags reads /Library/Preferences/FeatureFlags/Domain/<Domain>.plist;
# /Library/Preferences -> /private/var/preferences), and drop an LS database that was built without those points so
# lsd rebuilds it at next boot (only when it lacks them, so re-running is a no-op).
# Rollback: rm ROOT/private/var/preferences/FeatureFlags/Domain/ExtensionKit.plist (and delete the csstores again).
set -eu
R=${1:?usage: $0 ROOT_MOUNTPOINT}
[ -d "$R/System/Library/xpc" ] || { echo "not a root: $R"; exit 1; }
D=$R/private/var/preferences/FeatureFlags/Domain
mkdir -p "$D"
chown 0:0 "$D"; chmod 755 "$D"
F=$D/ExtensionKit.plist
cat > "$F.tmp" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>enforce_xpc_cache_code_signing</key>
	<dict>
		<key>Enabled</key>
		<false/>
	</dict>
</dict>
</plist>
EOF
if [ -f "$F" ] && cmp -s "$F" "$F.tmp"; then rm -f "$F.tmp"; echo "flag: already set"; else mv "$F.tmp" "$F"; echo "flag: written"; fi
chown 0:0 "$F"; chmod 644 "$F"
plutil -lint "$F"
# stale LS databases (built while the xpc cache was rejected): no PosterKit.framework declaring-framework record
n=0
for c in "$R"/private/var/mobile/Containers/Data/InternalDaemon/*/Library/Caches/*com.apple.LaunchServices-*.csstore; do
  [ -f "$c" ] || continue
  if LC_ALL=C grep -a -q 'PrivateFrameworks/PosterKit.framework' "$c"; then echo "csstore ok: $c"; continue; fi
  rm -f "$c"; n=$((n+1)); echo "csstore removed (stale): $c"
done
echo "STEP-LSD-EXTKIT-DONE removed=$n"
