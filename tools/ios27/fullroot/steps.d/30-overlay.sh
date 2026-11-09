#!/bin/bash
# 30-overlay.sh ROOT : Scion/Starbite userland + local-only stand-ins from overlay/ (manifest-checked), root:wheel.
#   usr/lib/{s,m,b,sbca,sbmtlc}.dylib (sweetinject: sbca GL renderer + Preferences hook, wallpaper/Metal shim, brightness shim),
#   usr/local/starbite/lib/{libOSMesa.8,libsbgl}.dylib (Mesa/Panfrost GLES), StarbiteMetal.bundle, usr/local/bin/{m2vtest,mtl27test}
#   (test tools, no launchd job), usr/libexec/cc_fips_test (fips-ok stand-in for the PAC-rewritten libcorecrypto; original kept as .orig),
#   24 wallpaper HEIC layers as PNG data, var/local/{mtl2-clients,mtl2-env,oslog-clients,sbwallpaper.png}.
set -euo pipefail
R=$1; O=${OVERLAY:-${IOS27_PRIVATE:-$(cd "$(dirname "$0")/.." && pwd)/private}/overlay}
(cd "$O" && shasum -a 256 -c MANIFEST.sha256 >/dev/null) || { echo "overlay manifest mismatch" >&2; exit 1; }
[[ -e $R/usr/libexec/cc_fips_test.orig ]] || cp -p "$R/usr/libexec/cc_fips_test" "$R/usr/libexec/cc_fips_test.orig"
n=0
while IFS= read -r f; do
    f=${f#./}; mkdir -p "$R/$(dirname "$f")"
    cat "$O/$f" > "$R/$f.new.$$" && chmod "$(stat -f %Lp "$O/$f")" "$R/$f.new.$$" && chown 0:0 "$R/$f.new.$$" && mv -f "$R/$f.new.$$" "$R/$f"
    n=$((n+1))
done < <(cd "$O" && find . -type f ! -name MANIFEST.sha256 | sort)
chown -R 0:0 "$R/System/Library/Extensions/StarbiteMetal.bundle" "$R/usr/local" "$R/private/var/local" 2>/dev/null || true
install -d -m 0777 "$R/private/var/local" "$R/private/var/local/oslog" "$R/private/var/local/m2v-cache" "$R/private/var/tmp"
chmod 0777 "$R/private/var/local/oslog" "$R/private/var/local/m2v-cache"
chmod 0755 "$R/private/var/local"
echo "overlay: $n files installed"
