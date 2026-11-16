# Metal on iOS 27 for the Galaxy S9: Starbite's Metal stack rebuilt, and what ran (8 and 9 November 2026)

iOS 27 on the S9 renders with OpenGL ES through `sbca` ([GPU-PATH.md](GPU-PATH.md)). This page is about the other route: Apple's Metal
framework on the Mali-G72, through a Metal plug-in that Starbite wrote for iOS 26 (a Metal 2 frontend, `HotMetal2.mm` and
`NativeBridgeMetalFrontend.mm`; metal2vulkan's bridge; Mesa's PanVK over the kernel's StarbiteMali). The scripts and tools in
`tools/ios27/metal/` rebuild that stack for iOS 27.0.1 and test it. Starbite's own part of it is in `tools/ios27/metal2/` (the two frontend files,
`HotMetal2.mm` and `NativeBridgeMetalFrontend.mm`, `build-m2v-bridge.sh`, the bridge's Rust sources and its patches 0001-0016 and 0100-0102,
`Info.plist`, `m2vtest.c`; MIT, credited below), so the scripts and the three patches in `tools/ios27/metal/patches/` work from this repository
as it is. No binary of any kind is in the repository.

## What is here (`tools/ios27/metal/`)

| File | What |
|---|---|
| `build-host.sh` | SPIRV-Headers, SPIRV-Tools (Vulkan SDK 1.4.363.0) and SPIRV-LLVM-Translator v23.1.3, against Homebrew's llvm 23: the host tools Mesa 26's `mesa_clc` needs |
| `mk-panvk27.sh` | `tools/ios27/mesa-ios/mk-panvk26-ios.sh` with its paths from the environment: the host tools (`mesa_clc`, `vtn_bindgen2`, `panfrost_compile`), then PanVK of Mesa 26.2.6 for iOS arm64 (the patches are `tools/ios27/mesa-ios/mesa26-panvk-ios.patch`, `-shader-size-cap`, `-bifrost-ra-multispill`) |
| `rust.sh`, `bridge.sh` | Rust 1.98.0 with the iOS target, then metal2vulkan's bridge (`libnative_metal_bridge.a`) through `tools/ios27/metal2/build-m2v-bridge.sh` (it clones metal2vulkan at 720ef1ce and applies the bridge patches) |
| `mk-bundle27.sh` | `StarbiteMetal.bundle` (frontend, bridge, PanVK; arm64, ad hoc signed) and `m2vtest`; the frontend and `HotMetal2.mm` are built from a copy with `frontend-dexr.diff` and `hotmetal2-diag.diff` applied (`NOPATCH=1`: as they are in `tools/ios27/metal2`) |
| `mtl27test.m` | the smoke test, six steps: the kernel's services (StarbiteMali, `IOAcceleratorES`, `IOCoreSurfaceRoot`), IOSurface, `MTLCreateSystemDefaultDevice()` through Metal's own discovery (or `--direct`), a render-pass clear read back, a render pass into an IOSurface-backed texture (what CoreAnimation does), and QuartzCore's compute and render shaders into pipelines |
| `sbmtlc.c`, `sbwall.m` | `sbmtlc.dylib`, a library for `sweetinject`: any inserted library makes the kernel start the program with `ptrauth_disabled=1` (xnu `0324`), which is what lets dyld take the arm64 plug-in into an arm64e process; it also captures `os_log` (logd is off on this root: an in-place hook of libsystem_trace's four `_os_log_*impl` entry points, opt-in by `/private/var/local/oslog-clients`) and `sbwall` draws a static wallpaper in SpringBoard from `/private/var/local/sbwallpaper.png` (opt-in: `install-metal27.py --wallpaper FILE` takes the image from the builder's own root, or any file on the Mac, at install time; no image is shipped here) |
| `install-metal27.py` | puts the stack into an attached iOS 27 root (`STAGE=<dir> sudo python3 install-metal27.py /Volumes/ROOT [--clients "mtl27test PosterBoard CollectionsPoster"] [--oslog "PosterBoard"] [--wallpaper FILE] [--no-job] [--uninstall]`), keeps every file it replaces as `NAME.before-metal27`, adds two run-once launchd jobs (`com.starbite.m2vtest`, `com.starbite.mtl27test`) |
| `mtl-cmdline.py` | a command line plus `sbmtl=1` (`--posters`: `sbmtlc` into PosterBoard and CollectionsPoster; `--metal-ca`: `sbgl=0`), staying under the 608-byte limit |
| `patches/` | `frontend-dexr.diff` (`SWEET_MTL2_DEXR=1`, see below, and a log line for each new texture shape), `hotmetal2-diag.diff` (an "opening device" log line), `starbite-drm-diag.diff` (a console line on the StarbiteMali connection) |

