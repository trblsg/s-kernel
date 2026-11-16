# Data inside `__text`: what the instruction rewrite must not touch

The cache pipeline (`tools/ios27/fullroot/pipeline/cache-pipeline.sh`, `docs/ios27/AOT-REWRITE.md`) makes iOS 27's shared
cache runnable on the Exynos M3 (ARMv8.0) by rewriting, ahead of time, every instruction that core lacks. The passes
find their sites by bit pattern: every 32-bit word of the pure-instruction sections that decodes as an LSE atomic, an
FP16 operation, a SHA3 / LRCPC / LOR instruction, ... is replaced by a branch to a thunk (or rewritten in place).

Not every word of those sections is an instruction. Hand-written assembly keeps its tables next to its code: bit
masks, polynomial coefficients of the math library, hash round constants, transform tables of codecs, ident strings.
A table entry that happens to have the bit pattern of a rewritten instruction was rewritten like one:

| table entry | what it is | what the decoder saw |
|---|---|---|
| `0x1fffffff` | a 29-bit mask (and the upper half of a 61-bit one) in an entropy decoder's mask table | `FNMSUB H31, H31, H31, H31` (FP16) |
| `0x78a5636f` | a SHA-256 round constant | `LDUMAXAH W5, W15, [X27]` (LSE) |
| words of `double` coefficient tables | sin/cos, tan, exp, log, tanh tables of the math library | SHA3, LSE, LRCPC, FP16 |

Nothing traps: the table simply holds a branch opcode where a constant was, and the function that reads it computes
garbage. On the phone that was, among others:

- **LZFSE buffer decoding returned wrong bytes** whenever the literal decoder's bit accumulator held exactly 29 or 61
  bits (two of 64 mask entries damaged). Output length right, content wrong. Every reader of compressed asset
  catalogs got damaged data: layered app icons came out as the placeholder (their vector layers no longer parsed) or
  as striped garbage (bitmap layers), deterministically, byte for byte the same at every boot. Restoring the two
  words alone brought back Mail, Clock, News, App Store, Wallet, Settings, Safari and TV on the home screen.
- wrong results from several math-library functions (their coefficient tables), wrong SHA-256 digests from two
  private SHA-256 implementations, a damaged transform table in an audio codec and in a software video codec.

## The guard

Two parts: the rewrite passes stay away from the data words (the **skip map**), and a check that they did.

