# iOS 27 on the Galaxy S9 (starlte): what it would take

> Written 8 November 2026. Nothing here has run on a phone. The facts below were read from an iOS 27 restore image on the build
> machine; nothing of that image is in this repository, and none may be.

## The Galaxy S9 runs it (8 November 2026): the ios-27 profile

The verdict below was written in the morning, from the restore image and Scion's tree alone. Since then iOS 27.0.1 (24A446) user space
has been booted on a real Galaxy S9 to SpringBoard's home screen, by a kernel built from this repository's `ios-27` profile. This
section says what that is and where it sits; the sections after it are the morning's analysis, kept.

**What it is.** `profiles/ios-27.conf`: Apple's `xnu-12377.121.6` (nothing newer is public; iOS 27.0.1's kernel is `xnu-13432.2.10`) with
Darwin 27's system calls (`commit 0120 of scion/xnu-12377`) and a machine layer for the S9 (`STARLTE`: the Exynos 9810, plain arm64, 4 KiB kernel
pages under 16 KiB user pages, no Apple SoC) that is the work of **Project Starbite** and the layers under it (below). iOS 27's user space
is arm64e; the S9's Cortex-A55 and Mongoose M3 have no pointer authentication, so the kernel emulates the instructions that trap (the
register forms of the PAC instructions, `FJCVTZS`, `FCADD`, `LDAPR` and the LSE atomics on the M3: `starlte/0319`, `0342`-`0355`;
`tools/ios27/a64e` has the user-space side), some tens of thousands to a couple of hundred thousand traps a second. The root is the
builder's own, from their own IPSW; nothing of Apple's is in this repository.

**Where the pieces are.** One tree, one branch of the fork:

