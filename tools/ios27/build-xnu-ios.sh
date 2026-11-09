#!/usr/bin/env bash
# tools/ios27/build-xnu-ios.sh [-jN]: the iOS-target XNU of the ios-27 profile (the Galaxy S9, STARLTE, on xnu-12377.121.6). This is what
# `OS_PROFILE=ios-27 make kernel` runs, through tools/make-kernel.sh (the profile's `builder dxb-ios`; docs/ios27/IOS27-TARGET.md).
#
#   DEVELOPER_DIR=<Xcode 16.4 or 26> KDKROOT=<unpacked Kernel Debug Kit 26.5 (25F71)> OS_PROFILE=ios-27 make kernel   -> ./hakios-kernel
#   tools/ios27/build-xnu-ios.sh -j4           the same build on its own; the full log on stdout
#   tools/ios27/build-xnu-ios.sh --kernel-path where the kernel is left (build/dxb-12377/build/xnu-ios.obj/DEVELOPMENT_ARM64_STARLTE/mach.development.starlte)
#   FRESH=1 ...                                start from a clean source tree and a new darwin-xnu-build
#
# It makes (once) build/dxb-12377: blacktop's darwin-xnu-build at the commit of tools/ios27/dxb.pin, the profile's tree in it
# (xnu/apply-series.sh: the fork's commit, the staged files, the shared drivers), as tools/ios27/dxb-common.sh does. It then copies
# dxb's build.sh to build-ios.sh with tools/ios27/dxb-ios.patch (the iOS target: object directory xnu-ios.obj, SDKROOT=iphoneos,
# no minimal UBSan, the firehose library from build/firehose-ios/) and runs it, with a hook the patch has: after dxb has built
# libfirehose_kernel.a against the macOS SDK, tools/ios27/firehose-ios.py makes the iOS one from it (LC_BUILD_VERSION: ld refuses a
# macOS object in an iOS link). Every step is incremental: the second run builds what changed.
# The S9's Wi-Fi driver object is linked in when tools/mk-brcmfmac.sh made one (xnu-deps/build/brcmfmac/out/brcmfmac.o).
# Needs: a Mac, Xcode (DEVELOPER_DIR), the Kernel Debug Kit (KDKROOT: Apple's, not in this repository), and jq, gum, xcodes, cmake and
# ninja on PATH (TOOLS_BIN=<directory> adds one). A full build takes about 25 minutes at -j4.
# The exit status is dxb's. The kernel is removed first, so one that is there afterwards was linked by this run; the kernel's symbol-set
# steps do not fail on this configuration (tools/make-kernel.sh, which calls this, judges the log all the same).
set -euo pipefail
cd "$(dirname "$0")/../.."
ROOT=$PWD
DXB_WHO=build-xnu-ios
. "$ROOT/tools/ios27/dxb-common.sh"

jobs=4
while [ $# -gt 0 ]; do
	case "$1" in
	-j[0-9]*) jobs=${1#-j} ;;
	-j) shift; jobs=${1:-}; [[ $jobs =~ ^[0-9]+$ ]] || dxb_die "-j wants a number" ;;
	--kernel-path) dxb_kernel_ios; exit 0 ;;
	-h | --help) sed -n '2,/^set -euo/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; exit 0 ;;
	*) dxb_die "unknown argument $1 (-jN, --kernel-path)" ;;
	esac
	shift
done
[[ $jobs =~ ^[0-9]+$ ]] && [ "$jobs" -ge 1 ] || dxb_die "-j wants a number"

# the profile: this builder is the one of a profile that asks for it (`builder dxb-ios`)
export OS_PROFILE=${OS_PROFILE:-$(python3 "$ROOT/tools/os-profile.py" name)}
[ "$(OS_PROFILE_QUIET=1 python3 "$ROOT/tools/os-profile.py" get builder 2>/dev/null || true)" = dxb-ios ] ||
	dxb_die "the profile $OS_PROFILE does not ask for this builder (builder dxb-ios in profiles/$OS_PROFILE.conf): OS_PROFILE=ios-27"
dxb_env
dxb_setup

cd "$DXB"
cp build.sh build-ios.sh
patch -s -p1 build-ios.sh < "$ROOT/tools/ios27/dxb-ios.patch" ||
	dxb_die "tools/ios27/dxb-ios.patch does not apply to darwin-xnu-build $DXB_SHA's build.sh (tools/ios27/dxb.pin)"
export XNU_FIREHOSE_IOS=$ROOT/tools/ios27/firehose-ios.py XNU_JOBS=$jobs
# The S9's Wi-Fi radio driver: brcmfmac's object (ISC; tools/mk-brcmfmac.sh), when one was made, is linked into the kernel
# (HAKIOS_OBJECTS, which the fork's makedefs/MakeInc.def adds to the link), else hakios.lx.c says the driver is not built in.
# Its debug map is stripped first: the kernel's CTF step cannot read Linux's types. make does not see the object change, so
# the kernel is linked again when it has (a stamp, as xnu-build.sh keeps for the other profiles).
LX=$ROOT/xnu-deps/build/brcmfmac/out/brcmfmac.o
lx_now=none
if [ -f "$LX" ]; then
	xcrun strip -S -o "$DXB/brcmfmac.o" "$LX" || dxb_die "cannot strip $LX"
	export XNU_EXTRA_MAKEARGS="${XNU_EXTRA_MAKEARGS:-} HAKIOS_OBJECTS=$DXB/brcmfmac.o"
	lx_now=$(shasum -a 256 "$DXB/brcmfmac.o" | cut -c1-16)
fi
if [ "$(cat "$DXB/.hakios-objects" 2>/dev/null)" != "$lx_now" ]; then
	rm -f "$(dirname "$(dxb_kernel_ios)")"/mach.*.unstripped "$(dirname "$(dxb_kernel_ios)")"/mach.*.unstripped.noctf
	echo "$lx_now" > "$DXB/.hakios-objects"
fi
KERNEL=$(dxb_kernel_ios)
rm -f "$KERNEL"
start=$(date +%s)
rc=0
MAKEFLAGS=${MAKEFLAGS:--k} bash build-ios.sh || rc=$?
if [ ! -f "$KERNEL" ]; then
	echo "build-xnu-ios: no kernel was linked: $KERNEL" >&2
	exit $((rc ? rc : 1))
fi
echo "build-xnu-ios: kernel built in $(( $(date +%s) - start )) s: $KERNEL"
exit $rc