Licences (`LICENSES.md`): the files here are Scion's, MIT. The frontend and the bridge are Starbite's, MIT (they come from IOS-26-Hot and, before it, UniXNU's
AdrenoMetal, credited in `NOTICE`); metal2vulkan (steelbrain) is GPL-2.0-or-later OR LGPL-3.0-or-later and is linked statically into the bundle (ship its source with any
binary); Mesa is MIT.

## The kernel needs nothing new

StarbiteMali (`0301`, `0305`, and the Mali fixes of the `03xx` range: among them no write-combine for imported and client memory) and StarbiteAccelerator (`0336`; class `IOAcceleratorES`,
`MetalPluginName` StarbiteMetal, device class `HotMTL2Device`, boot-argument `sbmtl=1`) are in the iOS 27 series. iOS 27's Metal still matches
`IOAcceleratorES` plus `MetalPluginName`/`MetalPluginClassName` and loads the plug-in from `/System/Library/Extensions` (checked in the 24A446 cache: the strings
are in Metal and IOGPU, `_MTLDevice` is exported).

## Install, test, switch off

1. Build the stack (above), then install it into an attached root with `install-metal27.py`; give the command line `sbmtl=1` (`mtl-cmdline.py`).
2. On the USB log: `STARBITE MTL: IOAcceleratorES published` (kernel), then `MTL2[pid mtl27test]: plugin loaded`, `native-metal: _MTLDevice is N bytes`
   (or `MISSING <class> -<selector>`), `MTL27TEST: 1 kernel .. 6 shaders PASS|FAIL`, `RESULT PASS` (also `/private/var/tmp/mtl27test.log`); `/private/var/tmp/m2vtest.log`
   has `step 1 device` to `step 5 import` (step 6, QuartzCore, is skipped on purpose).
3. Failure modes: step 3 gives nil and no "plugin loaded": Metal never loaded the bundle (check `sbmtl=1`, the path, the signature); "plugin loaded" and
   "device refused": PanVK or StarbiteMali (`mtl2.log`, `m2vtest.log`).
4. Off: drop `sbmtl=1` (Metal finds no device again), or empty `/private/var/local/mtl2-clients`, or `install-metal27.py --uninstall`.

## What ran

On a Galaxy S9 (8 November, the iOS-target kernel, a root that is the builder's own): `mtl27test` steps 1 to 5 passed: `MTLCreateSystemDefaultDevice()`
returned "Mali-G72 MC18", the clear read back exact, the IOSurface render exact. `m2vtest` (the bridge straight on PanVK): compute, render and import, no bad pixel. Step 6
failed from the test's own shader pairing, since fixed. PosterBoard gets a Metal device (20 s to open). The wallpaper is still black: PosterBoard's data
store is empty and no poster extension is ever started (although LaunchServices lists the `com.apple.posterkit.provider` extensions), so `sbwall` is a stop-gap.

On 9 November, with `sbgl=0` and backboardd in `mtl2-clients` (CoreAnimation's own Metal renderer in backboardd), CoreAnimation did run on Metal: `MTL2[backboardd]: device
Mali-G72 MC18 in 4183 ms`, about 4.5 thousand pipelines cached in `m2v-cache` (the first boot spent about a minute of CPU on shader compiles). It also failed: 18 command
buffers were refused at `variable_blur_clamp_downsample_frag_lph` (sampling an XR10 texture through a mip-filtered sampler is `NotImplemented(Format)` in metal2vulkan) and 12 no-copy
buffers were `NotRepresentable`. `patches/frontend-dexr.diff` is the answer tried for the first: with `SWEET_MTL2_DEXR=1` (in `mtl2-env`, off by default) the extended-range
formats (BGRA10_XR 552/553, BGR10_XR 554/555) of textures that live in device memory only are held as RGBA16Float, with the pipelines' attachments to match. **Not run** when this
was written: that patch on a phone, and the frame rate of CoreAnimation on Metal against `sbca`'s OpenGL ES path.

What came of the frame rate question is in [LIQUID-GLASS.md](LIQUID-GLASS.md): where the Metal path lost its time, what was changed, and the runs.

## Not done

- The wallpaper (above), and the 12 `NotRepresentable` no-copy buffers.
- The decmpfs type 16 of nine SplashBoard `*.ktx` snapshots (the kernel registers type 1 only): unrelated to Metal.
- The scripts were run against Starbite's tree (`SB=`) before `tools/ios27/metal2/` was imported; the import changed their paths only (`tools/metal2` to `tools/ios27/metal2`, the repository as the tree). `bridge.sh` and `mk-bundle27.sh` were not re-run on a phone for it.
