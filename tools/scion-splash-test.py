#!/usr/bin/env python3
# SPDX-License-Identifier: MIT-0
"""Test of the boot screen's drawing (docs/boot-screen.md): tools/scion-splash-preview.c, the loader's own code, built for
the host and run at the S9's panel size, the iPhone X's size (what QEMU's ramfb is) and a small one.

usage: tools/scion-splash-test.py        (needs a C compiler, cc or $CC; and for the header's check, Pillow and rsvg-convert, else that step is skipped)
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

def fork_added():
    """The files the ios-16.5 fork adds to Apple's XNU, at their paths in the tree (tools/xnu-tree.py added): Scion's own headers."""
    return subprocess.run([sys.executable, os.path.join(ROOT, "tools", "xnu-tree.py"), "added", "ios-16.5"], check=True, capture_output=True,
                          text=True).stdout.strip()
failed = 0


def check(what, ok, detail=""):
    global failed
    print(("ok   " if ok else "FAIL ") + what + ("" if ok else "  " + str(detail)))
    failed += not ok


def ppm(data):
    m = re.match(rb"P6\n(\d+) (\d+)\n255\n", data)
    w, h = int(m.group(1)), int(m.group(2))
    return w, h, data[m.end():]


def px(img, w, x, y):
    i = (y * w + x) * 3
    return tuple(img[i:i + 3])


def main():
    cc = os.environ.get("CC") or shutil.which("cc") or shutil.which("clang") or shutil.which("gcc")
    if not cc:
        sys.exit("scion-splash-test: no C compiler")
    with tempfile.TemporaryDirectory() as d:
        exe = os.path.join(d, "preview")
        r = subprocess.run([cc, "-O2", "-Wall", "-Wextra", "-Werror", "-I", os.path.join(fork_added(), "osfmk/console"), "-o", exe,
                            os.path.join(HERE, "scion-splash-preview.c")],
                           capture_output=True, text=True)
        check("the preview builds with the loader's headers, no warnings", r.returncode == 0, r.stderr)
        if r.returncode:
            sys.exit(1)
        hw = int(re.search(r"#define SPLASH_LOGO_HW\s+(\d+)", open(os.path.join(fork_added(), "osfmk/console/scion_splash.h")).read()).group(1))
        logo = open(os.path.join(ROOT, "xnuloader/scion_splash_logo.h")).read()
        lw, lh = (int(re.search(r"#define SPLASH_LOGO_%s (\d+)" % k, logo).group(1)) for k in "WH")
        check("SPLASH_LOGO_HW is the sprout's height over its width", lh * 1000 // lw == hw, (lw, lh, hw))
        for w, h in ((1440, 2960), (1125, 2436), (480, 800)):
            out = subprocess.run([exe, "-w", str(w), "-h", str(h), "-p", "500"], capture_output=True, check=True).stdout
            pw, ph, img = ppm(out)
            check("%dx%d: the picture has the screen's size" % (w, h), (pw, ph) == (w, h) and len(img) == w * h * 3)
            top, bottom = px(img, w, 5, 5), px(img, w, 5, h - 5)
            check("%dx%d: the ground is dark, lighter at the top" % (w, h), sum(top) > sum(bottom) and max(top) < 60, (top, bottom))
            greens = 0
            for y in range(int(h * 0.30), int(h * 0.50)):
                for x in range(w // 3, 2 * w // 3):
                    r, g, b = px(img, w, x, y)
                    greens += g > 150 and g > r and g > b
            check("%dx%d: the sprout's leaves are on the screen" % (w, h), greens > (w * h) // 2000, greens)
            # the progress line: filled green on the left half, track grey on the right half, nothing outside it
            by = None
            for y in range(int(h * 0.45), int(h * 0.80)):
                if px(img, w, int(w * 0.40), y)[1] > 150:
                    by = y
                    break
            check("%dx%d: the progress line is under the sprout" % (w, h), by is not None)
            if by is not None:
                left, right = px(img, w, int(w * 0.40), by), px(img, w, int(w * 0.60), by)
                check("%dx%d: the line is filled to 50 %%, the track after it" % (w, h), left[1] > 150 and right[1] < 60 and right != px(img, w, int(w * 0.60), by - 20), (left, right))
    r = subprocess.run([sys.executable, os.path.join(HERE, "scion-splash-logo.py"), "--check"], capture_output=True, text=True)
    if "No module named" in r.stderr or "FileNotFoundError" in r.stderr:
        print("skip the header's check (Pillow or rsvg-convert missing)")
    else:
        check("xnuloader/scion_splash_logo.h is what tools/scion-splash/scion-sprout.svg makes", r.returncode == 0, r.stderr)
    print("scion-splash-test: " + ("all passed" if not failed else "%d failed" % failed))
    sys.exit(1 if failed else 0)


main()
