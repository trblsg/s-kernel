<!-- SPDX-License-Identifier: MIT-0 -->
# One tree, two XNU bases

Scion builds one kernel tree on two of Apple's XNU releases: **xnu-8796.121.2** (iOS 16.5, arm64) for phones whose CPUs lack
pointer authentication, the Galaxy S9 first, and **xnu-12377.121.6** (iOS 26, arm64e) for phones with FEAT_PAuth, the
Pixel 10 Pro first. Drivers, SoC layers and devices are shared; what differs per base is Apple's source, the patch series on it and,
where an interface differs, a shim. This page is the layout, what builds today, and what is left. `docs/modular-kernel.md` is the
background (UniXNU's layers and how this tree maps onto them).

## Layout

| Path | What it holds | Per base? |
|---|---|---|
| `profiles/<os>.conf` | the OS a build is for: XNU tag and commit, `base`, `machine`, `sdk`, `hfs`, `arch`, status, and `builder` (only ios-27 has one: `dxb-ios`, darwin-xnu-build) | one per OS; several may share a base (ios-26 and ios-27 both name `xnu-12377`) |
| `xnu-src/<xnu_tag>/` | the kernel tree being built: the profile's commit of Scion-Project/xnu (`docs/xnu-fork.md`), plus the drivers. Not in git | yes, one per tag, side by side |
| the fork [Scion-Project/xnu](https://github.com/Scion-Project/xnu) | Scion's changes to Apple's files, as commits on Apple's tag, one branch per base (`scion/xnu-8796`, `scion/xnu-12377`, `scion/xnu-12377-starlte`); a profile pins a commit (`docs/xnu-fork.md`) | yes |
| `drivers/<class>/` | the drivers, by subsystem, with `conf` and `map` | shared; a driver says which bases it is built on (below) |
| `socs/<soc>/` | a SoC's numbers and layer (`exynos9810`, `lga` for the Tensor G5, ...) | shared |
| `devices/<name>/` | a phone: `device.conf` (now with `profile`), configs, image recipe | shared; the device names its profile |

The kernel tree itself comes from Scion-Project/xnu, a fork of Apple's repository with Scion's changes as commits on Apple's
tags, one branch per base (`docs/xnu-fork.md`); each profile names the commit a build takes. The series below are what those
branches were made from, and remain until they are removed.

`tools/os-profile.py` is the one place a path or tag per base comes from:

```
$ tools/os-profile.py list
ios-16.5   real             arm64  xnu-8796.121.2
ios-26     experimental     arm64e xnu-12377.121.6
ios-27     not-implemented  arm64e xnu-12377.121.6
$ tools/os-profile.py get src          # xnu-src/xnu-8796.121.2
$ OS_PROFILE=ios-26 tools/os-profile.py get fork_branch   # scion/xnu-12377
```

Which profile: `OS_PROFILE` if set, else the device's (`make NAME_defconfig` writes `.os-profile` from `devices/NAME/device.conf`'s
`profile` line, and removes it for a device without one), else `ios-16.5`. So `make starlte_defconfig && make kernel` is what it
always was, and `make blazer_defconfig && make kernel` is an xnu-12377 build.

`xnu/apply.sh`, `xnu/apply-series.sh`, `xnu/sync-drivers.sh`, `tools/make-kernel.sh`, `xnu-env.sh`, the
`Makefile` and `tools/hakios-config.py` take the tree, fork commit, machine configuration, SDK and HFS step from the profile. For
`ios-16.5` every one of them resolves to the value it had before, which is how the S9's kernel stays the same objects (below).
Tools that only make sense on xnu-8796 (`tools/kernel-delta.sh`, `tools/guard-test.sh`, `tools/soc-compile.sh`, the APFS tools,
`tools/new-workspace.sh`) still name its tree. `tools/kernel-version.py` takes the Darwin release and XNU tag from the profile
(`darwin25.5` and `xnu-12377.121.6` for ios-26; `VERSION`'s `DARWIN` and `XNU` are ios-16.5's).

## Statuses

| Status | Meaning |
|---|---|
| `real` | builds, and a kernel from it has booted on a phone (ios-16.5: the Galaxy S9) |
| `experimental` | the base and its series apply and a kernel links; a build says on stderr that nothing from it has booted (ios-26) |
| `not-implemented` | named so the plan has a place; any build fails with the reason (ios-27) |

## Drivers across bases

A driver is source in `drivers/`, copied into the tree by `drivers/map`, and built when `config/MASTER.hakios` and the
HakiOS block of `bsd/conf/files` say so (`tools/hakios-config.py kernel`). Both series now carry the place for them
(the `bsd/conf/files` and `config/` changes of `scion/xnu-8796` on 8796, the fork's commit `0080-hakios-drivers` on 12377),
and a driver says which bases it is built on with a `bases` line in its `drivers/<class>/conf` (`xnu-8796` when it says none;
a `standard` file takes it too). `tools/hakios-config.py kernel` puts into a tree only the drivers of that tree's base, and says
which it left out; the drivers table (`docs/reference/drivers.md`) marks a driver built for a device only on the base of the
device's profile. The first file on xnu-12377 is `drivers/core/hakios.version.c` (`kern.hakios.version`), with
`commit 0110 of scion/xnu-12377` as its shim; no other driver names `xnu-12377` yet.

What is left, in order, each step on its own:

1. Shims where an interface really differs, and only there (`docs/modular-kernel.md`: none has been shown yet). They go beside
   the interface headers the drivers already use (`sys/hakios.xnu.h` and the per-driver `*_xnu.c`), one directory per base:
   `drivers/core/xnu-12377/`. Expected first: `kalloc`'s typed forms, and pointer signing of the function pointers in
   `cdevsw` and IOKit tables on arm64e.
2. Drivers one at a time on xnu-12377, starting with the device-neutral ones (DWC3, the log, the UFS core), each marked with
   `bases xnu-8796 xnu-12377` in `conf` once it builds there.

## What builds

Built on the Mac mini on 7 November 2026 (Xcode 27.2):

- **ios-16.5, starlte:** built from this branch and from main with the same version suffix; every object compared (below).
- **ios-26, VMAPPLE:** `OS_PROFILE=ios-26 make kernel` fetches the fork's `scion/xnu-12377`, and links an arm64e
  kernel for Apple's virtual machine (`VMAPPLE`, the profile's `machine`), with no device's drivers (below). Linked, not booted.
- **ios-27, STARLTE:** `OS_PROFILE=ios-27 make kernel` on the Mac mini, 9 November 2026, from a fresh clone, `-j4`, with Xcode 16.4 and the
  Kernel Debug Kit 26.5 (25F71): darwin-xnu-build's iOS-target XNU, in about seven minutes, as `hakios-kernel` (Mach-O arm64). Built
  the way the profile's kernel was built before (by hand, in the same directory), it gives the same 1077 objects byte for byte, the same
  92,690 symbol names, and a kernel that is the same in every segment but `__CTF` (the type data: the kernel's own `ctfmerge` makes a
  different one each time, a relink of the same objects as well).

### The S9 stays the same

Both trees were applied from scratch and built with `HAKIOS_VERSION_SUFFIX` fixed, `make starlte_defconfig`, `make kernel`,
`-j4`. The SHA-256 of every `.o` under `BUILD/obj` is compared, and the kernels byte for byte: 993 of 994 objects are the same
bytes; the one that differs is `version.o`, which holds the build's date; the kernels differ in 24 bytes, the 16 of `LC_UUID`
and 8 of the build time in the two copies of the version string. The pull request that brought this page has the hashes.

### The xnu-12377 build

`OS_PROFILE=ios-26 make kernel` (or `make blazer_defconfig && make kernel` with that device's repository) on the Mac mini, Xcode
27.2, 7 November 2026, from a fresh clone: `xnu-deps/setup.sh`, then `xnu/apply-series.sh`, then XNU's build for `VMAPPLE`,
`DEVELOPMENT`, `-j4`, about five minutes. The result is `hakios-kernel`, a Mach-O arm64e kernel ("Darwin Kernel Version 25.5.0",
`kern.osrelease` 25.5.0). What it takes, beyond what xnu-8796 does:

1. **The SDK.** `xnu-deps/setup.sh` makes `xnu-deps/sdk/iPhoneOS26.xnu.sdk` for a profile on this base (its `sdk` is 26): links to
   an iPhoneOS SDK of 26 or later (Xcode's on macOS, 27.2 here; `$THEOS/sdks/iPhoneOS26*.sdk` on Linux, which you bring), plus
   `usr/local`: `availability.pl` from AvailabilityVersions-157.2, the firehose headers of libdispatch-1542.100.32, and stage 2's
   `libfirehose_kernel.a`, compiled for arm64e against the kernel's exported headers. AvailabilityVersions-157.2 is the last
   public release and its version list stops at iOS 17.3 and macOS 14.3: `xnu-deps/av-extend.py` adds the later versions, taken
   (numbers only) from the SDK's own `AvailabilityVersions.h`. There is no pthread library to make: xnu-12377 has it built in.
2. **The version.** XNU 12377 reads its own version from the SDK's `System.kext`, which no public SDK has: `xnu-env.sh` gives it
   the profile's `darwin` (25.5.0) as `RC_DARWIN_KERNEL_VERSION`. It calls `/usr/bin/xcrun` by path: `xnu-build.sh` passes
   `XCRUN=xcrun`, so ours answers. The machine is the profile's (`xnu-build.sh`).
3. **DriverKit's `.iig` files.** On macOS `xnu-deps/host/bin/iig` hands over to Apple's own `iig`, which Xcode has (`xcrun -f iig`):
   it writes the headers and the implementation for xnu-12377's 19 classes, as Apple's build does. On Linux, where there is no
   `iig`, it hands out the headers it would generate, ready-made in the macOS 26.1 SDK (the same XNU release family) that
   `setup.sh` fetches, with empty implementations, as for xnu-8796 (`xnu-deps/gen-iig-stubs.py` for what the linker then
   misses). The Linux path is written, not yet run.
4. **What Apple keeps in its KDK.** Apple compiles the arm64 machine layer (`start.s`, `locore.s`, the caches, `pmap`) into a
   binary library that comes with its Kernel Debug Kit, and includes headers it does not publish. the fork's `scion/xnu-12377`
   (one commit per patch; each message says what it does) compiles that layer from the tree for `VMAPPLE` (`config/MASTER.arm64.vmapple`), links the
   KDK library only if it is there, has the TrustCache headers from xnu-8796.101.5 (its first commit), and stands in, as Scion's
   own code, for what is still missing: CoreEntitlements' V2 types, four entitlement names, SART, the UAT, the CPU tunables, the
   read-only region (`amcc_rorgn_scion.c`: none is locked), `ml_enable_monitor` and `ml_device_is_prod_fused`, the forced
   cache clean, `CPU_OVRD`, and three calls of `pmap.c` into the monitor.

`VMAPPLE` expects Apple's hypervisor for its paravirtualized pointer authentication and read-only region, which QEMU and crosvm
do not offer. `make kernel SCION_NATIVE_PAC=1` builds a kernel that does without them (below); the default build does not change.

### Booting on QEMU (`SCION_NATIVE_PAC=1`)

`OS_PROFILE=ios-26 make kernel SCION_NATIVE_PAC=1` adds what a VM without Apple's hypervisor needs (patches 0090-0095: the PAC keys
in the architectural registers, the chained-fixup rebase in `arm_init`, BTI on `__chkstk_darwin`, no Apple registers, the physical
aperture's slack, a 16550 console) and registers minicrypto (0100). It boots on QEMU `virt` the way crosvm starts a VM on the Pixel
10: avfboot (`avf/shim/`) as the Linux image, the kernel in an XNUP payload (`xnuloader/mkpayload.py --no-version`) as the initrd,
`-M virt,gic-version=3 -cpu max,pauth-qarma3=on` (TCG; QARMA3 is the Pixel's PAC algorithm), one CPU, 2 GB, boot-args
`-v serial=3 debug=0x8 cpus=1 -noprogress sched_preemption_disable_debug_mode=0 interrupt-masked-debug-mode=0
sched_hygiene_debug_pmc=0`. The last three turn off the scheduler-hygiene timeouts, which an emulated CPU does not meet: their
defaults count ticks of a 24 MHz timebase, QEMU's counter runs at 1 GHz (an interrupt handler's 500 us is 12 us there).
`-noprogress`: there is no framebuffer for the boot progress a macOS-style kernel draws.

Where it stops, Mac mini, QEMU 11.1, 8 November 2026 (not run on a phone):

| Kernel | Stop |
|---|---|
| without minicrypto (as Scion-Project/scion-avf#23 left it) | data abort in `SHA1Init`, from `IOPlatformExpertDevice::generatePlatformUUID` in `PE_init_iokit`: no crypto table; the panic then faults again reading its own header, so nothing is printed |
| with minicrypto (0100) | past `PE_init_iokit`, NVRAM, the swap threads and lockdown mode, to `panic: image4 interface not available @kern_trustcache.c:584` (MAC policy start), and the panic log prints in full |

The panic log needs avfboot to report all of RAM in `/chosen/dram-size` for an arm64e kernel: `/pram` lies above XNU's window,
and xnu-12377 maps it as memory, not as a device, only inside `dram-size`; as a device its panic header's unaligned fields
fault.

**The kernel's crypto, and why not Apple's corecrypto.** XNU calls its digests, ciphers and random generator through a table
that Apple's corecrypto kext registers; there is no kext here. Apple publishes corecrypto's source (github.com/apple/corecrypto),
but under its evaluation-only Internal Use License: internal use, "for the sole purpose of verifying the security characteristics
and correct functioning" of it. That rules out building it into Scion's kernel or shipping it, so Scion does not use it. minicrypto, Scion's
own provider from the xnu-8796 series (with BearSSL, MIT, and Monocypher, CC0-1.0, for the key store's parts that this base
does not build), gives MD5, SHA-1, SHA-256/384/512, HMAC, AES-ECB/CBC/CTR (the ARMv8 instructions where there are any) and an
HMAC-DRBG; it self-tests at boot. Not provided: AES-XTS and GCM, DES, ChaCha20-Poly1305, RSA.

**Next stop.** Trust caches need the AMFI and image4 interface tables that their kexts register. The xnu-8796 series has an AMFI
stand-in (`libkern/amfi/miniamfi.c`, 0180); an image4 one would be new and sits in code signing, so it waits for a decision on
how far a stand-in may go. The series
after this is the Tensor G5 platform, written fresh (gaps, below).

## The S9's machine layer on xnu-12377 (the ios-27 profile)

`profiles/ios-27.conf` builds the Galaxy S9 (`STARLTE`) on this base, from the same tree as `ios-26`: the fork's `scion/xnu-12377`, into which
the S9's layer (`scion/xnu-12377-starlte`, one ordered block of commits on Apple's tag) is merged (`docs/xnu-fork.md`, one xnu-12377 tree). The Exynos 9810 has
no pointer authentication and no Apple SoC, so the machine layer is not `VMAPPLE`'s: it comes from Project Starbite and the layers under it, and
`docs/ios27-starlte.md` says whose each is, what runs, and what the follow-up is (folding the drivers into `drivers/`, merging the stand-ins
that both machine layers make with `0020`-`0100`, so that one layer serves both). `OS_PROFILE=ios-27 make kernel` builds it, as an
iOS-target XNU, with blacktop's darwin-xnu-build (the profile's `builder dxb-ios`, `tools/ios27/build-xnu-ios.sh`, in `build/dxb-12377`
rather than `xnu-src/`): it needs Xcode (`DEVELOPER_DIR`) and the Kernel Debug Kit 26.5 (`KDKROOT`), Apple's, which you bring, and
`docs/ios27/IOS27-TARGET.md` says what it does. The profile's other build, `tools/ios27/build-xnu-12377.sh`, is the macOS-target one.

## ios-on-pixel-10: what Scion may take

ios-on-pixel-10 by TheAndersMadsen (private here as `Scion-Project/scion-blazer`) boots iOS 26 on the Pixel 10 Pro. Its author
allows Scion to use **his own code** under Scion's licence (APSL for kernel files, MIT for tools), with credit. Third-party files
in it keep their own terms, and GPL never goes into a kernel file. Read from each file's header and its `docs/LICENSING.md` on
7 November 2026, at f04fd1f; this is a reading, not legal advice.

| Path in scion-blazer | Origin | Licence | Here |
|---|---|---|---|
| XNU 12377 kernel series (`combined/ios26-p11-all-in-one.patch`, against xnu-12377.121.6) | the author's work on top of `ios-tegu`'s port | **not in its git**: tegu's context lines carry no licence | no; rewrite on the pristine base (gaps) |
| `xnu/ios16-base/patches/` | xnu-8796 snapshot: Apple, HakiOS, tegu-derived, local, not split by origin | mixed, partly none | no |
| `dualboot/` (`readback.*`, `xnu_usb_readback.inc`, `android_handoff.*`, tests, `validate*.py`) | author's own | MIT by header | readback and its XNU adapter: `devices/blazer/carried/dualboot/` (not built) |
| `runtime/exit_trace/` | author's own | MIT (`LICENSE`) | `devices/blazer/carried/exit_trace/` (not built) |
| `runtime/ca_sampler/`, `runtime/bt_keychain_trace/` | author's own; user space | MIT (`LICENSE`) | no: user-space interposers, not kernel; candidates for `userland/` later |
| new `blazer_*` kernel files in tracked `research/**/*.patch` with an MIT header and nothing of tegu (`blazer_nfc*`, `blazer_cli10_bus.h`, `blazer_haptic_tap.h`, `blazer_haptic_service.h`, `blazer_mag_hw.*`, `blazer_charger.h`) | author's own | MIT by header | `devices/blazer/carried/` (not built) |
| other new kernel files in `research/` patches with no header (`blazer_usbhost*`, `blazer_memplan.h`, `hk_screen*`, `hk_bright_calib.h`, `blazer_aks_*.inc`, `sweet_touch_event.h`, `hk_physical_keys.inc`) | author's, but written inside tegu-derived classes (`Sweet*`) or with no header | unknown | no; ask, or rewrite |
| changes in `research/` patches to existing files (`SweetFramebuffer.cpp`, `SweetHID.cpp`, `blazer_gauge.c`, `blazer_bcm4390*.c`, ...) | the author's hunks on files that are not in git | the hunks are his; the files they apply to are not available | no |
| `shim/` 11 files `SPDX MIT` (`tgl_all.*`, `tgl_dcs.*`, `tgl_scanout_takeover.h`, `tgl_ufs_handoff.*`, `tgl_ufs_host.*`, `tgl_ufs_route.*`) | author's own; `tgl_ufs_host.c` follows the vendor's UFS order and values, `tgl_ufs_route.c` includes `tgl_ufs_link.h` | MIT by header | no (loader, not kernel); `tgl_ufs_route.c` cannot be taken without the GPL link file |
| `shim/tgl_ufs_link.c`, `.h` | Linux UFS code (Samsung, Linux Foundation, Google lines) | **GPL-2.0-only** | never in a kernel file; facts only |
| `shim/frame_tables.h`, `shim/dcs_tables.h`, `shim/dpu_quiesce.h` | generated from GrapheneOS vendor panel and DPU sources | **treat as GPL-derived** | no; facts only |
| `shim/start.S`, `loader.c`, `payload.h`, `laguna_usb.*`, `stamp.c`, `chooser*.c`, `tgl_usb_console.c`, `wdt.h`, `tgl_fastcore_s.S`, `shim.c` | local, tegu-derived in part (entry sequence, payload format, USB protocol) | **none** | no; design facts only |
| `shim/google-wdt-inspect.h`, `shim/probe.h`, `shim/fdt.*`, `shim/keys_laguna.*`, `shim/tgl_cpm.*` | register facts from Google's kernel source, local code | none stated | no |
| `shim/font.h` | generated from a `cp850-8x16` PSF font | **unknown** | no |
| `native/metal/isa/`, `native/metal/streams/` | author's own | MIT (`LICENSE` per directory); the AIR front end it links is external **LGPL-3.0-or-later** | no; GPU work, not this change |
| `tools/dualboot.py`, `tools/stage_dualboot_readback.py`, `tools/test_dualboot.py`, `tools/android_bootset.py`, `tools/test_android_bootset.py`, `tools/native_gpu_upstream.py` | author's own | MIT by header | no; candidates for `scion-installer` (GPT planner) |
| `tools/mkbootimg.py`, `third_party/aosp_bootimg/` | AOSP | **Apache-2.0** | no |
| `tools/image_tool.py` | local; `USER` boot arguments copied from tegu's build script | none | no |
| `tools/mkadt.py`, the other 80 files of `tools/`, `adt/` (YAML patches over Apple's device tree) | local and porting code, provenance partly recorded | none stated | no; design only |
| `*.md`, `docs/`, `.github/`, `research/` notes | local; some documents follow `ios-tegu`'s structure | none stated | facts only, in our own words |

### Gaps to write again

What the Pixel 10 Pro needs on xnu-12377 and cannot be taken, so is Scion's to write on the pristine base:

1. The platform: Tensor G5 early boot, GIC, PSCI, timers, the device-tree glue (that tree's series, tegu-derived).
2. The display path XNU uses (that tree's `Sweet*` framebuffer and surface classes, tegu-derived), and the boot shim's panel
   bring-up, whose tables are GPL-derived.
3. UFS: the link start-up (`tgl_ufs_link.*`, GPL-2.0-only) and the kernel's UFS owner the readback calls into.
4. Touch and buttons delivery (`SweetHID` hunks), the AKS user-session and key-wrap hooks (`SweetKeyStore` hunks).
5. The hooks the carried files need: `blazer_hw_port.h`, `blazer_cpmq_xnu.h`, the USB console core.

## Branding

User-visible strings in what this change adds say Scion; HakiOS's and ios-on-pixel-10's names stay where they credit the work.
The S9's own strings (`EXTRAVERSION = -hakios`, the banner) are not changed here, because the point of this change is that the S9
kernel stays the same objects; renaming them is its own change.
