# The root's own executables: the rewrite for ARMv8.0 cores (iOS 27 on the Galaxy S9)

[docs/ios27/AOT-REWRITE.md](../../../docs/ios27/AOT-REWRITE.md) rewrites every instruction the Exynos M3 lacks in the dyld shared cache. What it leaves is
the root filesystem's Mach-O files outside the cache: 284 files of iOS 27.0.1 (24A446), 74,377 sites (LSE 73,300, SHA3 448, LDAPR 410, FP16 203; the
census of 9 November counted 46 FP16 words because it kept the mnemonic and dropped the operand, so `fcmp h0, ...` went into the wrong bucket). These tools
rewrite those files the same way, off the phone, and check the result. **Nothing made by them has been run on a phone when this was written** (9 November
2026): the layout, every byte and the frame contract are checked; that dyld loads a file with the appended segment is not.

Not in the repository: the stub generators (`gen/rewrite-grow.py`, `fp16_stub.py`, `misc_stub.py`, `bf16_stub.py`, `classify.py` and the oracle word tables of
the cache rewrite: they are not published yet, see AOT-REWRITE.md), the list of files (`files284.txt`, from the census) and of course the binaries. The tools
expect them beside themselves (`gen/`, `ref/<absolute path>` for the untouched originals copied out of your own root image).

| File | What |
|---|---|
| `macho-rewrite.py IN OUT [--mode A\|T\|auto] [--dry-run] [--report F]` | one Mach-O (thin or fat, every arm64/arm64e slice): every site becomes `B thunk`; the thunk and the bodies are the cache rewrite's (`STR x30, [sp, #-144]!`, `BL body`, `LDR x30, [sp], #144`, `B site+4`; one body per distinct word, the LSE ones with an A55 half and an M3 half); `LDAPR` becomes `LDAR` in place; sites inside `LC_DATA_IN_CODE` ranges are left |
| `run-all.py [--mode ..] [--stage D] [--out M]` | every listed file into `stage/<absolute path>` and `manifest.json` (path, SHA-256 before and after, mode, slices, sites by class, what is left and why, cdhash before and after) |
| `macho-check.py [--stage D] [--manifest M] [--only LIST]` | the checker, with a Mach-O parser of its own; it compares against `ref/` and fails on any deviation (below) |
| `negtest.sh` | seven tampered trees (a body byte, a site put back, a data byte, a signature byte, the segment's size, the chained fixups' segment count, and an untouched control): the checker must fail the six and pass the control |
| `install-rootrw.sh ROOTIMG [--only LIST] [--undo]` | writes the staged files into a raw HFS+ root image in place (`cat new > target`, so owner, mode, flags and extended attributes stay), keeps each original under `orig/` the first time, verifies SHA-256 after every write; `--undo` puts the originals back. It refuses an image outside its own directory unless `FORCE_ANY_IMAGE=1` |
| `verify-image.sh IMG stage\|ref` | every listed file inside an image against the staged tree or the originals |
| `mklists.py` | three lists for a staged roll-out: three leaf daemons; everything not held back; then the held-back ones (launchd, dyld, the interpose libraries, boot-critical daemons, the biggest files, kernel extensions), with the reason for each |

## Where the stubs go

- **T: the zero padding at the end of `__TEXT`** (after the last section, inside the segment's file range). No load command changes, the file keeps its size; the
  only bytes that differ are the sites and the stubs. 252 of the 284 files fit.
- **A: a new segment `__STUBX`** (`LC_SEGMENT_64`, no sections, read and execute) **after the end of the file**, for the 31 files whose stubs do not fit (the big
  ones). File offset = the old size rounded up to 16 KB, address = the end of the last segment rounded up to 16 KB; the load command is added last, so no
  segment index changes. Where the header has no 72 free bytes, advisory load commands are dropped, only as far as needed (`LC_SOURCE_VERSION`,
  `LC_LINKER_OPTIMIZATION_HINT`, `LC_DYLIB_CODE_SIGN_DRS`, `LC_DATA_IN_CODE`, `LC_FUNCTION_STARTS`; their data stays in `__LINKEDIT`). The chained fixups'
  `seg_count` is raised by one without moving a byte of `__LINKEDIT` (free bytes after the table, or the table started four bytes earlier in the pad after the
  28-byte header).
- **The code signature is not touched** in either mode: the CodeDirectory, so the cdhash, the identifier and the entitlements, are byte for byte the original's.
  Edited pages no longer match their page hashes and `__STUBX` lies beyond `codeLimit`. That is accepted by this kernel only as it is built and booted here:
  a DEVELOPMENT kernel with `cs_integrity_skip=1` marks a page validated before it hashes it (`bsd/kern/ubc_subr.c`, `cs_validate_range` and `cs_validate_page`),
  `cs_enforcement_disable=1` reports no violation otherwise, the "section outside code signature" test of `load_segment()` (`bsd/kern/mach_loader.c`) cannot fire
  for a segment that starts after the signature (its subtraction is unsigned and wraps), and `F_ADDFILESIGS_RETURN` gives dyld the same covered end as before. A
  RELEASE kernel would kill the process at the first edited page.
- Inserting the segment before `__LINKEDIT` and signing again was not built: it needs the same header room and the same chained-fixups change, and it changes the
  cdhash for nothing this kernel asks for.

One file cannot be rewritten (`/usr/lib/libBacktraceRecording.dylib`, 15 sites: no padding and 8 bytes short of header room), and one word is left on purpose
(a `CASPAL` encoding with an odd register pair in `fairplayd.H2`: unallocated, so data).

## What the checker checks

For each slice: header and load-command sizes; segments 16 KB aligned, inside the file, in order, with no two overlapping in the file or in memory; the existing
segments unchanged; the load commands = the original's minus the advisory ones dropped, plus `__STUBX` last; `otool -l` parses the file; the signature's load
command and blob unchanged and `codesign -dvvv` printing the same CDHash and Identifier; the chained fixups consistent, every old entry still where its segment
is; everything outside sites, load commands, the fixups header and the stub area identical to the reference; every changed word of an executable section a `B`
into the stub area, to exactly the generator's thunk, whose `BL` goes to exactly the body the generator makes for the **original** word (taken from the
reference); within +-128 MB; nothing else in the stub area; the frame contract of AOT-REWRITE.md for every distinct body (no store lands in the thunk's 144
bytes); a second census (no word the generators handle is left, and no stub word needs more than ARMv8.0 + CRC except the A55 half of the LSE bodies).

On 9 November: 283 files staged (252 T, 31 A), 73,951 sites to thunks and 410 `LDAPR` in place, 1,480,860 bytes of stubs, 1,745 distinct bodies with no
frame-contract violation, the checker green on the staged tree and on an all-A tree (273 files; 11 lack header room), `negtest.sh` as described, the installer
tried on a private copy of an image (install, a second run that changes nothing, undo, each verified).

## Not known until it runs on a phone

1. Whether dyld accepts mode A: a segment after `__LINKEDIT`, the dropped advisory load commands, the changed `seg_count` (for 21 slices the table starts four
   bytes earlier). Mode T changes no structure; the first list and nearly all of the second are T.
2. Kernel extensions and DriverKit files go through other loaders (this kernel probably loads none of them).
3. There is no unwind information for the stub area.