`pipeline/cache/textdata-guard.py` needs one input besides the caches: the list of words no A64 decoder accepts
(`undef.txt`, what `oracle.sh` writes when it runs llvm-objdump with every feature on over the cache's distinct words).
Two rules, either one flags a word of a pure-instruction section as *not provably an instruction*:

- **C**: an undecodable word within 8 words before *and* within 8 words after it.
- **D**: walking back from it over decodable words, the first other thing met is an undecodable word, not the start
  of its function (`LC_FUNCTION_STARTS`) and not an instruction that ends a block (`B`, `BR`, `RET`, `BRK`, ...). Code
  is entered at a function start or after such an instruction; a table entry sits behind other table entries.

Exact pointer-authentication opcodes are exempt (a table word is not exactly `RETAB` by accident, and code next to
vendor-specific instructions the decoder does not know would flood the report).

### Step 0b: the skip map

    textdata-guard.py map STOCK/dyld_shared_cache_arm64e --undef undef.txt [--reviewed textdata-reviewed.list] --out skip.npy

walks every pure-instruction section of the **stock** cache and writes the sorted addresses (unslid, `uint64`, `.npy`)
of the words the passes must never rewrite: every word where rule C or D holds, every word in an `LC_DATA_IN_CODE`
range of its image (an authoritative data marker; this cache has almost none), minus the addresses a person reviewed
as code, plus the ones reviewed as data. On the 24A446 cache that is 6.3 million words (50 MB): zero padding and literal
pools between functions, almost none of them with the bit pattern of anything a pass rewrites. The map is built once per
build from the stock cache; it is as private as the reviewed list (addresses of a vendor cache).

The pipeline exports it as `TEXTDATA_SKIP=skip.npy` for steps 1..6.

### The passes skip

`a64e-rewrite.py` (cache mode), `a64e-rewrite2.py`, `lse-rewrite.py` and `rewrite-grow.py` read `TEXTDATA_SKIP`. For
each section they take the indexes of the words that are in the map (`np.searchsorted` on the sorted array) and clear
them from their candidate masks **before** anything is rewritten or any thunk or body is allocated. A candidate that is
left alone is counted (`textdata: N words left alone` at the end of a pass, `data_left` in its counts) and, in the
passes that write a log, logged as `0xSITE WORD D`; `lse-rewrite.py verify`/`restore` and `grow-check.py` understand
those lines (`grow-check.py` checks that the site still holds the stock word). A candidate the pass could not have
rewritten anyway (unallocated encodings, a `LDRAA` with an out-of-range offset) is not counted. `stubscan.py --fix` only
touches the stub islands outside every image's `__TEXT` and does not read the map. Without `TEXTDATA_SKIP` every pass
does exactly what it did before.

Root Mach-Os (`macho-rewrite.py`, `a64e-rewrite.py macho`) have no map: the words of an `LC_DATA_IN_CODE` range are not
touched, and the two masked classes (`LDRAA`/`LDRAB`, `LDAPR`) are also left alone when rule C holds with the
self-contained predicate "cannot be an instruction" (top-level encoding group <= 3, or a high half of zero; no oracle
list needed). `A64E_NO_RULE_C=1` turns that off. The words left alone are counted as `data_left`.

### Step 8: check and verify

    textdata-guard.py check  STOCK/... NEW/... --undef undef.txt [--reviewed textdata-reviewed.list]   # exit 1 on an unreviewed candidate
    textdata-guard.py verify STOCK/... NEW/... --map skip.npy                                         # exit 1 if a mapped word changed
    textdata-guard.py scan   STOCK/... NEW/... --undef undef.txt > candidates.tsv                     # the review tool

`cache-pipeline.sh` runs `check` (every word the build changed that rule C or D flags must be in the reviewed list) and
`verify` (every word of the new cache whose address is in the map equals the stock word; the offenders are listed and
the build fails). A build of another cache version stops at `check` until its candidates are reviewed.

`scan` is how the list is made: on the 24A446 cache of a build without the map it gives 145 candidates out of 33.7
million changed words. A person reads them once: 40 are data (the table above), 105 are code (one- or two-instruction
functions between two literal pools or runs of zero padding, and code after vendor-specific instructions). The list has
a line per candidate, `VADDR DAMAGED STOCK note` for data and `VADDR code note` for code; it holds addresses and words
of the vendor's cache, so it lives in the builder's private directory (`$IOS27_PRIVATE/textdata-reviewed.list`, next to
`patches.txt` and `undef.txt`), not in this repository. The **code** lines matter to the map: the rules cannot tell a
tiny function between two zero runs from a table, so without the list the map keeps those 105 functions from being
rewritten (they would then trap on the phone). With the list, they are rewritten and the 40 data words are not.

`apply` (put the reviewed data words back from the stock cache; idempotent; `--revert` damages them again, for an A/B
test) is for caches that were built without the map, and what the pipeline does when there is no `undef.txt` but a
reviewed list. The thunks of the sites it restores stay in the cache, unreachable; a build with the map never makes them.

Roots that already carry a cache built without the map: `steps.d/85-textdata-restore.sh ROOT` (the same `apply`, on
the mounted root, in place).

`textdata-guard-test.py` is a self-test (synthetic section, no vendor data): `python3 textdata-guard-test.py`.

## What the rules cannot see

A single constant between a `ret` and the next function start, with decodable neighbours on both sides, is not
flagged by C or D. None is known; a constant of that kind that also has the bit pattern of a rewritten instruction
would have to be found by its symptom. The principled fix is a real code/data map (recursive descent from the
function starts, jump tables resolved) inside the rewrite passes themselves, so that they never leave a function's
reachable code; the guard is the check that such a map would have to pass as well.

## The second cause of the empty Settings rows (not the rewrite)

With LZFSE repaired the home screen was right, but thirteen symbol rows of Settings (Wi-Fi, Bluetooth, Battery,
Camera, Control Center, Home Screen, StandBy, Sounds, Focus, Screen Time, Passcode, Emergency SOS, Privacy) were
still empty. The icon agent's own log (os_log capture) names the step: CoreUI "failed to produce foreground layer"
for exactly those thirteen symbols. CoreUI draws a symbol whose glyph is not square oversampled and scales it down
with a Core Image filter, through a default `CIContext`; square glyphs skip the filter, which is why General and
Accessibility were drawn and Wi-Fi and Battery were not. A default `CIContext` is Metal, else OpenGL ES, else
**nil**: only a widget extension or an explicit software request gets the software renderer. The icon agent has
neither a Metal device nor OpenGL ES on this port ("No Metal renderer available", "No OpenGLES renderer available"),
got nil, and the icon was never made (the failure is cached as an empty image).

Two ways out were tried on the phone:

- the agent as a Metal client (client list + the injection that lets the arm64 Metal plug-in load): the tiles are
  drawn, but the glyphs come out empty and one app icon changes for the worse - the Metal driver's problem, open;
- one word of CoreImage in the shared cache (the builder's private word list, like `patches.txt`): "Metal is not
  available" goes to the software context instead of on to OpenCL / OpenGL ES / nil. All thirteen rows are drawn.
  Root step: `steps.d/86-coreimage-sw-context.sh` (private list; this repository carries only the mechanism).

Side effect to know: every process without a Metal device now gets a software Core Image context where it got
none, so work that silently did not happen before is done on the CPU.
