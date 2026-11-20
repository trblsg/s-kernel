#!/usr/bin/env python3
"""Build-time, reversible S9 ablation: remove widget extensions and AegirPoster.

Run ONLY on userland/sysroot, which build.sh copies from sysimage/rootfs.
Never run this on a live phone or on sysimage/rootfs itself.
No SpringBoard/chronod/PosterBoard binaries or launchd jobs are modified.
"""
import argparse
import os
import plistlib
import shutil
from pathlib import Path

BUNDLE_SUFFIXES = (".appex", ".xpc", ".bundle", ".app")
POSTER_ID = "com.apple.nanouniverse.aegirproxyapp.aegirposter"


def info_for(bundle):
    info = bundle / "Info.plist"
    try:
        with info.open("rb") as stream:
            result = plistlib.load(stream)
            return result if isinstance(result, dict) else {}
    except (OSError, ValueError, plistlib.InvalidFileException):
        return {}


def classify(bundle):
    info = info_for(bundle)
    ident = str(info.get("CFBundleIdentifier", "")).lower()
    name = bundle.stem.lower()
    extension = info.get("NSExtension", {})
    if not isinstance(extension, dict):
        extension = {}
    point = str(extension.get("NSExtensionPointIdentifier", "")).lower()
    extkit = info.get("EXExtensionPointIdentifier", "")
    if isinstance(extkit, str):
        point = point or extkit.lower()

    # Drop only the Aegir extension itself, never its host framework/app.
    if "aegirposter" in name or ident == POSTER_ID or ident.endswith(".aegirposter"):
        return "aegir"

    # A WidgetKit or legacy Today extension, not e.g. Share/Intents/Files.
    if bundle.suffix.lower() == ".appex":
        if "widget" in point or "widget" in name or "widget" in ident:
            return "widget"
    return None


def candidates(root):
    found = []
    for base, dirs, _files in os.walk(root, topdown=True, followlinks=False):
        for name in list(dirs):
            path = Path(base) / name
            if path.is_symlink():
                dirs.remove(name)
                continue
            if not name.lower().endswith(BUNDLE_SUFFIXES):
                continue
            category = classify(path)
            if category:
                found.append((category, path))
                dirs.remove(name)  # don't walk into a bundle that will be removed
    return sorted(found, key=lambda item: str(item[1]))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sysroot", type=Path)
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--manifest-in", type=Path)
    parser.add_argument("--manifest-out", type=Path)
    args = parser.parse_args()
    root = args.sysroot.resolve()
    if root.name != "sysroot" or not (root / "System/Library").is_dir():
        parser.error("requires the temporary userland/sysroot tree; never run on a live SYSTEM")

    found = candidates(root)
    widgets = [path for kind, path in found if kind == "widget"]
    posters = [path for kind, path in found if kind == "aegir"]
    for kind, path in found:
        print(f"widget_ablation: {'would_remove' if args.dry_run else 'remove'} "
              f"{kind} {path.relative_to(root)}", flush=True)
    print(f"widget_ablation: found {len(widgets)} widget bundles, "
          f"{len(posters)} AegirPoster bundles", flush=True)

    # Fail instead of publishing an image that silently still contains one of
    # the two categories requested for the experiment.
    if not widgets or not posters:
        raise SystemExit(
            "widget_ablation: STOP: expected at least one widget extension and "
            "one AegirPoster bundle; inspect SYSTEM image paths before retrying"
        )
    if not args.manifest_in or not args.manifest_out:
        parser.error("--manifest-in and --manifest-out are required")
    if args.dry_run:
        print("widget_ablation: dry run: no changes made", flush=True)
        return

    # mkapfs.py can rebuild missing directories from system.manifest.
    # Use a filtered COPY of the manifest so deleted bundles stay absent.
    rels = tuple(str(path.relative_to(root)).encode("utf-8", "surrogateescape")
                 for _kind, path in found)
    kept, pruned = 0, 0
    with args.manifest_in.open("rb") as source, args.manifest_out.open("wb") as output:
        for line in source:
            fields = line.rstrip(b"\n").split(b"\t")
            path = fields[3] if len(fields) >= 4 else b""
            if any(path == prefix or path.startswith(prefix + b"/") for prefix in rels):
                pruned += 1
            else:
                output.write(line)
                kept += 1
    print(f"widget_ablation: manifest kept={kept} removed={pruned}", flush=True)

    for _kind, path in found:
        shutil.rmtree(path)
    marker = root / ".ioss9-widget-ablation"
    marker.write_text(
        "S9 experiment only: WidgetKit/Today .appex and AegirPoster bundles\n"
        f"widget_bundles={len(widgets)} aegir_bundles={len(posters)}\n",
        encoding="utf-8",
    )
    remaining = candidates(root)
    if remaining:
        raise SystemExit(
            f"widget_ablation: STOP: {len(remaining)} matching bundles still present"
        )
    print(
        "widget_ablation: READY: all found widget extension bundles and "
        "AegirPoster removed from this temporary SYSTEM staging tree",
        flush=True,
    )


if __name__ == "__main__":
    main()
