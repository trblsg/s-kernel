# iOS 27 on the Galaxy S9: the key store and data protection (9 November 2026)

What iOS 27.0.1 (24A446) asks of AppleKeyStore and of the data volume's protection classes, what this kernel answers, and what it does
not do. The kernel side is two files of the fork's ios-27 layer: `iokit/Kernel/arm/SweetKeyStore.cpp` (the stand-in for Apple's
AppleKeyStore) and `bsd/sweet_hfs/sweet_hfs_pclass.c` (protection classes on the built-in HFS Plus). The changes of this page are behind bits
of the boot-argument `sweetspeed=<mask>` and are **off by default**:

| Bit | Change | Where |
|---|---|---|
| `0x100000` (1048576) | the built-in HFS Plus keeps, reports and inherits data-protection classes, as labels; what has no class reads back as D | `bsd/sweet_hfs/sweet_hfs_pclass.c`, hooks in `core/hfs_vnops.c`, `hfs_attrlist.c`, `hfs_readwrite.c` |
| `0x200000` (2097152) | with the bit above: the mount says `MNT_CPROTECT` (what `MKBDeviceFormattedForContentProtection()` and SQLite read), and an update mount keeps it | `core/hfs_vfsutils.c`, `bsd/vfs/vfs_syscalls.c` |
| `0x400000` (4194304) | the key store answers as a device with **no passcode** does, in full (below) | `iokit/Kernel/arm/SweetKeyStore.cpp` |
| `0x1000000` (16777216) | APFS's purgeable-file and directory-statistics `fsctl()`s are accepted (and forgotten) by the built-in HFS Plus | `core/hfs_readwrite.c` |
| `0x2000000` (33554432) | sandbox extensions are issued and consumed by the sandbox stand-in: tokens that grant nothing, there being no sandbox | `bsd/kern/sweet_amfi.c` |
| `0x4000000` (67108864) | a passcode that gates the screen, answered to Apple's keybagd (the fork's `wip/ios27-keystore-passcode`, not in `wip/ios27-keystore`) | `iokit/Kernel/arm/SweetKeyStorePasscode.cpp` |

**In short: nothing here encrypts anything.** There is no Secure Enclave, no passcode, no key that lives outside the kernel. A protection
class is a label that programs can set and read back; a file marked A is as readable from the flash, and while the screen is locked, as one
marked D. The keychain's "wrapped" keys are the keys behind an 8-byte tag (see the head of `SweetKeyStore.cpp`). Scion's iOS 16.5 kernel has a
real key bag for the keychain (`docs/keystore.md`, `docs/passcode.md`); it is not on this tree yet, and the last section says what bringing it
over takes.

## What was wrong

Found on a Galaxy S9 running 24A446 (the kernel's own log of key store calls, and the programs' logs), then read from the binaries:

