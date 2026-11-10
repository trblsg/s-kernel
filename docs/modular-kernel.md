<!-- SPDX-License-Identifier: MIT-0 -->
# A modular kernel: how this tree maps onto UniXNU's layers

What it would take for this kernel to sit on pristine XNU with an ordered patch series and separate drivers, as
[UniXNU](https://github.com/UniXNU/UniXNU) does. This page describes; nothing here has been converted. Read from their README,
`components.toml`, `feeds.toml`, `docs/porting/index.md` and `UniXNU-sheng/board.toml` on 5 November 2026.

## Layers

UniXNU has six layers: boot image, UEFI firmware, boot manager, XNU loader (DarwinBoot), kernel, user space. Source is split into
shared code (UniXNU and its component repositories), SoC repositories and device repositories.

| UniXNU | Here | Difference |
|---|---|---|
| `feeds.toml`: pinned XNU and other Apple sources, never copied | `xnu/apply.sh` fetches the fork's pinned commit (Apple's tag and our commits on it, HFS Plus among them) | same idea; ours is written in scripts, not data |
| `patches/xnu/series`, per-tree: arch, then SoC, then OS | the commits of the fork's `scion/xnu-8796`: one for the changes to Apple's files, one for the new files, then the numbered patches | ours is one large change plus a numbered set |
| `components.toml`: 26 components, each its own repository, with `hardware`, `os_abi`, `verified_devices`, `build_stage` | `drivers/<class>/conf`, `drivers/map`, built in by `CONFIG_HAKIOS_*` | ours are in one tree, chosen at build; `conf` has no `os_abi` or `verified_devices`. `device.conf` status lines cover the second |
| `build_stage`: kernel, prelink, embedded, payload, firmware | everything linked into the kernel | we have no prelinked drivers and no kits |
| `UniXNU-<soc>-common` | `socs/<soc>/` | the same layer. `socs/exynos9810` holds the numbers; shared Exynos drivers read them through `sys/hakios.exynos_soc.h` |
| `UniXNU-<device>/board.toml`: name, SoC, kit, partition LBAs, defconfig, config fragments, patch series, firmware | `devices/<name>/device.conf`, `configs/`, `mkimage.sh` | ours has status and no firmware recipe; the patch series is shared by all devices |
| `uefi-<device>` | `hakios-uefi`, one for all; `devices/<name>/uefi/` | one tree here against one per device there |
| DarwinBoot, a UEFI driver | `xnuloader/` | the same job. UniXNU's boot manager has no counterpart here: UEFI starts the loader directly |

The kernel layer is where the two differ most. UniXNU pins xnu-10063 and xnu-12377; this tree is on xnu-8796.121.2.

## How much is ours in XNU proper

Changed lines (added plus removed) in Apple's files, counted from the patch form (`xnu/hakios.patch` and `xnu/patches/`, since replaced by the fork's commits) on 5 November 2026:

| Directory | `hakios.patch` | Numbered patches | Sum |
|---|---:|---:|---:|
| `osfmk/arm64` | 391 | 1370 | 1761 |
| `bsd/dev` (mostly `hakios.input.c`, a driver of ours) | 136 | 811 | 947 |
| `osfmk/arm` | 68 | 268 | 336 |
| `bsd/kern` | 333 | 95 | 428 |
| `iokit/Kernel` | 58 | 376 | 434 |
| `bsd/vfs` | 161 | 0 | 161 |
| `osfmk/kern` | 130 | 9 | 139 |
| `pexpert` | 19 | 125 | 144 |
| `bsd/conf`, `osfmk/conf`, `iokit/conf`, `config`, `makedefs` | 212 | 1 | 213 |
| the rest | 165 | 108 | 273 |
| All | 1673 lines, 82 files | 3163 lines, 39 files | 4836 |

The count was made by `tools/xnu-patch-size.py`, which went with the patch form; `git diff --shortstat` of the fork's commit against Apple's (`tools/xnu-tree.py` fetches both) counts the same lines. It gave two numbers: every added or removed line of
the changes to Apple's files and of the numbered patches (the count above), and the lines that change what Apple wrote (a hunk of one of our own files or of a driver was
left out). On 7 November 2026, at each of three points:

| | Patches | Every file | Apple's files |
|---|---|---:|---:|
| main of 7 November | `hakios.patch` and 29 numbered | 4065 | 2582 |
| numbered patches that only change our own files folded into those files | `hakios.patch` and 16 numbered | 2582 | 2582 |
| bring-up logic moved out of Apple's files into files of ours, included or called from them | `hakios.patch` and 12 numbered | 1410 | 1410 |

