#!/bin/bash
# FULL-ROOT variant of tools/stage-ios-root.sh (GPL material from Starbite stays on the mini): no omit list, CACHE_SRC override.
# Stage an iOS / iPadOS root on an already-mounted HFSX volume.
# From the herolte fork's tools/a5xelte-stage-ios-root.sh (iOS 16); changes:
#   - PaperBoard is optional (merged into PosterBoard in iOS 18).
#   - INSTALL_STAGED_APPS=1: the removable stock apps (Safari, Photos, Camera,
#     Mail, App Store, ...) live in /private/var/staged_system_apps and are only
#     installed by DataMigration's SystemAppMigrator on a real first boot, which
#     the tuned root skips. They go into /Applications instead (LaunchServices
#     registers system apps from there), and their placeholders in
#     /System/Library/AppPlaceholders (same bundle IDs) are left out.
# Run as root: iOS UID/GID values are part of the boot contract.
set -euo pipefail

if [[ $# != 4 || ${EUID} != 0 ]]; then
    echo "usage (as root): $0 OS_MOUNT SYSTEMOS_MOUNT APPOS_MOUNT HFSX_MOUNT" >&2
    exit 2
fi
os=$1 sysos=$2 appos=$3 root=$4
# /Applications also contains launchd-managed system UI services, not just
# optional home-screen apps. Require their source before starting any staging.
ui_apps=(PosterBoard PreBoard InputUI MediaRemoteUI ClockAngel)
[[ -f "$os/Applications/PaperBoard.app/PaperBoard" ]] && ui_apps+=(PaperBoard)
for app in "${ui_apps[@]}"; do
    [[ -f "$os/Applications/$app.app/$app" ]] || {
        echo "missing required UI source: $app" >&2; exit 1;
    }
done
# iPadOS 18 (A10) ships dyld_shared_cache_arm64; iOS 26 is arm64e only.
dsc="$sysos/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64"
[[ -f $dsc ]] || dsc=${dsc}e
for source in "$os/sbin/launchd" "$os/System/Library/CoreServices/SpringBoard.app/SpringBoard" "$dsc"; do
    [[ -f "$source" ]] || { echo "missing source: $source" >&2; exit 1; }
done
[[ -d "$appos/System" && -d "$root" ]] || exit 1
[[ $(stat -f %u "$os/sbin/launchd") == 0 ]] || {
    echo "OS image was mounted without owners; remount with -owners on" >&2
    exit 1
}
[[ $(stat -f %u "$dsc") == 0 ]] || {
    echo "SystemOS image was mounted without owners" >&2
    exit 1
}
mount | grep -F " on $root (hfs," >/dev/null || {
    echo "destination must be a mounted HFSX volume" >&2
    exit 1
}
[[ ! -e "$root/sbin/launchd" ]] || {
    echo "destination already has launchd; refusing to overlay a partial root" >&2
    exit 1
}

# Keep the selected display/lock-screen/input service bundles. /Applications
# also contains system services, so a blanket exclusion is not safe. The user
# requested only display-related restoration, not every stock app.
app_filters=('--include=/Applications/')
for app in "${ui_apps[@]}"; do
    app_filters+=("--include=/Applications/$app.app/***")
done
app_filters+=('--exclude=/Applications/***')
# STAGE_ALL_APPS=1 keeps every stock app (Settings, Photos, ...; about
# 215 MiB): the HEROLTE root has room for them.
if [[ ${STAGE_ALL_APPS:-0} == 1 ]]; then
    # Not an empty array: macOS bash 3.2 with set -u treats it as unbound.
    app_filters=('--include=/Applications/***')
fi
# FULL ROOT: nothing omitted (build-full-root.sh; the only things left out are the two *.symbols files).
omit=()
# TRIM_IOS26=1: iOS 26 is ~18 GB uncompressed (APFS compression is lost on HFSX) and the
# root must fit 16 GiB. Leave out non-English localizations and the photo / voice /
# search ML data (Espresso weights, Core ML models, TTS voices, word models); none of
# it is needed by the UI, the daemons fall back or skip their analysis.
trim=()
if [[ ${TRIM_IOS26:-0} == 1 ]]; then
    trim=('--include=en.lproj/***' '--include=Base.lproj/***' '--include=English.lproj/***'
          '--include=en_*.lproj/***' '--exclude=*.lproj/'
          '--exclude=*.espresso.*' '--exclude=*.mlmodelc/'
          '--exclude=/System/Library/PrivateFrameworks/VoiceServices.framework/TTSResources/'
          '--exclude=/System/Library/PrivateFrameworks/TextToSpeechMauiSupport.framework/TTSResources/'
          '--exclude=/System/Library/PrivateFrameworks/CVNLP.framework/Models/')
fi
echo "Copying selected OS files"
/usr/bin/rsync -aH --numeric-ids --extended-attributes ${trim[@]+"${trim[@]}"} "${app_filters[@]}" ${omit[@]+"${omit[@]}"} "$os/" "$root/"
echo "Copying SystemOS and AppOS cryptexes"
mkdir -p "$root/private/preboot/Cryptexes/OS" "$root/private/preboot/Cryptexes/App"
# CACHE_SRC: the S9-rewritten shared cache (tools/pipeline/cache-pipeline.sh) replaces the stock one; every other SystemOS file is copied as is.
cexcl=()
[[ -n ${CACHE_SRC:-} ]] && cexcl=('--exclude=/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64e*')
/usr/bin/rsync -aH --numeric-ids --extended-attributes --exclude="*.symbols" ${cexcl[@]+"${cexcl[@]}"} "$sysos/" "$root/private/preboot/Cryptexes/OS/"
if [[ -n ${CACHE_SRC:-} ]]; then
    cdst="$root/private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld"
    mkdir -p "$cdst"
    for f in "$CACHE_SRC"/dyld_shared_cache_arm64e*; do
        cp -p "$f" "$cdst/"; chown 0:0 "$cdst/${f##*/}"
    done
    echo "installed rewritten cache: $(ls "$cdst" | wc -l) files from $CACHE_SRC"
fi
/usr/bin/rsync -aH --numeric-ids --extended-attributes "$appos/" "$root/private/preboot/Cryptexes/App/"

# The donor's ignition_level=1 boot looks for the shared cache at this path.
# Hard links use no second copy of the roughly 3 GiB cache.
cache="$root/private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld"
mkdir -p "$root/System/Library/Caches/com.apple.dyld"
for file in "$cache"/*; do
    [[ -f "$file" ]] || continue
    ln "$file" "$root/System/Library/Caches/com.apple.dyld/${file##*/}"
done

# dyld may request an individual cryptex dylib before full cryptex mounting.
for cryptex in OS App; do
    source="$root/private/preboot/Cryptexes/$cryptex"
    while IFS= read -r -d '' file; do
        relative=${file#"$source"/}
        [[ $relative == System/Library/Caches/com.apple.dyld/* ]] && continue
        target="$root/$relative"
        [[ -e $target || -L $target ]] && continue
        mkdir -p "${target%/*}"
        if [[ -L $file ]]; then
            ln -s "$(readlink "$file")" "$target"
        else
            ln "$file" "$target"
        fi
    done < <(find "$source" ! -type d -print0)
done

# INSTALL_STAGED_APPS=1: all of them; or a list of names, e.g. "MobileSafari Calculator".
if [[ ${INSTALL_STAGED_APPS:-0} != 0 ]]; then
    staged="$os/private/var/staged_system_apps"
    [[ -d $staged ]] || { echo "no $staged" >&2; exit 1; }
    for bundle in "$staged"/*.app; do
        name=${bundle##*/}
        if [[ $INSTALL_STAGED_APPS != 1 && " $INSTALL_STAGED_APPS " != *" ${name%.app} "* ]]; then
            continue
        fi
        /usr/bin/rsync -aH --numeric-ids --extended-attributes "$bundle" "$root/Applications/"
        chown -R 0:0 "$root/Applications/$name"
        rm -rf "$root/System/Library/AppPlaceholders/$name"
        echo "installed stock app: $name"
    done
fi

db="$root/private/var/db"
install -d -o 272 -g 272 -m 0751 "$db/diagnostics"
install -d -o 272 -g 272 -m 0755 "$db/uuidtext" "$db/diagnostics/timesync"
for dir in diagnostics uuidtext diagnostics/timesync; do
    install -o 272 -g 272 -m 0644 /dev/null "$db/$dir/.migrated"
done
install -d -o 0 -g 0 -m 0700 "$root/private/var/keybags"
printf '%s\n' '<?xml version="1.0" encoding="UTF-8"?>' \
    '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">' \
    '<plist version="1.0"><dict><key>KeyBagKeys</key><data>U1dFRVQ=</data></dict></plist>' \
    > "$root/private/var/keybags/systembag.kb"
chown 0:0 "$root/private/var/keybags/systembag.kb"
chmod 0600 "$root/private/var/keybags/systembag.kb"

[[ $(stat -f %u "$root/sbin/launchd") == 0 ]] || {
    echo "launchd ownership was not preserved" >&2
    exit 1
}
[[ -f "$root/System/Library/CoreServices/SpringBoard.app/SpringBoard" ]] || exit 1
for app in "${ui_apps[@]}"; do
    [[ -f "$root/Applications/$app.app/$app" ]] || {
        echo "required UI executable missing after copy: $app" >&2; exit 1;
    }
done
# iPadOS 18 (A10) ships dyld_shared_cache_arm64; iOS 26 is arm64e only.
[[ -f "$root/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64" ||
   -f "$root/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64e" ]] || exit 1
sync
df -h "$root"
echo "staged root complete; validate contents and filesystem before flashing"
