# Licensing

This file is a plain-English map of who may use what under which licence. It does not replace the licence files.

The repository-level `LICENSE` (APSL-2.0 for the XNU-derived kernel and `drivers/`, MIT for the rest, by directory), `LICENSE.APSL`, `NOTICE`, `THIRD_PARTY_LICENSES.md` and `LICENSES.md` are not changed.

## Original work: MIT-0

Files that the Scion project wrote entirely from scratch (every commit by the project's authors, no third-party notice, nothing ported or derived from another codebase) are licensed **MIT-0** (MIT No Attribution, SPDX `MIT-0`; text in `LICENSES/MIT-0.txt`). Each such file carries `SPDX-License-Identifier: MIT-0` in its first lines, except the files listed in the table below, which cannot hold a comment.

Two groups are MIT-0 here: the tools, scripts, documentation and CI files (no connection to XNU), and a separate set of new driver files under `drivers/` that are compiled into the XNU kernel. The second group was written from scratch, and MIT-0 is compatible with distributing it alongside APSL-2.0 code; the XNU files they sit with stay APSL-2.0.

MIT-0 paths (top-level location, number of files):

- `.github`: 3 files
- `avf`: 15 files
- `configs`: 1 file
- `docs`: 14 files
- `drivers`: 19 files
- `profiles`: 2 files
- `qemu`: 3 files
- `tools`: 29 files
- `xnu`: 1 file

The full per-file list is in the pull request that introduced this file, and every file's own first lines are authoritative.

## Files that keep their existing licence

- Anything not listed above, and any file that has no explicit `SPDX-License-Identifier: MIT-0` tag, stays under the repository's existing licence, as before.
- The XNU-derived kernel and everything the project modified from HakiOS (other authors in history): APSL-2.0 / MIT as `LICENSE` says.
- `xnuloader/`, `avf/shim/` (boot loader and shim), `devices/blazer/` (carried from scion-blazer), `wifi/` (Linux brcmfmac layer), `socs/`, `userland/`, `tools/ios27/`, `docs/ios27/`, `tools/{metal,mesa,gfxstream,mali-drmshim,vk}/`: left as they are (see the PR for reasons).

## MIT-0 files without an in-file tag

These files cannot carry a comment (or begin with front matter that must stay first), so they are covered by this table instead.

| Path | Licence |
|---|---|
| `tools/scion-splash/scion-sprout.svg` | MIT-0 |