| Path | What |
|---|---|
| `profiles/ios-27.conf` | machine `STARLTE`, fork branch `scion/xnu-12377`, the S9 layer merged from `scion/xnu-12377-starlte` (`fork_commit`, `fork_stage`, `fork_copy`), `drivers none`, status experimental |
| the fork's `scion/xnu-12377-starlte`, first commits | the order: the edits darwin-xnu-build makes, what the staging scripts made (Apple's HFS and libpthread and BearSSL by commit), the files the old series added (platform code, the Starbite and wardevsa drivers, the IOHID event services, the crypto stand-ins, the Mali and display services), then the patches |
| the fork's `scion/xnu-12377-starlte`, the commits after them | the patches: hunks on Apple's files only (and on the sources the scripts staged). [starlte-porting-notes.md](ios27/starlte-porting-notes.md) has the Starbite author's porting notes |
| `xnu/stage/` | `00_setup_coreentitlements.sh`, the profile's `fork_stage`: CoreEntitlements' V2 headers from the Kernel Debug Kit you bring, which cannot be in git |
| `profiles/ios-27.copy` | the `fork_copy` list: the shared drivers and SoC headers of this repository that the tree takes at build time |
| `xnu/apply-series.sh` | makes the tree: the fork's commit, `fork_stage`, `fork_copy`, and (`drivers none`) no driver from `drivers/` |
| `xnuloader/sboot/` | the loader S-Boot starts (boot menu, OS store, recovery menu, USB console and fastboot subset); Scion's own path boots through Mu-Silicium's UEFI and `xnuloader/`, which this leaves alone |
| `tools/ios27/` | build, packing and test tools (`build-xnu-ios.sh`, which `OS_PROFILE=ios-27 make kernel` runs, with `dxb-common.sh`, `dxb.pin`, `dxb-ios.patch` and `firehose-ios.py`; `build-xnu-12377.sh` and `-ios.sh`, `mkpayload-ios26.sh`, `make-xnu-boot.sh`, the ADT makers, `a64e/`, `gpu/`, `mesa-ios/`, `ca-gpu/`) and wardevsa's `herolte/` packing tools |
| `tools/ios27/metal/` | Starbite's Metal stack (Metal 2 frontend, metal2vulkan, PanVK) rebuilt for iOS 27: build scripts, the root installer, `mtl27test`, `sbmtlc`; `docs/ios27/METAL.md` |
| `tools/ios27/speed/` | the boot-time and trap-cost work's root tools (the cache's PACGA and LDRA rewrite, the daemon list, the Settings patch, the kbd and QuartzCore patches of `docs/ios27/NIGHT.md`, `bench.py`) and `tools/ios27/cmdline/starlte-fast.txt`, its command line; `docs/ios27/SPEED.md` |
| `tools/ios27/keystore/` | `dptest`, the on-phone test of the key store's answers, protection classes and the keychain, with its launchd jobs and installer; `docs/ios27/KEYSTORE.md` |
| `tools/ios27/lse/` | the shared cache's LSE atomics rewritten into LL/SC thunks for the M3s (`lse-rewrite.py`, the root scripts, the differential test); `docs/ios27/EIGHT-CORES.md` |
| `userland/src/sbca.c` | CoreAnimation's renderer through OpenGL ES on the Mali-G72 (Mesa Panfrost behind HakiOS's EAGL bridge), with iOS 27.0.1's addresses; the bridge's iOS 27 table is `tools/ios27/ca-gpu/hakios/` |
| `docs/ios27/` | `IOS27-TARGET.md` (the iPhoneOS-target build and what it changes), `PARITY.md` (against Scion's 16.5 kernel), `EIGHT-CORES.md` (the eight cores: the LSE rewrite, the soft pins), `SPEED.md` (boot time and trap cost: what was measured, `sweetspeed`, the root edits), `OPTIMISE.md` (the sampling profiler's cost table and the changes made against it: FP16 forms, recommended cores, Panfrost's batch renaming, the DRM stand-in's answers, the surface hash; `tools/ios27/perf/`), `NIGHT.md` (what killed the daemons at boot, the 10 Hz, async GPU submits, the panel's brightness, Metal in CoreAnimation tried), `GPU-PATH.md` (what the GPU track found), `METAL.md` (Metal on the Mali), and the author's pages on the loader's USB console and fastboot, the microSD root and the recovery menu |

**Layers, and whose they are** (`LICENSES.md` has the licences): ios-sweet (dipa-studio, MIT) `0003`-`0081`; ios-a5xelte (orhunnx and A5XELTE
contributors, MIT) `0082`-`0128`; the Galaxy S7 layer of ios-a5xelte (wardevsa) `0200`-`0238`, used and published with the author's
permission; the Starbite author's STARLTE port `0300`-`0398`; Scion's `0400` and up, and `0120`. The Starbite author's name is a placeholder
("Starbite author") until he gives the one he wants credited.

**Status.** It runs, slowly at first (CPU-composited, 1.5 GiB of RAM visible, Settings usable); with the GPU path of `sbca` (the
`EDRClient` fix took the phone from about 0.1 to 10-50 frames a second, as the GPU track measured it) it is usable; frame pacing is in progress. 8-core mode is accepted as
the default (below) and was built but not booted when this was written. The parity patches `0420`-`0424` (extra RAM, the CPU governor,
Wi-Fi stage 1, TMU, DPU scan-out) are behind boot-arguments, off by default and not booted. `OS_PROFILE=ios-27 make kernel` builds this profile as an iOS-target XNU with darwin-xnu-build
(`docs/ios27/IOS27-TARGET.md`; it needs Xcode and the Kernel Debug Kit 26.5 you bring: `DEVELOPER_DIR`, `KDKROOT`), and
`tools/ios27/build-xnu-12377.sh` builds the macOS-target one. What was run on the phone is in each commit message; the
repository's own checks (`docs/checks.md`) boot nothing.

**Default boot arguments.** The kernel boots with `cpus=8` (four A55 and four M3: `starlte/0304` brings the M3s up, `0342`-`0344` emulate
the LSE and LDAPR instructions they lack for EL0, and pin a thread that uses another one to an A55), `usb_gadget=5` (the log on the USB-C
console: `/dev/cu.usbmodem*`), `rd=ufsroot0 ufsroot=USERDATA ufsrw=1 rootrw=1`, `sweetacl=1`, `sweetfb_native=1 sweetfb_scale=1`,
`sweetfb_gpu=1`, `sweetinject=/usr/lib/sbca.dylib,backboardd,Preferences`, `vm_compression_limit=196608`, `wdt=-1 -wdt_no_pmu`, and this kernel's
own code-signing policy for the builder's own root (`cs_enforcement_disable=1 cs_integrity_skip=1 launchd_unsecure_cache=1`: its AMFI is a stand-in,
`starlte/0025`, `0035`, `0036`, `0322`). `startop=1` adds trap statistics; `sweetm3unpin=N` (default off) lets a thread pinned to an A55 go back to the M3s after N ms (`docs/ios27/EIGHT-CORES.md`). The set is `tools/ios27/cmdline/starlte-8core.txt` (pass it to `tools/ios27/mkpayload-ios26.sh NAME FILE`; the payload
holds 607 bytes of it); the set that booted on the phone at 4 cores is that with `cpus=4`. `docs/boot-args.md` lists what the 16.5 kernel and
user space read; the S9's own are in the sources of the fork's `scion/xnu-12377-starlte`.