The second step is a pure refactor: the tree `xnu/apply.sh` lays out is the same. The third moves text (the start of the early-boot assembly, the
slow-core pairing, `systrace=`, the exec and persona code, the file system tables, the extra RAM, the cache-maintenance trap, the ID-register floor,
the two consoles) into new files of the fork and `drivers/core/`: where the text is included at the same place the compiler reads the same
program (the assembler's output is byte-identical; a C object differs in its debug line table, and in the line numbers of assertions after the
moved text), and where it is a function in a file of its own, the object is not the same.

About 4800 lines, so a `patches/xnu-8796/series` would be small. Much of the numbered set's `bsd/dev` part changes our own drivers and
would be folded into them. Beside the patches:

| Kind | Size |
|---|---|
| `drivers/` | 102 files, about 100,000 lines |
| the fork's new files: filesystems (`bsd/miscfs`, 32 files) | 17,300 lines |
| the fork's new files: `iokit/Kernel`, the display and framebuffer classes user space needs | 9,300 lines |
| the fork's new files: `osfmk/minicrypto` | 6,400 lines |
| the fork's new files: `bsd/sys` headers | 3,500 lines |
| the fork's new files: `osfmk/arm64` | 2,700 lines |

Only the first is a clean driver. Whether `bsd/miscfs` and `iokit/Kernel` count as components or kernel changes needs a decision per file.

## What each driver uses of XNU

From a scan of the source for each interface, so this shows what is used, not how deeply. The right-hand column is a guess: none of this has
been built against another XNU.

| Driver | Lines | Uses | On xnu-10063 and xnu-12377 (guess) |
|---|---:|---|---|
| DWC3 and USB glue | 2500 | `ml_io_map`, sysctl, boot args, threads, delays, kalloc | likely holds; `kalloc` forms change between releases |
| UFS | 7500 | `ml_io_map`, cdevsw, devfs, sysctl, boot args, threads, locks, pmap, interrupts, SMC | block device registration likely holds; pmap calls are the risk |
| MFC | 1200 | `ml_io_map`, cdevsw, devfs, boot args, threads, locks, pmap, interrupts, copyin, SMC | likely holds |
| Mali | 2900 | `ml_io_map`, IOKit, sysctl, boot args, threads, locks, pmap, kalloc, copyin, SMC | the IOKit user-client side depends on what the OS expects of a GPU, not only on XNU: unknown |
| DPU | 700 | `ml_io_map` | holds; the risk is in `scion/xnu-8796:iokit/Kernel` above it |
| ABOX | 2200 | `ml_io_map`, boot args, threads, delays, pmap, SMC | likely holds |
| Camera (CSIS) | 2900 | `ml_io_map`, cdevsw, devfs, boot args, threads, locks, pmap, SMC | likely holds |
| Kinibi TEE | 1300 | boot args, threads, pmap, interrupts, kalloc, SMC | likely holds |
| brcmfmac and PCIe | 1800 | `ml_io_map`, boot args, threads, locks, delays, pmap, interrupts | likely holds; the Apple80211 surface above it is OS-dependent: unknown |
| S6SY761 | 500 | `ml_io_map`, boot args, threads, delays | holds |

## The seam

A driver should depend on a small interface, with its XNU touch points in one place, and SoC numbers in `socs/`, board numbers in the device
repository. Today:

- Done: SoC numbers (`sys/hakios.exynos_soc.h`), the log (`sys/hakios.log.h`), and a directory per sizeable driver.
- Not done: most drivers declare what they use of XNU with their own `extern` lines (`ml_io_map`, `delay`, `kernel_thread_start`). One header per
  group of these, then per-driver `*_xnu.c` where the driver has more than a few calls (MFC, UFS, Mali), is the next step.
- Not done: board wiring such as which USI a chip is on still sits in each driver, though the addresses are named in the SoC's header. That data
  belongs in the device repository.

`boot-args` are read directly with `PE_parse_boot_argn`, which has been stable in XNU for years, and need no wrapper.

## Licences

This is a reading of the files, not legal advice, and it needs a person's review before anything is contributed.

| Our files | Licence | With BSD-2-Clause-Patent |
|---|---|---|
| the fork's `scion/xnu-8796`, `drivers/` | APSL 2.0 (`LICENSE`: treated as a derivative of XNU) | can sit beside it in a separate repository or patch set, keeping APSL; cannot be relabelled. UniXNU's own XNU patches face the same question |
| `xnuloader/`, `userland/src/`, `tools/`, docs, `devices/`, `socs/` | MIT | compatible, keeping the notice |
| Upstream HakiOS's part of each | as above, by the same `LICENSE` | Scion cannot relicense what it did not write |
| `wifi/lx/include/` | GPL-2.0 for part of the LinuxKPI and Linux headers (`THIRD_PARTY_LICENSES.md`) | needs review on its own, as it does today |
| `brcmfmac`, `ath11k` | ISC, BSD-3-Clause-Clear, fetched and built, not kept | compatible |