- **The lock state said "a passcode is set, and the device is unlocked".** MobileKeyBag's `MKBGetDeviceLockState` returns the `sls` value of
  the device state (selector 17 or 35); ManagedConfiguration's "is a passcode set" is `MKBGetDeviceLockState() != 3`. The stand-in answered
  `sls` 0 and state flags 4 (unlocked since boot). A device with no passcode answers `sls` 3 ("disabled") and flags 6 (no passcode, unlocked
  since boot). With 0, SpringBoard asked the key store to lock at every screen-off (selectors 8 and 32, which failed, 199 and 38 times in one
  day's log), and Settings and `profiled` took the passcode as set.
- **The configuration was unanswered.** `profiled` asked for it (selector 31, `MKBGetDeviceConfigurations`) 267 times in the same log, the
  passcode settings 16 times, each answered "unsupported".
- **Files had no protection class.** `fcntl(F_GETPROTECTIONCLASS)` on the HFS Plus root answered `ENOTSUP`, and setting a class was accepted
  and forgotten. InstallCoordination's `-[IXGlobalConfiguration createDirectories]` makes its data directory with class D
  (`-[IXFileManager createDirectoryAtURL:withIntermediateDirectories:mode:class:error:]`: `open`, `fcntl(F_GETPROTECTIONCLASS)`,
  `fcntl(F_SETPROTECTIONCLASS)`), and on any error logs "Failed to create data directory" and ends the process with `os_crash`: so
  `installcoordinationd` could not start. PosterBoard calls its data store unusable when a directory it has just marked
  `NSFileProtectionNone` does not read back as None (`NSURLFileProtectionKey`, through `getattrlist(ATTR_CMN_DATA_PROTECT_FLAGS)`).

The selector numbers were not the problem: every `aks_*` function of 24A446 still calls the number it called on iOS 16 (table below); iOS 27
only adds to the list.

**What is verified, and how.** One Galaxy S9, the fork's commit 2adf535 (iOS-target STARLTE kernel), `sweetspeed` bits `0x100000` and `0x400000`, and the test of the last section (`dptest`, as root and as `mobile`), one boot: classes set by `fcntl`, `setattrlist`, `open_dprotected_np` and `NSFileManager` read back by `fcntl`, `getattrlist` and `NSURL`, also after reopening; a file made in a directory of class C came out C; what had no class read D. `MKBGetDeviceLockState` 3, `MKBDeviceUnlockedSinceBoot` 1, `MKBGetDeviceConfigurations` a dictionary (grace period 0, 10 attempts); selectors 7, 14, 17, 23, 31, 35, 40, 103, 107 as the table below says. A keychain item added, read back and deleted for each of "after first unlock", "when unlocked" and "always, this device only". `installcoordinationd` logged "Created .../InstallCoordination" and ".../Removability" and stayed up (it ended in `os_crash` before). The boot before that one found a panic: the configuration is asked for with a 32 KB buffer, which arrives as a memory descriptor, and the stand-in wrote to it unprepared ("IOGMD: not wired"); fixed in 2adf535. **`0x1000000` (the purgeable calls) and `0x2000000` (sandbox extensions):** on the second S9, one boot of ten minutes on the full root with all four bits (and the GPU side's memory cap): PosterBoard's log had none of the 111 purgeable errors and none of the 16 sandbox-extension errors of the boots before. Before that, a boot with the purgeable calls on an older GPU line left the USB bus at 66 s with nothing on the console (no panic log); it did not come back with the newer kernel, and what it was is not known. **The wallpaper picker is not there yet:** Settings' Wallpaper pane draws and "Add New Wallpaper" opens its sheet, which stays on a spinner; entering the pane, PosterBoard aborted twice right after `-[PRSServer fetchPosterSnapshotsWithRequest:]`, "CoreVideo: Failed to read HardwarePlatform, MG query returned NULL" and CMPhoto starting: the poster snapshot path, which is beyond this page. Settings' Passcode pane shows the state of a device without one ("Turn Passcode On"). **Not run on a phone yet:** `0x200000` (`MNT_CPROTECT`).

## What the key store answers

`AppleKeyStore` is published under `IOResources` with the user client `AppleKeyStoreUserClient`, type 0; libaks opens it and calls selector 0
once. Scalars in and out are 64 bit; passcodes and their lengths travel as scalars (an address in the caller and a length), larger answers in the
output structure. The device state and the configuration are DER: a SET of SEQUENCE { UTF8String key, INTEGER value }.

| Selector | With `sweetspeed&0x400000` | Without it (as before) |
|---|---|---|
| 0 open, 4 unload bag, 5 set system, 9 unlock device, 12 unlock bag, 13 lock bag, 26/27 assertion hold/drop, 55 unload session bags, 101 assertion consume | success, nothing to do | the same |
| 7 lock state | flags 6: no passcode, unlocked since boot | flags 4: unlocked since boot |
| 8, 32 lock device | success, nothing to lock | unsupported |
| 10, 11 wrap, unwrap key | the key behind an 8-byte tag: **not protected at rest** | the same |
| 14 system bag | handle 0 (keybagd then loads no bag file) | the same |
| 17, 35 device state | `ss` 6, `sls` 3 (disabled), `sb` `sfa` `sgs` `sas` `sgpe` `srcd` 0 | `ss` 4, `sls` 0 |
| 23 bag UUID | 16 fixed bytes | unsupported |
| 30 set configuration | the grace period `gp` is kept for this boot (it applies to nothing) | unsupported |
| 31 configuration | `bd` 0, `gp`, `mua` 10 | unsupported |
| 40 generation | 0 | unsupported |
| 42 verify password | success: there is nothing to compare with | unsupported |
| 100 assertion promote, 115 prewarm, 160 enable cache flow | success | unsupported |
| 102, 103 "the first-unlock notification was posted" | remembered | accepted, and "not posted" for ever |
| 107 "SE support in restore mode is set" | 0 | unsupported |
| `IOConnectSetNotificationPort` (`aks_register_for_notifications`) | success; nothing is ever sent, the state cannot change | unsupported |
| everything else (57 remote peers, 135 beacon keys, 43 `aks_ref_key_*`, escrow and backup bags, the SE and memento calls, 15 change secret) | `kIOReturnUnsupported` | the same |

