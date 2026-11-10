#!/usr/bin/env python3
# SPDX-License-Identifier: MIT-0
"""Make xnuloader/scion_splash_logo.h: the Scion sprout of the boot screen, as data the loader can draw.

usage: tools/scion-splash-logo.py [--width 420] [SVG] > xnuloader/scion_splash_logo.h
       tools/scion-splash-logo.py --check [SVG]          (the committed header is what the SVG makes)

The picture is tools/scion-splash/scion-sprout.svg: the sprout and the chip of Scion's own logo (scion-project.github.io's
logo.svg) without its dark rounded square, drawn to a transparent background, which is the boot screen's. It is
rendered with rsvg-convert (librsvg) at WIDTH pixels wide, then packed as a palette of at most 64 colours and, per
pixel, an 8-bit coverage, in runs: three bytes a run (length 1..255, coverage, palette index). The loader unpacks it
into two planes once and scales it to the screen with a bilinear sample. Python 3 with Pillow, to run this; the loader
needs neither.
"""
import argparse
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_SVG = os.path.join(HERE, "scion-splash", "scion-sprout.svg")
HEADER = os.path.join(HERE, "..", "xnuloader", "scion_splash_logo.h")


def render(svg, width):
    from PIL import Image
    with tempfile.TemporaryDirectory() as d:
        png = os.path.join(d, "s.png")
        subprocess.run(["rsvg-convert", "-w", str(width), svg, "-o", png], check=True)
        return Image.open(png).convert("RGBA")


def pack(im):
    from PIL import Image
    w, h = im.size
    px = im.load()
    solid = Image.new("RGB", (w, h))
    sp = solid.load()
    seen = 0
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            if a > 160:
                sp[x, y] = (r, g, b)
                seen += 1
    # (a palette from the solid pixels; the antialiased edge takes the nearest colour)
    q = solid.quantize(colors=64, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    flat = q.getpalette()[:64 * 3]
    pal = [tuple(flat[i * 3:i * 3 + 3]) for i in range(64)]
    used = sorted({q.getpixel((x, y)) for y in range(h) for x in range(w) if px[x, y][3] > 160})
    remap = {old: new for new, old in enumerate(used)}
    pal = [pal[i] for i in used]

    def nearest(c):
        return min(range(len(pal)), key=lambda i: sum((pal[i][k] - c[k]) ** 2 for k in range(3)))
    cache = {}
    tokens = []
    run = None
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            if a == 0:
                tok = (0, 0)
            else:
                key = (r, g, b)
                if key not in cache:
                    cache[key] = nearest(key)
                tok = (a, cache[key])
            if run is not None and run[0] == tok and run[1] < 255:
                run[1] += 1
            else:
                if run is not None:
                    tokens.append(run)
                run = [tok, 1]
    tokens.append(run)
    data = bytearray()
    for (a, i), n in tokens:
        data += bytes((n, a, i))
    return pal, bytes(data)


def header(width, svg_name):
    im = render(DEFAULT_SVG if svg_name is None else svg_name, width)
    pal, data = pack(im)
    out = []
    out.append("/*\n * The Scion sprout of the boot screen: data made by tools/scion-splash-logo.py from "
               "tools/scion-splash/scion-sprout.svg\n * (Scion's own logo, without its background). Do not edit by hand.\n */\n")
    out.append("#define SPLASH_LOGO_W %d\n#define SPLASH_LOGO_H %d\n#define SPLASH_LOGO_COLOURS %d\n\n" % (im.size[0], im.size[1], len(pal)))
    out.append("static const uint8_t splash_logo_palette[SPLASH_LOGO_COLOURS][3] = {\n")
    for c in pal:
        out.append("\t{ %d, %d, %d },\n" % c)
    out.append("};\n\n/* runs of (length, coverage, palette index), row after row */\n")
    out.append("static const uint8_t splash_logo_runs[%d] = {\n" % len(data))
    for i in range(0, len(data), 24):
        out.append("\t" + ", ".join("%d" % b for b in data[i:i + 24]) + ",\n")
    out.append("};\n")
    return "".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--width", type=int, default=420)
    ap.add_argument("--check", action="store_true")
    ap.add_argument("svg", nargs="?")
    a = ap.parse_args()
    text = header(a.width, a.svg)
    if a.check:
        have = open(HEADER, encoding="utf-8").read()
        if have != text:
            sys.exit("scion-splash-logo: xnuloader/scion_splash_logo.h is not what the SVG makes (tools/scion-splash-logo.py > it)")
        print("scion-splash-logo: the header is what the SVG makes")
        return
    sys.stdout.write(text)


if __name__ == "__main__":
    main()