Drivers state that Samsung's GPL kernel was "read for these facts, not copied". Facts are generally not protected, but whether a given table of
register sequences stays on the right side of that is a question for a lawyer. `THIRD_PARTY_LICENSES.md` has 5 rows marked to verify.

## OS profiles

`profiles/<name>.conf` says what a build is for: XNU's tag and commit, the base and its series, the ADT, what user space expects. `OS_PROFILE` picks
one, else the device's (`profile` in `device.conf`), default `ios-16.5`. `ios-26` (xnu-12377.121.6, arm64e, the Pixel 10 Pro's
`devices/blazer`) is `experimental`: its pristine base applies and a build is attempted. [Two XNU bases](two-bases.md) is the design. `ios-27` is
named and fails:

```
$ OS_PROFILE=ios-27 make kernel
os-profile: ios-27 is not-implemented: this tree has no kernel for xnu-12377.121.6 (arm64e). Only ios-16.5 builds; docs/modular-kernel.md has what is missing.
```

The tree, series, machine configuration and SDK now come from the profile (`tools/os-profile.py get src`, [Two XNU bases](two-bases.md)); not yet
`tools/kernel-version.py`'s Darwin version and a few xnu-8796-only tools. No compat layer exists,
because no difference between XNU versions has been established for any driver: it should be added only where one is.

### What iOS 27 needs

UniXNU's `docs/porting/arm64e.md`: iOS and iPadOS 27 are arm64e only, 18 is the last with arm64. An arm64e profile builds XNU with pointer authentication and links the original arm64e kexts. Every enabled
CPU has to implement FEAT_PAuth with address and generic keys; XNU has to write the key registers at EL1; PAC instructions must not trap at EL1 and EL0. Their 24A446 profile pins xnu-12377.121.6, with Darwin 26's
system calls added, and notes that no Darwin 26 XNU source is published yet. Its kernelcache is an `MH_FILESET` with stripped symbols, and their prelink handles that; ours has no prelink at all.

| SoC here | CPUs | FEAT_PAuth | Basis |
|---|---|---|---|
| Exynos 9810 | Exynos M3, Cortex-A55 | no | `exynos9810.config`: the M3s implement ARMv8.0; the A55 is ARMv8.2. So the S9 cannot run iOS 27 |
| SC7280 (SM7325, redwood) | Cortex-A78, A55 | no, by the architecture they implement | guess, from Arm's documents; not read from `ID_AA64ISAR1_EL1` on the phone |
| Tensor G5 (lga, frankel) | Armv9.2 cores | probably | guess: the architecture has it, whether the hypervisor leaves it usable is not known |
| QEMU virt | `-cpu max` | yes | guess: not run here with a PAC guest |

Differences in the driver-facing interfaces between 8796 and 12377, all guesses, as none of this has been built: `kalloc` type-based forms have tightened over releases, which would touch most drivers' allocations; `ml_io_map`
and `PE_parse_boot_argn` are likely unchanged; XNU-side pointer signing would need the function pointers in our `cdevsw` and IOKit tables to be correctly typed, which they mostly are, but the `extern` declarations in drivers
are not checked against the kernel's and would need the interface headers above first; `pmap` calls are the most likely to have moved. The files in `bsd/miscfs` and the IOKit classes among the fork's new files are the least known.

## Order of work

1. One XNU interface header per group of driver `extern`s, and per-driver interface files for the largest drivers. No change to the code that runs.
2. On a branch, generate pristine xnu-8796 plus `patches/xnu-8796/series` and the drivers from main, mechanically. A kernel from it has to match main's
   apart from version and UUID, the same proof used for the driver tree. Done another way (9 November 2026): the kernel is a fork, Scion-Project/xnu,
   with the work as commits on Apple's tags (`docs/xnu-fork.md`); the generated series had fallen behind `xnu/hakios.patch` and is gone.
3. Descriptions beside the manifest (`board.toml`-like, `feeds.toml`-like) where they replace or generate what exists: `devices/manifest.conf`, the Apple
   tag in `apply.sh`. No second source of truth.
4. Components as repositories, in the order DWC3, UFS, MFC, Kinibi: DWC3 is device-neutral already, UFS has per-SoC hooks and the most users, MFC and Kinibi
   are one SoC family's.
5. A newer XNU and arm64e, on a device with PAC. Not started, and not for the S9.

## Not determined

- Whether the IOKit classes in `scion/xnu-8796:iokit/Kernel` and the `bsd/miscfs` files hold on newer XNU. Not tried.
- Whether UniXNU's series could take any of ours as they are. We did not read their patches beyond the directory names.
- How much of `osfmk/arm64` is GICv2 and Exynos support that could be a SoC series, and how much is core. Needs a read of each hunk.
- Whether `drivers/` must be APSL at all: `LICENSE` says so, and only the authors can change that for their own code.
