# Third-party licenses

Every third-party component HakiOS redistributes or fetches at build time, with its origin,
license, and status. Where "origin uncertain" or "to verify" is written, the item is **not**
asserted to be cleared for redistribution; treat it as pending review (see `COMPLIANCE.md`).

This supersedes the summary in `NOTICE`, which stays as the short, canonical list.

## Redistributed in this repository

| Component | Path | Origin | License | Notes |
|---|---|---|---|---|
| XNU modifications | the fork `Scion-Project/xnu` (branch `scion/xnu-8796`), `drivers/` | derivative of Apple XNU | APSL 2.0 (`LICENSE.APSL`) | base tag `xnu-8796.121.2`, fetched by `xnu/apply.sh` |
| ios-on-pixel-10 (TheAndersMadsen) | `devices/blazer/carried/` | its author's own kernel-side files, from Scion-Project/scion-blazer at `f04fd1f` | MIT (by each file's header; `devices/blazer/carried/exit_trace/LICENSE`) | used with the author's permission, credit kept; not built. Its third-party parts are not here: `docs/two-bases.md` |
| libdeflate | `scion/xnu-8796:bsd/miscfs/apfs/libdeflate/` | Eric Biggers | MIT | see its `COPYING` |
| libzbitmap | `scion/xnu-8796:bsd/miscfs/apfs/libzbitmap/`, `xnu-deps/build/lzbitmap/` | Corellium LLC (port of eafer/libzbitmap) | GPL-2.0+ OR MIT | used under MIT |
| Monocypher 4.0.2 | `scion/xnu-8796:osfmk/minicrypto/third_party/monocypher.{c,h}` | Loup Vaillant and others, [monocypher.org](https://monocypher.org) (unchanged) | CC0-1.0 or BSD-2-Clause | Argon2id for the key store; see `third_party/README.md` |
| BearSSL `aes_ct64` | `scion/xnu-8796:osfmk/minicrypto/third_party/bearssl_*` | Thomas Pornin, [bearssl.org](https://bearssl.org), commit 7bea48e5e850 (only the include line changed; `bearssl_inner.h` is cut down from its `inner.h`) | MIT | constant-time AES for the key store |
| SoftwareAES (UniXNU) | `scion/xnu-8796:iokit/Kernel/MiniIOAESAccelerator.cpp` | ArmVirtPlatform contributors, github.com/UniXNU/SoftwareAES at `4a45326da5c2` | BSD-2-Clause-Patent | the whole file is under it; its notice is in the file's head |
| UniXNU's xnu-12377 fixes | `commit 0096 of scion/xnu-12377`, `0097-pmap-sync-64bit-pa.patch`, `0098-native-pac-user-jop.patch` | UniXNU contributors, github.com/UniXNU/UniXNU at `a9ec98c` (commits `f65660d`, `a11553a`) | BSD-2-Clause-Patent for their lines; APSL 2.0 as patches to Apple's XNU | notice in `LICENSES/BSD-2-Clause-Patent-UniXNU.txt`; each patch's head says what is theirs |
| LinuxKPI / Linux headers | `wifi/lx/include/linux/`, `wifi/lx/include/net/` | Linux / FreeBSD LinuxKPI | GPL-2.0 (Linux) / BSD-2-Clause (FreeBSD) | small compatibility headers; see each file's notice |
| brcmfmac patch | `wifi/lx/brcmfmac/patches/` | changes to Linux's brcmfmac (Broadcom) | ISC | the BCM4361's IDs, RAM base and firmware names, and two lines where the kernel's `brcmfmac=N` stops; its context lines are brcmfmac's |
| Linux DRM shim | `tools/mesa/hakios-drm/`, `xf86drm.h` | Linux / X.Org | MIT | build-time shim for Mesa |
| Mesa patches | `tools/mesa/*.patch` (`turnip-darwin.patch` among them) | changes to Mesa, HakiOS | MIT (Mesa's terms for the files they change) | patches only; Mesa itself is fetched |
| AdrenoMetal patch | `tools/m2v/adrenometal-ios16.patch` | changes to UniXNU/AdrenoMetal, HakiOS | MIT, Copyright (c) 2026 NativeMetal contributors | patch only; the source is fetched. `tools/m2v/ios16-compat.h` is our own, MIT |
| metal2vulkan patch | `tools/m2v/metal2vulkan-hakios.patch` | changes to steelbrain/metal2vulkan, HakiOS | GPL-2.0-or-later OR LGPL-3.0-or-later, as the code it changes; **not MIT** | patch only, and its context lines are their code. The file carries no notice of its own: **to verify** that this is enough |
| The window server's Metal plug-in | `userland/src/HakiosMetalCA.m` | begun from vcvkk's `userland/src/HakiosMetal.m` (github.com/vcvkk/darwin-redwood, branch hakios-workspace, at eb9fdf5), with this tree's changes | **to verify**: that repository states no license (read 2026-11-04); this tree's changes are MIT | not in `prebuilt/` |
| AIR / Metal-library tooling | `tools/mesa/air/` | HakiOS (Darwin on redwood contributors) | MIT | our own; LLVM bitcode layout is a published format |
| gfxstream Vulkan guest patch | `tools/gfxstream/vk/mesa-gfxstream-scion.patch` | changes to Mesa's `src/gfxstream` (Google / AOSP) | MIT | applied by `tools/gfxstream/mk-vk.sh`; `tools/gfxstream/vk/scion/` is our own (MIT) |
| AdrenoMetal (Metal frontend and C-ABI bridge) | `userland/src/metalvk/NativeBridgeMetalFrontend.{mm,h}`, `tools/metal/bridge/NativeMetal{Objects,Bridge}.{rs,h}` | UniXNU, github.com/UniXNU/AdrenoMetal at `ce5e19e33e05` ("NativeMetal contributors") | MIT (`LICENSE.AdrenoMetal` beside each) | copied with the notice; Scion's changes marked `Scion:`; METAL=vk only |
| mkbootimg.py | `devices/redwood/prebuilt/mkbootimg.py` | Android Open Source Project | Apache-2.0 | |
| Mu-Silicium / Project Mu derived files | `devices/redwood/uefi/` | Project Silicium, Microsoft, contributors | BSD-2-Clause-Patent; some GPL-2.0+ OR MIT | derived parts keep upstream license |
| Qualcomm/Xiaomi UEFI driver binaries | `devices/redwood/uefi/files/Binaries/` | Qualcomm / Xiaomi (from device `xbl.img`) | vendor's — **to verify** | needed only to rebuild UEFI; removable, re-extract with UEFIReader |
| Galaxy S9 prebuilt UEFI | `devices/starlte/prebuilt/SILICIUM_UEFI.fd`, `BootShim.bin` (scion-starlte) | built from Mu-Silicium `e51cd877e31c` with `uefi/` | BSD-2-Clause, BSD-2-Clause-Patent; contains Carbon Hypervisor (`Hyp.bin`), GPL-2.0 | build recipe and hashes in `devices/starlte/prebuilt/README.md`; not yet byte-reproducible |
| Samsung / Broadcom firmware (Galaxy S9: audio, Wi-Fi, Bluetooth, sensor hub) | not in the repository: `devices/starlte/firmware/*/SHA256SUMS` and `devices/starlte/tools/fetch-firmware.sh` (scion-starlte) fetch it | Samsung / Maxim / Cirrus / Broadcom, via the LineageOS vendor trees | vendor's: **to verify** | pulled by the user's build from their own copy (`--from`, e.g. the phone's `/vendor/firmware`) or the pinned public vendor trees |
| FocalTech touch firmware header | `drivers/input/touch/hakios.focaltech_fw.h` | FocalTech (device firmware blob) | vendor's — **to verify** | candidate to move to pull-from-device |
| Apple host headers (build only) | `xnu-deps/hostinc/mach-o/`, `.../mach/` | Apple (open-source cctools/xnu headers) | APSL 2.0 | host-side Mach-O headers for the cross build |
| Pre-built kernel & user space | `prebuilt/` | built from this repo | APSL 2.0 / MIT (as source) | reproducible from source here |
| Project Starbite (the Starbite author) | the fork's `scion/xnu-12377-starlte` (commits `03xx` and the files they add), `xnuloader/sboot/`, `tools/ios27/`, `userland/src/sbca.c` | the author's tree of 8 November 2026, with his agreement | his own files MIT (`LICENSES/MIT-Starbite.txt`); patches to Apple's XNU APSL 2.0; `xnuloader/sboot/` GPL-2.0-only as each file's SPDX line says | per path: `LICENSES.md`. The loader is a program of its own; none is copied into a kernel file |
| ios-sweet (dipa-studio) and ios-a5xelte (orhunnx, A5XELTE contributors) | `the commits of scion/xnu-12377-starlte` `0003`-`0128` | github.com/dipa-studio/ios-sweet, github.com/orhunnx/ios-a5xelte, as ported to xnu-12377 by the Starbite author | MIT for their parts (`LICENSES/MIT-ios-sweet.txt`, `MIT-ios-a5xelte.txt`); APSL 2.0 for the XNU changes | |
| herolte (Galaxy S7) layer of ios-a5xelte (wardevsa) | `the commits of scion/xnu-12377-starlte` `0200`-`0238`, the loader files marked "Based on ios-a5xelte shim/herolte", `tools/ios27/herolte/` | github.com/wardevsa/ios-a5xelte, branch `herolte` | used and published with the author's permission (relayed by the Scion maintainer, 8 November 2026); the repository states no licence of its own; patches to XNU files APSL 2.0 | credit kept in `NOTICE`; if the author states a licence later, that governs |
| HakiOS EAGL bridge (copy, with iOS 26/27 tables) | `tools/ios27/ca-gpu/hakios/` | HakiOS authors | MIT (`LICENSE.hakios` beside it) | Scion's copy is `userland/src/eagl.c`; the follow-up folds them |
| Mesa patches, DRM shim | `tools/ios27/mesa-ios/` | changes to Mesa; a shim after HakiOS's `hakios-drm` | MIT | patches only; Mesa is fetched |
| darwin-xnu-build edits and patches | `xnu/stage/ and the fork's scion/xnu-12377-starlte`, `tools/ios27/dxb-ios.patch` | blacktop | MIT, Copyright (c) 2023-2025 blacktop | the patches are APSL 2.0 as patches to XNU |

## Fetched at build time, never committed

| Component | Fetched by | Origin | License |
|---|---|---|---|
| Darwin userland 16.5 IPSW (user space of the system image) | `sysimage/unpack.sh` | Apple | Apple's — user supplies, user's own copy |
| iPhoneOS 16.5 SDK | `xnu-deps/setup.sh` (github.com/theos/sdks) | Apple (headers) via theos | Apple's / theos |
| Apple open sources (AvailabilityVersions, libdispatch, libpthread, cctools, bootstrap_cmds) | `xnu-deps/setup.sh` | Apple | APSL 2.0 |
| MacOSX13.3 SDK (Kernel.framework DriverKit headers only) | `xnu-deps/setup.sh` | Apple via joseluisq/macosx-sdks | Apple's |
| lzfse | `xnuloader/mkidentity.sh` (when no decoder present) | Apple, github.com/lzfse/lzfse | BSD-3-Clause |
| ath11k (Wi-Fi driver) | `tools/mk-ath11k.sh` | Linux | BSD-3-Clause-Clear |
| brcmfmac and brcmutil (the Galaxy S9's Wi-Fi driver) | `tools/mk-brcmfmac.sh` (the Linux release `tools/mk-ath11k.sh` pins) | Linux (Broadcom) | ISC |
| Mesa (GL=1 images) | `tools/mk-mesa.sh` | Mesa project | MIT and others |
| Mesa's gfxstream guest Vulkan driver (`src/gfxstream`, with the Vulkan runtime it builds on) | `tools/gfxstream/mk-vk.sh` | Google / AOSP via the Mesa project | MIT |
| gfxstream (codegen descriptions; host renderer for the Scion app) | `tools/gfxstream/fetch.sh`, `mk-host-android.sh` | Google | Apache-2.0 (some MIT) |
| metal2vulkan (runtime, translator, AIR reader; METAL=vk images) | `tools/metal/mk-metal2vulkan.sh` (github.com/steelbrain/metal2vulkan at `720ef1ce9ae2`, with AdrenoMetal's patches and ours in `tools/metal/m2v/`) | Anees Iqbal | GPL-2.0-or-later OR LGPL-3.0-or-later; only ever in its own `libmetal2vulkan.dylib`, which can be replaced |
| Rust toolchain and crates (ash, libc, sha2, serde ...) | `tools/metal/mk-metal2vulkan.sh` (rustup into `xnu-deps/rust`, crates.io) | the Rust project, crate authors | MIT / Apache-2.0 |
| Turnip, Mesa's Vulkan driver (TURNIP=1) | `tools/mk-turnip.sh`, in `tools/mk-mesa.sh`'s tree | Mesa project, 25.0.7 | MIT and others |
| hakios-metal (METAL_PLUGIN=ca) | `hakios-metal/` in this tree, `userland/build.sh` | [oosexgang/hakios-metal](https://github.com/oosexgang/hakios-metal) | MIT |
| AdrenoMetal's frontend and bridge (M2V=1) | `tools/mk-m2v.sh`, pinned at `243d63c` | [UniXNU/AdrenoMetal](https://github.com/UniXNU/AdrenoMetal) | MIT, Copyright (c) 2026 NativeMetal contributors |
| metal2vulkan (M2V=1) | `tools/mk-m2v.sh`, pinned at `624bab3`, with AdrenoMetal's 30 patches | [steelbrain/metal2vulkan](https://github.com/steelbrain/metal2vulkan) | `GPL-2.0-or-later OR LGPL-3.0-or-later`, the user's choice (its `LICENSE`) |
| Rust crates of that build (M2V=1) | `cargo`, by `tools/mk-m2v.sh`; `xnu-deps/build/m2v/bridge/Cargo.lock` | crates.io: ash, libc, libloading, sha2 and their dependencies, 34 packages | those read: MIT OR Apache-2.0 (22), MIT (3), ISC (1), Unlicense OR MIT (1); the 7 not found in the local registry (proc-macro2, quote, serde, serde_derive, syn, unicode-ident, windows-link): **to verify** |
| Rust's standard library (M2V=1) | the Rust 1.98 toolchain, target `aarch64-apple-ios` | the Rust project | MIT OR Apache-2.0 |
| QuartzCore's `default.metallib` for the window server's Metal | `tools/ca-metallib.py`, from your own Mac or a later system's IPSW (`CA_QC_METALLIB=`) | Apple | Apple's: user supplies; the result stays on your machine and is never committed |
| Procursus / Sileo packages | `tools/mk-jb.sh` | Procursus team | their own licenses |
| Phone firmware (GPU, Wi-Fi, DSP, modem, haptics, cameras) | `devices/redwood/pull-firmware.sh` | Qualcomm / Xiaomi | vendor's — user's own device |

## A binary built with `M2V=1`: to verify

`userland/build.sh TURNIP=1 M2V=1` links `libadreno_metal_bridge.a`, which holds the metal2vulkan runtime (`GPL-2.0-or-later OR
LGPL-3.0-or-later`), statically into the Metal plug-in (`HakiosMetal`) and into `m2v_test`, beside AdrenoMetal's frontend (MIT), the plug-in's
own code and hakios-metal. Whoever distributes such a binary takes on that license's obligations for it, whichever of the two is chosen: under
the LGPL, among others, a recipient's ability to relink against a changed library; under the GPL, the whole binary's source under compatible
terms. What exactly that requires here, and whether the parts in the row above marked **to verify** allow it, is **to verify**, and has not
been settled. Until it is, no such binary is distributed: nothing of it is in `prebuilt/` (`prebuilt/userland` has no `HakiosMetal`), and you
fetch and build it yourself, on your machine. `libvulkan_freedreno.dylib` (Turnip) is a separate library, loaded at run time, under Mesa's terms.


## Read for reference, not copied

| Component | Read for | Origin | License | How it was used |
|---|---|---|---|---|
| ShengUfs | `drivers/storage/ufs/hakios.ufshc.c/.h` | [UniXNU/ShengUfs](https://github.com/UniXNU/ShengUfs) at `f2d6a4a72f5e7be7d2c9b78eba16cf373868fa30`, Copyright (c) 2026, blu-sharky | BSD-2-Clause (files marked BSD-2-Clause-Patent) | compared with this driver item by item (`hakios.ufshc.h`); the behaviour adopted was written again here, no line of it is in this tree. Its notice, below, goes with it all the same |
| Samsung's Exynos 9810 kernel (the Galaxy S9's) | the S9's UFS facts in `hakios.ufshc.c` | Samsung, Linux | GPL-2.0 | facts only (addresses, register offsets, descriptor layout, quirks), each named with its file there; no code |
| Samsung's S6E3HA8 panel tables for the Galaxy S9+ | `drivers/display/panel/hakios.s6e3ha8_star2.h`: the S9+ panel's dimming tables as data, made by `tools/extract-star2-panel.py` | Samsung, as carried in [ananjaser1211/Apollo](https://github.com/ananjaser1211/Apollo) | GPL-2.0 | data only (numbers of the panel), as the S9's tables in `hakios.s6e3ha8_gamma.h`; no code |
| Samsung's Exynos 9810 camera setfiles (`fimc-is-cis-2l3-setA.h`, `-setB.h`, `fimc-is-cis-3m3-setA.h`) | the Galaxy S9+'s rear sensors' register tables and RAM patches, as two headers made at build time for a `star2lte` kernel (`tools/mk-star2-setfiles.sh`; never committed) | Samsung, as carried in [ananjaser1211/Apollo](https://github.com/ananjaser1211/Apollo) at `30656faf0758` | GPL-2.0 | fetched at build time, data only; not in this repository |
| Samsung's bcmdhd (the S9's Broadcom driver) | the BCM4361's chip and PCI IDs, RAM base and revisions in `wifi/lx/brcmfmac/patches/bcm4361.patch`; the firmware's file names in `devices/starlte/WIFI.md`; in the fork's `bsd/sys/hakios.bcm4361_cis.h` (both kernel lines) the OTP addresses of the module's identity and the table from its two identity bytes to the NVRAM file's name; in the fork's `bsd/sys/hakios.macaddr.h` the text form of the phone's address file | Samsung / Broadcom, the S9's kernel at `e5906667068e` (`drivers/net/wireless/broadcom/bcmdhd_101_12`) | GPL-2.0 | facts only, named in the patch's head and in each header's; no code |
| galaxy-s9plus-mainline-linux | the same, cross-checked | [SheroAbi/galaxy-s9plus-mainline-linux](https://github.com/SheroAbi/galaxy-s9plus-mainline-linux) at `bcee6014fc2e44a4c4f1c34bca0a880d4b543ec3` (`src/ufs/ufs-exynos9810.c`, `src/mainline-dts/exynos9810-star2lte.dts`) | GPL-2.0 | facts only (the protector's descriptor type and PRDT entry size, 4 KB entries, OCS 7 for device errors, LUN 0's layout), named in `hakios.ufshc.c`; no code |

ShengUfs's notice:

```
BSD 2-Clause License

Copyright (c) 2026, blu-sharky
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## How to rebuild the inventory

- Tracked non-text/binary files: `git ls-files -z | xargs -0 file --mime-type | grep -v text/`
- License mentions in tracked files: `git grep -li "SPDX-License-Identifier\|GPL\|BSD\|MIT\|APSL\|copyright"`

If a component's origin is uncertain, it is marked **to verify** above and must be treated as not
yet cleared for redistribution until confirmed with its owner.