**The follow-up (not done, and why).** The branch carries its own drivers: `drivers none` in `profiles/ios-27.conf` tells `xnu/apply-series.sh`
to take none from `drivers/`. What is left to fold into this repository's modular layout, each step with the applied tree checked file for
file as the last two were (the branch's files and the `copy` lines):

1. The adapted Scion drivers that the branch still holds. Done (9 November 2026, `docs/xnu-fork.md`): `hakios.dwc3.c` (its
   xnu-12377 include is behind `__has_include` in `drivers/usb`), `hakios.exynos_tmu.h` and `hakios.scion_gov.c` come from `drivers/` at
   build time (`profiles/ios-27.copy`) and the fork keeps no copy. Left: `hakios.exynos_cpufreq.c`, `hakios.exynos_tmu.c`, `hakios.exynos_pcie.c`, `hakios.exynos_dpu.c`,
   `hakios.exynos_acpm_dvfs.c`, `hakios.exynos9810_mem.h`, `hakios.exynos_soc.h` become `drivers/` files with `bases xnu-8796 xnu-12377` in their `conf`
   (the differences go into `drivers/core/xnu-12377/` shims or the drivers themselves).
2. The Starbite and wardevsa platform and device code (`sweet_*.c`, `a5xelte_*.c`, `herolte_*.c`, `starlte_*.c`, the Mali user client, the framebuffer and
   surface services, the touch drivers) renamed to Scion's conventions (`hakios.<thing>.c`), grouped by subsystem under `drivers/` with `conf` blocks, with what
   Scion already has (UFS, touch, the DPU, the S9's MUIC and battery) taken from there, and the boards this tree does not use (SWEET, A5XELTE, HEROLTE) dropped
   once STARLTE no longer needs their patches (its board layer is built on them today).
3. The stand-ins the series makes twice (`sart.h`, `uat.h`, `tunables.s`, the read-only region, the crypto providers, AMFI) merged with `0020`-`0100` of the ios-26
   series, so one machine layer serves `VMAPPLE` and `STARLTE`; then the profile could be built by `xnu-build.sh` in `xnu-src/` like the others
   (`make kernel` builds it today, through darwin-xnu-build in `build/dxb-12377`).
4. The loader: `xnuloader/sboot` only where `xnuloader/` has nothing equivalent (S-Boot hand-off, the menu); the USB console is the DWC3 gadget's.
5. `userland/src/eagl.c` takes the iOS 26 and 27 function tables of `tools/ios27/ca-gpu/hakios/eagl.c`, and `tools/mesa-ios` is folded into `tools/mesa`.

## Verdict

**Not bootable from Scion today, and no test image can be made yet.** Three things stand between the S9 and iOS 27, each on
its own:

1. **The kernel.** Apple has published no XNU source for iOS 27. The newest tag in apple-oss-distributions/xnu is
   `xnu-12377.121.6` (iOS 26 / macOS 26.5), the base of the `ios-26` profile; iOS 27.0.1's own kernel is
   `xnu-13432.2.10` (Darwin 27.0.0). So iOS 27 here means iOS 27's user space on xnu-12377 with Darwin 27's additions put
   back. That base builds today only for Apple's virtual machine (`VMAPPLE`, `docs/two-bases.md`); none of the S9's drivers,
   and none of the Exynos 9810 machine layer, is on it yet (`drivers/*/conf`: only `hakios.version.c` names `xnu-12377`).
2. **The CPUs.** From iOS 17 on, every iPhone user-space binary is arm64e only. The S9's Cortex-A55 (ARMv8.2) and Mongoose
   M3 (ARMv8.0) cores have no pointer authentication: the hint-space forms (`PACIASP`, `AUTIASP`, `XPACLRI` ...) run as
   NOPs, but `RETAA`/`RETAB`, `BRAA`/`BLRAA` and friends, `LDRAA`/`LDRAB`, the register forms of `PAC*`/`AUT*` and `PACGA`
   are UNDEFINED and trap. A sample of iOS 27 code (the arm64e Mach-O files of the restore ramdisk) has them in the hundreds
   of thousands, beside v8.3 `FJCVTZS`/`FCADD` and `LDAPR` (RCpc, which the M3 lacks too), and LSE atomics, which the M3
   lacks. Scion has no answer for that user space yet: any answer changes Apple's code inside the builder's own root image,
   which is a separate decision from anything in this repository.
3. **The builder's inputs.** The root comes from the builder's own IPSW. Its system and system-cryptex disk images are
   AEA-encrypted; decrypting them, and building a root file system from them (which needs root on the Mac), is the builder's
   own step.

## What iOS 27 asks of the kernel (measured)

The iOS 27.0.1 kernelcache for the iPhone 11 (`RELEASE_ARM64_T8030`) was read with `ipsw` and compared with
`xnu-12377.121.6`:

- **System calls.** Both tables end at 557. Darwin 27 fills eight numbers that are `nosys` in xnu-12377: `pipe2` (148),
  `dup3` (149), `fchflagsat` (214), `getumask` (215), `aio_readv` (246), `aio_writev` (247), `fsctlat` (249) and
  `guarded_ftruncate_np` (352). iOS 27's `libsystem_kernel` has a wrapper for each; `pipe2` returns its two descriptors the
  way `pipe` does (in the two return registers, which the wrapper stores). No number 12377 implements was taken away
  (the entries that are absent from the iOS kernel are compiled out on iOS in 12377 too: System V IPC, audit, NFS).
- **Mach interfaces (MIG).** The 17 subsystems the iOS 27 kernel serves all exist in 12377's `.defs`, at the same base
  numbers, and none has a routine number above 12377's range. Changes in a routine's arguments within a range are not
  checked here.
- **Kernel extensions.** The kernelcache links 260; Scion uses none of Apple's. What iOS 27's daemons ask of AMFI,
  AppleImage4, IOHIDFamily, IOSurface and the GPU family, beyond what iOS 26 asked, is only learned by booting.

`commit 0120 of scion/xnu-12377` adds the eight system calls to the xnu-12377 base, as Scion's own code
(APSL, like the files it changes): `pipe2` (O_CLOEXEC, O_CLOFORK, O_NONBLOCK), `dup3` (O_CLOEXEC, O_CLOFORK; the same
descriptor twice is EINVAL), `fchflagsat` and `fsctlat` (relative to a directory descriptor), `getumask`, and
`guarded_ftruncate_np` (`ftruncate` checked against the descriptor's guard, as `guarded_pwrite_np` is). `aio_readv` and
`aio_writev` answer ENOTSUP, so a Darwin 27 library gets an error rather than SIGSYS. UniXNU carries the same eight calls
(its `darwin26-syscalls.patch`); its code is not used here.

## Order of work

1. The Exynos 9810 machine layer on xnu-12377: a board configuration beside `VMAPPLE`, plain arm64 (no PAC keys to manage),
   4 KiB kernel pages under 16 KiB user pages as the 8796 S9 kernel has them.
2. The S9's drivers on xnu-12377, one at a time, each with `bases xnu-8796 xnu-12377` once it builds (UFS, the log, DWC3
   first).
3. A user space the S9's cores can run (point 2 above): a decision for the owner first.
4. The AMFI and image4 stand-ins that `docs/two-bases.md` names as the next stop on QEMU.

The S9's iOS 16.5 kernel is unaffected by all of this: 0120 is in the xnu-12377 series only.
