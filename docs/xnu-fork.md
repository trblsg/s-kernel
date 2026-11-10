<!-- SPDX-License-Identifier: MIT-0 -->
# The kernel is a fork: Scion-Project/xnu

Scion's kernel source is [Scion-Project/xnu](https://github.com/Scion-Project/xnu), a fork of Apple's
`apple-oss-distributions/xnu`. Scion's changes are ordinary commits on Apple's release tags, one branch per base, instead of a
patch series that a build applies to a fresh copy of Apple's tree. This repository keeps what is not the kernel tree (devices,
SoC layers, the driver configuration, the loader, user space, tools, docs) and names, per OS profile, the commit of the fork a
build takes.

| Profile | Branch | On Apple's tag | What the branch's commits are |
|---|---|---|---|
| `ios-16.5` | `scion/xnu-8796` | `xnu-8796.121.2` | Scion's changes to Apple's files (one commit), Scion's new files (one commit), HFS Plus (Apple's `hfs-627.100.6` files, then the port's changes to them), the numbered patches `1002` to `1517`: one commit each, in the order the build used to apply them |
| `ios-26` and `ios-27` | `scion/xnu-12377` | `xnu-12377.121.6` | one tree for both machines (below): the TrustCache headers of `xnu-8796.101.5`, then the patches `0010` to `0120` (VMAPPLE's layer), one commit each; merged with `scion/xnu-12377-starlte` |
| (history of ios-27) | `scion/xnu-12377-starlte` | `xnu-12377.121.6` | the S9's layer, frozen at `56c907c4d4c3` and merged into `scion/xnu-12377`: one commit per step of the old series (what darwin-xnu-build's edits and the staging scripts make: Apple's libpthread and HFS and BearSSL, by commit; the files the series added; the `copy` lines' files; every patch), then the commits that took the shared drivers out and dropped SWEET's empty drivers. Its commits are the ones the ios-27 docs name (`commit 0429 of scion/xnu-12377-starlte`) |

Each commit's message carries the patch's own description, the file of this repository it came from (and the commit of this
repository it was taken at), and for the ios-27 branch the layer it belongs to (ios-sweet, ios-a5xelte, wardevsa's Galaxy S7
layer, the Starbite author's STARLTE port, Scion's), as `LICENSES.md` and `NOTICE` record them. The licences are the files' own:
the fork is APSL-2.0 as Apple's XNU is; files of other authors keep theirs.

### One xnu-12377 tree for VMAPPLE and STARLTE

`scion/xnu-12377` at `296d251e68ea` is both machines' tree: the merge of the S9's layer into VMAPPLE's. Each board compiles
exactly what it compiled on its own branch: in the 99 source files either side changed, the generic ARMv8 boards (those that
define `ARM64_BOARD_CONFIG_A5XELTE` or `ARM64_BOARD_CONFIG_SWEET`: STARLTE and the boards it is built on) see the S9 layer's text
and every other board VMAPPLE's, through `#if` pairs (identical changes, the Darwin 27 system calls of `0120`, are there once); the
file lists name each side's files `optional scion_generic_arm64` or `optional scion_vmapple`, keywords of the machines' MASTER
files. The stand-ins both layers made at the same path are one file now: `osfmk/arm64/ppl/sart.h`, `uat.h` and
`osfmk/arm64/tunables/tunables.s`; the others (the read-only region `amcc_rorgn_*`, `ml_enable_monitor`, the crypto providers
minicrypto and BearSSL, AMFI) differ in what they do and stay per board. VMAPPLE's TrustCache, CodeSignature and CoreEntitlements V2
headers moved to `EXTERNAL_HEADERS/scion-vmapple/` (on the include path of the other boards only), so that STARLTE keeps the Kernel
Debug Kit's. Both kernels were built before and after the merge, each at one path: the code is the same but for the line numbers
compiled into it (`__LINE__` of asserts and panics, as `mov wN, #line` immediates, and the names of a few symbols the startup and
`kalloc_type` macros make from them): 789 such instructions in VMAPPLE's, 646 in STARLTE's, every symbol at the same address.

## How a build takes it