So with the bit: SpringBoard and ManagedConfiguration see a device with no passcode; keychain items work as before and are not protected;
access-controlled keychain items (`SecAccessControl`, selector 43) cannot be made; **a passcode cannot be set** (selector 15 answers
unsupported, so keybagd's change fails and the state stays "no passcode").

## Protection classes on the data volume

The root of an S9 is one HFS Plus volume on USERDATA, `/private/var` included. Apple's HFS keeps a file's class in its content-protection
attribute next to the file's wrapped key, on a volume formatted for it, and the storage driver encrypts with that key; this port builds HFS
without that (`CONFIG_PROTECT` off in `bsd/sweet_hfs/sweet_hfs_compat.h`): no key hierarchy for files, no inline encryption. With
`sweetspeed&0x100000` the bookkeeping half exists:

- **Set:** `fcntl(F_SETPROTECTIONCLASS)`, `setattrlist(ATTR_CMN_DATA_PROTECT_FLAGS)`, `open_dprotected_np`, a create that names a class.
  Files take A, B, C, D or CX; directories "none", A, B, C or D; F only at creation (as in Apple's HFS). Anything else is `EINVAL`; a vnode
  that is neither a file nor a directory `EBADF`.
- **Kept:** in the extended attribute `com.apple.system.scion.pclass` (4 bytes: version 1, the class), a "system" attribute that the xattr
  calls of user space neither list nor reach. A cnode remembers it once read.
- **Inherited:** a new file or directory takes the class its directory was given, as on iOS; one extra attribute write per create in such a
  directory.
- **Read:** `fcntl(F_GETPROTECTIONCLASS)`, `getattrlist`, `getattrlistbulk` (for items in core), `NSURLFileProtectionKey`. What was never
  given a class reads back as **D** (`NSFileProtectionNone`), which is the truth here. `F_GETPROTECTIONLEVEL` answers 5,
  `F_GETDEFAULTPROTLEVEL` D.
- `sweetspeed&0x200000` adds `MNT_CPROTECT` to the mount's flags. That is a separate bit because it changes what user space does, not what
  the kernel answers: `MKBDeviceFormattedForContentProtection()` is that flag on `/var`, and SQLite strips its file-protection open flags
  without it ("volume does not support data protection").

**Purgeable files (`sweetspeed&0x1000000`).** PosterFoundation marks every directory of the wallpaper store purgeable
(`-[NSURL pf_setPurgable:afterDate:error:]`) with APFS's `fsctl()`s, whatever the volume is: group `'J'`, 3 (is this directory counted, 24
bytes out), 113 (count it, 272 bytes), 111 (mark purgeable, 48 bytes), and 109 to ask (48 bytes out). On HFS they were `ENOTTY`: "Error
marking ... as purgable" 115 times a boot, and no poster store. This volume keeps no such accounting and nothing here purges files, so with
the bit the calls succeed and are forgotten; the questions answer zeroes ("not counted", "not purgeable").

An image of the volume attached on a Mac shows nothing of this (the attribute is hidden there too); a kernel without the bit ignores the
attributes and answers as before.

## Sandbox extensions (`sweetspeed&0x2000000`)

This kernel runs without a sandbox: the stand-in policy named "Sandbox" (`bsd/kern/sweet_amfi.c`) allows every check. A sandbox extension is
how one process hands another the right to a file: `sandbox_extension_issue_*` makes a token (`__mac_syscall("Sandbox", 5, ...)`), the other
side's `sandbox_extension_consume` (6) turns it into a handle, `release` (7) gives it back. The stand-in failed 5 and 6 with `ENOENT`, and
the programs check: PosterKit logs "failed to issue sandbox extension to PID ..." for every poster path it sends to a poster extension. With
the bit, issue answers a token (a fixed prefix, then the class and the path, readable) and consume answers a handle (a counter); nothing is
recorded and nothing is checked, because there is nothing to grant. The argument layouts are libsystem_sandbox's of iOS 27.0.1: issue takes
seven 8-byte fields (class, type, data, flags, the token buffer of 0x3ff0 bytes, pid, more flags), consume three (token, its length, where
the handle goes). **Built, not run on a phone yet.**

## The user client as 24A446 calls it

Read from `AppleKeyStore.framework` (libaks is a dylib of the shared cache on iOS 27), MobileKeyBag and keybagd: each `aks_*` function ends in
one `IOConnectCallMethod` with a constant selector. The numbers below are the ones this page needs; they are the numbers of iOS 16 (the
16.5 map is in `docs/keystore.md` and `docs/passcode.md`).

| Selector | libaks function | Shape |
|---|---|---|
| 0 | (open) | none |
| 2 | `aks_create_bag` | 7 scalars in |
| 3 | `aks_save_bag` | handle in; the bag out, as a structure |
| 4 | `aks_unload_bag` | handle |
| 5 | `aks_set_system` (`_with_passcode`, `_with_opts`) | 5 scalars in |
| 6 | `aks_load_bag` | the bag in, as a structure; handle out |
| 7 | `aks_get_lock_state` | handle in; flags out |
| 8, 32 | `aks_lock_device` (32: now) | handle |
| 9 | `aks_unlock_device` | 4 scalars in: handle, the passcode's address, its length, 0 |
| 10, 11 | `aks_wrap_key`, `aks_unwrap_key` | handle, class; key in; key out (10 also returns the class) |
| 12, 13 | `aks_unlock_bag`, `aks_lock_bag` | 5 scalars; handle |
| 14 | `aks_get_system` | handle out |
| 15 | `aks_change_secret` (`_opts`, `_with_kek`) | 12 scalars in: [0] handle, [1] the generation option (keybagd: 4 "make a new bag", 0 for a bag kept in xART), [3] and [4] the old passcode's address and length, [5] and [6] the new one's, [7] to [9] an ACM blob and a parameter, [11] flags; 1 scalar out: the handle of the bag that holds the new secret |
| 17, 35 | `aks_get_device_state`, `aks_get_extended_device_state` | handle in; DER out |
| 23 | `aks_get_bag_uuid` | 16 bytes out |
| 26, 27, 100, 101 | `aks_assert_hold`, `_drop`, `_promote`, `_consume` | 3, 2, 2, 1 scalars |
| 30, 31 | `aks_set_configuration`, `aks_get_configuration` | DER in; DER out (a 32 KB buffer) |
| 40 | `aks_generation` | 2 scalars in, 1 out |
| 42 | `aks_verify_password` | a packed list in, the passcode first |
| 43 | `aks_ref_key_*` (`_aks_operation`) | DER both ways |
| 55 | `aks_unload_session_bags` | 1 scalar |
| 57 | `aks_remote_peer_get_state` | |
| 102, 103 | (MobileKeyBag: the first-unlock notification was posted; was it) | 1 scalar out for 103 |
| 105 | `aks_change_secret_epilogue` | 4 scalars |
| 107 | `aks_se_support_in_rm_is_set` | 1 scalar out |
| 115 | `aks_prewarm_sps` | none |
| 135 | `aks_bak_get_beacon`, `aks_bak_get_swizzler` | 2 scalars; 16 KB out |
| 139 | `aks_memento_get_state` | 4 KB out |
| 160 | `aks_enable_cache_flow` | 1 scalar |

Who was seen calling what on the phone (the kernel logs the first call of each selector with its caller): keybagd 0, 7, 14, 107; securityd
10, 11; SpringBoard 8, 26, 27, 32, 135; `profiled` and the passcode settings 31; containermanagerd, backboardd 17; powerd, sleepd,
cameracaptured, backboardd 35; sharingd 57.

## What is not there, and what a passcode would take

- **A passcode** is not in `wip/ios27-keystore`; it is the branch `wip/ios27-keystore-passcode` on top of it (`sweetspeed&0x4000000`), which ran once on a phone: set, lock, a wrong and the right passcode, and removal, by raw calls and through `MKBKeyBagChangeSystemSecret`, `MKBLockDevice` and `MKBUnlockDevice`, that is through Apple's keybagd, each with the lock state it should give (`dptest passcode`). It is a screen lock: a PBKDF2 verifier and the wrong-attempt delays of Scion's 16.5 key store in `/private/var/keybags/scion-passcode`, no encryption. Not seen yet: the lock screen's keypad by hand, a restart with a passcode set, Settings' own panes. How it meets keybagd: Settings changes the passcode through `profiled` and keybagd's XPC service. keybagd's path for a device without a Secure Enclave
  ("KBChangeSystemNonSeSecret" in its log) is: `aks_change_secret_opts` (15), which answers the handle of the bag that holds the new secret;
  "KBSaveSystemBagHandle": `aks_save_bag` (3) on that handle and the answer written, with the opaque data that says what kind of passcode it
  is, to `/private/var/keybags/systembag.kb`; `aks_set_system_with_opts` (5) when the handle is a new one; `aks_change_secret_epilogue` (105).
  Before it, `aks_get_lock_state`, `aks_memento_get_state` (139), the configuration's user and group UUIDs, and MobileGestalt answers decide
  between this path and the Secure Enclave ones. The lock screen then uses 9 (unlock), 8 and 32 (lock), 42 (verify), the state (7, 17, 35)
  and IOKit's general-interest messages on the service. Without the passcode branch selector 15 answers "unsupported", so a
  change fails and the device stays without a passcode. Scion's iOS 16.5 kernel has the state machine, the verifier, the delays after wrong
  attempts and a real key bag for the keychain (`MiniAppleKeyStore.cpp`, `docs/passcode.md`, `docs/keystore.md`), behind its own daemon in
  place of keybagd's service; the work here is to bring that core to this tree and to answer Apple's keybagd as above. It needs keybagd's own
  log from a phone at each step.
- **Encryption of files.** Stage 4 of `docs/keystore.md` applies unchanged: a volume formatted for content protection, keys per file, and a
  storage driver that encrypts.
- **Access-controlled keychain items and LocalAuthentication** (`aks_ref_key_*`, AppleCredentialManager): no stand-in.
- **Effaceable storage, escrow and backup bags, anything of the Secure Enclave**: no, and for the last no plan.

## Testing

`tools/ios27/keystore/` has a test that runs on the phone: `dptest`, a launchd job (as root and as `mobile`) that waits a minute after boot and
writes `/private/var/local/oslog/dptest.log` and `dptest-mobile.log`: the mount's flags; classes on new files and directories by every way of
setting and reading them, and after reopening; the key bag state through MobileKeyBag; the read-only selectors of the user client; a keychain
item added, read back and deleted for three accessibility classes; MobileGestalt's passcode answers; a system container path. `build.sh`
builds it with your Xcode; `install-dptest.sh MOUNTPOINT` puts it and its two jobs in an attached root image (and in the root's launchd job
cache), `uninstall-dptest.sh` takes them out. It calls no selector that changes the key store's state. A third job, installed only with `DPTEST_PASSCODE=1 install-dptest.sh`, is the passcode test (`dptest passcode`, report `dptest-passcode.log`): it sets a passcode, locks, tries a wrong and the right passcode, and removes it again, first by raw calls and then through MobileKeyBag; a last step in a separate process removes the passcode whatever happened before. It needs the passcode branch's kernel; on any other it changes nothing.
