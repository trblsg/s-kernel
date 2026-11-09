# iOS 27 on the S9: the full root and the one-script build (`tools/ios27/fullroot/`)

`build-full-root.sh` turns the device's own decrypted iOS 27.0.1 (24A446, N104AP) OS / SystemOS / AppOS images into a flashable raw
HFSX root for the Galaxy S9's USERDATA (volume at byte 0, sparse file, 32 GiB by default; ~20 GiB used). Nothing Apple is in this
repository: the script reads the builder's own images and writes only to the builder's disk.

    sudo tools/ios27/fullroot/build-full-root.sh build          # IPSW images -> out/root-full.img
    sudo tools/ios27/fullroot/build-full-root.sh verify IMG     # fsck + read-only attach + offline checks
    tools/ios27/fullroot/build-full-root.sh flash SERIAL IMG [BOOT]   # TWRP; the serial is required and pinned in every adb call
    tools/ios27/fullroot/verify-boot.sh SERIAL BOOT LABEL [MIN] # one verification boot, timestamped console capture, boot-metrics.py

## "Full" means
Every file of the OS image and of the SystemOS + AppOS cryptexes. The earlier ("trimmed") builds left out, to fit 16-24 GiB:
`/System/Library/{LinguisticData,PreinstalledAssetsV2,NanoTimeKit,Health,OnBoardingBundles,Audio,Assistant}`, all non-English
`*.lproj`, `*.espresso.*`, `*.mlmodelc`, the TTS/CVNLP model directories and 39 of the 50 stock apps (kept in
`/private/var/staged_system_apps`). That is ~4.3 GiB; the full root puts all of it back and installs every stock app into
`/Applications`. Still left out: the two `dyld_shared_cache_arm64e.symbols` files (1.2 GB, debug symbols only).
No asset catalog, icon or `.icns` was in the trimmed set (checked file by file against the images), so the trim is not what makes
some Settings rows lack icons.

## Pipeline
1. `pipeline/cache-pipeline.sh` (unprivileged): copy the stock shared cache, `a64e-rewrite` (arm64e -> arm64), `stubscan --fix`
   (stub islands outside image `__TEXT`), `lse-rewrite` (hot LSE atomics -> LL/SC thunks), `a64e-rewrite2` (PACGA / LDRA writeback),
   the builder's own single-word cache patches (`$IOS27_PRIVATE/patches.txt`, not in the repo), then the full AOT rewrite
   (`aot/rewrite-grow.py`: every remaining non-ARMv8.0 instruction class, stubs in grown TEXT mappings) -> `fix-cs.py` ->
   `grow-check.py` (replay of the kernel's shared-region mapping rules; must print `FAILS 0`). The AOT pass needs an instruction
   "oracle" table generated from the builder's own cache (`$IOS27_PRIVATE`/`aot/oracle/`, not in the repo).
2. `pipeline/root/stage-full-root.sh` (from `tools/ios27/stage-ios-root.sh`): the full copy, the rewritten cache hard-linked into
   `/System/Library/Caches`, every stock app installed.
3. `steps.d/NN-name.sh ROOT` in name order, each idempotent, `ROOT` = the mounted volume with owners on, run as root. A fix from
   another branch is one more file in that directory:
   `10 macho-a64e` (PAC rewrite of every arm64e Mach-O outside the caches) | `20 launchd` (hardware + crash-loop daemons only,
   `_PanicOnCrash` dropped; `lists/`; `DISABLE_HOGS=1` adds the opt-in CPU-hog list) | `30 overlay` (builder's userland shims,
   manifest-checked) | `40 prefs` (Reduce Motion / Reduce Transparency OFF, CA compression off, watchdogs off) |
   `50 xpc-disable` (MessagesBlastDoorService) | `60 wifi-firmware` (inert data, only read with the Wi-Fi boot-args).
4. `check-root.sh`: required files, cache hard links, patched words, launchd plist sanity, prefs, Mach-O rewrite complete.

## Daemon policy
The real iOS 27 runs: only jobs for hardware the port has no driver for (`lists/daemons-hw.list`) and measured crash loops
(`lists/daemons-crashloop.list`) are moved to `launchd-disabled.plist`. The CPU-hog list is opt-in and off.