`profiles/<os>.conf` names the fork (`xnu_fork`), the branch (`fork_branch`) and the commit (`fork_commit`). `xnu/apply.sh` (and
`xnu/apply-series.sh`, which it calls for an xnu-12377 profile) then fetches that one commit (`git fetch --depth 1`), checks it out on
the branch `hakios`, and adds what it always added after the series: the drivers of
`drivers/map` and of the SoC and device repositories checked out, and the drivers' configuration (`tools/hakios-config.py
kernel`). `make kernel` is unchanged for the user: `make starlte_defconfig && make kernel` builds the S9's iOS 16.5 kernel,
`OS_PROFILE=ios-26 make kernel` the VMAPPLE kernel, and `OS_PROFILE=ios-27 make kernel` the iOS 27 S9 kernel (an iOS-target XNU, built by
blacktop's darwin-xnu-build in `build/dxb-12377`, with the Xcode and the Kernel Debug Kit you bring: `DEVELOPER_DIR`, `KDKROOT`;
`docs/ios27/IOS27-TARGET.md`), each from the fork.

What may not be in git is still staged at build time, by the scripts the profile names in `fork_stage`: for ios-27 that is
CoreEntitlements' V2 headers, which come from the Kernel Debug Kit you bring (`xnu/stage/00_setup_coreentitlements.sh`,
`KDKROOT`). The libpthread and HFS staging copies the SDK's `TargetConditionals.h` (Apple's, APSL-2.0 by its header) into the
tree; on the branch it is the copy from Xcode 27.2's macOS SDK, the one the iOS 27 kernel was built with.

Scion's shared drivers stay in this repository, as for the other profiles (`drivers/map`): for ios-27 the files that
`series.ios27`'s `copy` lines named are copied into the tree at build time from `drivers/` and `socs/` (`profiles/ios-27.copy`, the
profile's `fork_copy`; the profile's `drivers none` keeps `xnu/apply-series.sh` from adding any other), and the branch does not keep a second copy; `hakios.s6e3ha8.c` is the exception until its ios-27 change
(patch 0428) is folded into `drivers/display/panel`.

`XNU_FORK_URL` fetches the fork from elsewhere (a local clone that has the commit). A profile without a `fork_commit` does not build.

## Changing the kernel

Commit in the fork, on the profile's branch (or a branch from it, with a pull request there), push, and set the profile's
`fork_commit` to the new commit in a pull request here. A tree made by `xnu/apply.sh` is already a clone of the fork at that
commit: work in it, commit, and push from it. Apple's next release is a merge: `git merge xnu-NEW-TAG` on the branch.

## Proof that nothing changed

When the fork was made (9 November 2026, from this repository at `706528182d10`), the trees were made both ways on the Mac mini
and compared with `diff -r` (content) and the list of executable files: the same, for all three profiles (ios-16.5 with the
S9's configuration and every device repository checked out; ios-26; ios-27 with the Kernel Debug Kit 26.5). The kernels built
from them are compared in the pull request that brought this page.

## The patch forms are gone

Until the fork, the kernel was kept here as patches: `xnu/hakios.patch`, `xnu/files/`, `xnu/patches/` and `xnu/hfs/` for ios-16.5,
`patches/xnu-12377/` (the series, `series.ios27`, `starlte/`) and `xnu/files-12377/` for the xnu-12377 profiles, with `xnu/export.sh`
to write the tree back into them. They were copies of what the fork holds, and are removed (a fork commit's message names the
file it came from). `SCION_XNU_FROM_PATCHES` and `xnu/export.sh` went with them. What stayed in this repository is what the fork
does not hold: the shared drivers (`drivers/`, `socs/`, `devices/`), `xnu/stage/00_setup_coreentitlements.sh` (it copies headers out
of the Kernel Debug Kit you bring) and the licence texts that `NOTICE` and `LICENSES.md` name (`LICENSES/`).

Tools that read the kernel's sources (the boot-argument catalogue, the host tests of the kernel's headers, the APFS tools, the loader's
build) get them from the fork with `tools/xnu-tree.py`:

| | |
|---|---|
| `tools/xnu-tree.py path [PROFILE]` | a cached, pristine checkout of the profile's `fork_commit` (no drivers, no configuration) in `xnu-src/fork-<12 characters of the commit>/`; fetched the first time (`git fetch --depth 1`, so with the network), reused while its `HEAD` is the commit |
| `tools/xnu-tree.py added [PROFILE]` | a directory of only the files the fork adds to Apple's XNU, at their paths in the tree: what `-I` wants (the checkout itself has Apple's `<sys/types.h>`) |
| `tools/xnu-tree.py changed [PROFILE]` | the files the fork's commit adds or changes relative to Apple's commit (`xnu_commit`); `--diff-filter=A` for the added ones |
| `tools/xnu-tree.py diff [PROFILE] [FILE...]` | `git diff -U0` of the two |
| `tools/xnu-tree.py log [PROFILE]` | the fork's commits on Apple's, oldest first (the branch's history is fetched for this one) |

`PROFILE` is `OS_PROFILE`, else the active profile. `XNU_FORK_URL` and `XNU_URL` (Apple's repository, for Apple's commit if the fork
lacks it) apply as for `xnu/apply.sh`. `xnu-src/` is not in git. CI steps that need the sources fetch them from GitHub the same way.

## What is next

1. The drivers into the fork's tree, one source per hardware block with `bases` (`docs/two-bases.md`), deduplicated against the
   copies the ios-27 branch carries. `hakios.dwc3.c`, `hakios.exynos_tmu.h`, `hakios.scion_gov.c` and the files of the old `copy`
   lines are one source already (`profiles/ios-27.copy`); left: `hakios.exynos_cpufreq.c`, `hakios.exynos_tmu.c`, `hakios.exynos_dpu.c`,
   `hakios.exynos_pcie.c` (they differ in behaviour, so merging them changes one kernel or the other), `hakios.s6e3ha8.c` (patch 0428),
   `hakios.exynos_acpm_dvfs.c` (the series' own) and the SoC headers `hakios.exynos_soc.h` and `hakios.exynos9810_mem.h`.
2. One xnu-12377 branch: the stand-ins both machine layers make (SART, UAT, tunables, the read-only region, crypto, AMFI) merged,
   so that `scion/xnu-12377` builds VMAPPLE and STARLTE.
3. The Starbite and wardevsa files renamed to Scion's conventions, and what STARLTE does not use dropped, as commits on the fork.
   STARLTE is built on the A5XELTE and HEROLTE boards' code (`makedefs/MakeInc.def` defines all three for it), so only SWEET is
   unused: its six drivers that compiled to empty objects are gone (`sweet_ufs.c`, `sweet_keys.c`, `sweet_touch.c`, `sweet_rpmh.c`,
   `sweet_spmi.c`, `sweet_usb.c`); its blocks in Apple's files, `MASTER.arm64.sweet` and `SWEET.h` are left.
