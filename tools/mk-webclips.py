#!/usr/bin/env python3
# SPDX-License-Identifier: MIT-0
"""Make the web clips Scion starts with: Info.plist and icon.png for each web app of a manifest.

usage: tools/mk-webclips.py --out DIR [--manifest FILE] [--only ID,ID] [--fullscreen] [--flat]
                            [--skel FILE] [--list]

A web clip is what Safari's "Add to Home Screen" makes: a directory <UUID>.webclip with an Info.plist (title,
address) and an icon.png, in /var/mobile/Library/WebClips; SpringBoard shows one icon for it. Nothing here
is a configuration profile (no .mobileconfig, no MDM, no signing): the files are the same ones Safari writes.

  --out DIR        where to write. By default DIR gets mobile/Library/WebClips/<UUID>.webclip/..., the shape of
                   userland/var/ (userland/build.sh merges it in); with --flat, DIR gets <UUID>.webclip/... directly
                   (the packages of the Sileo repository, which carry the clips in /var/jb/usr/share/scion/webclips)
  --manifest FILE  userland/webclips/manifest.json by default
  --only ID,ID     only these clips
  --fullscreen     every clip opens as a stand-alone web app (FullScreen true) instead of in Safari. Off by
                   default: the stand-alone launcher (com.apple.webapp) is not in the system image's file list, and
                   nobody has tried one on this port, while a clip that opens Safari needs nothing but Safari
  --skel FILE      also write the lines of etc/var.skel for the directories (d, mode, uid, gid, path), which init
                   makes before it copies the files (userland/src/init.c, mount_writable)
  --list           print "UUID id title url" for each clip and write nothing

The icons are drawn here, by this script, from shapes: a coloured square with a plain pictogram. They are ours
(MIT, like this script) and they deliberately are not the sites' logos: a wordmark or a logo belongs to its
owner (trademark, and for some a licence of its own), and a web clip is no reason to ask. The output is
the same bytes every time (no dates, no random numbers), so an image built twice is the same image.
"""
import argparse
import json
import math
import os
import plistlib
import struct
import sys
import uuid
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DEFAULT_MANIFEST = os.path.join(ROOT, 'userland', 'webclips', 'manifest.json')
NAMESPACE = uuid.UUID('6f1c0f7e-5c10-4a4e-9a52-5c10a0b5c10c')      # clips' UUIDs are made from their ids in it
SIZE = 180                                                          # iOS's icon for a web clip on the home screen
CLIPS_IN_VAR = ('mobile', 'Library', 'WebClips')


# --- shapes: signed distances in a unit square, negative inside -------------------------------------------

def circle(cx, cy, r):
    return lambda x, y: math.hypot(x - cx, y - cy) - r


def box(cx, cy, hw, hh, r=0.0):
    def d(x, y):
        qx, qy = abs(x - cx) - hw + r, abs(y - cy) - hh + r
        return math.hypot(max(qx, 0), max(qy, 0)) + min(max(qx, qy), 0) - r
    return d


def capsule(ax, ay, bx, by, r):
    def d(x, y):
        px, py, vx, vy = x - ax, y - ay, bx - ax, by - ay
        h = max(0.0, min(1.0, (px * vx + py * vy) / (vx * vx + vy * vy)))
        return math.hypot(px - vx * h, py - vy * h) - r
    return d


def polygon(*points):
    """A convex polygon (in either winding)."""
    edges = []
    area = sum(points[i][0] * points[(i + 1) % len(points)][1] - points[(i + 1) % len(points)][0] * points[i][1]
               for i in range(len(points)))
    sign = 1.0 if area > 0 else -1.0
    for i in range(len(points)):
        (x0, y0), (x1, y1) = points[i], points[(i + 1) % len(points)]
        length = math.hypot(x1 - x0, y1 - y0)
        edges.append((sign * (y1 - y0) / length, -sign * (x1 - x0) / length, x0, y0))   # outward normal

    def d(x, y):
        return max((x - x0) * nx + (y - y0) * ny for nx, ny, x0, y0 in edges)
    return d


def union(*shapes):
    return lambda x, y: min(s(x, y) for s in shapes)


def subtract(a, *holes):
    return lambda x, y: max(a(x, y), *(-h(x, y) for h in holes))


def ring(cx, cy, r, thickness):
    return lambda x, y: abs(math.hypot(x - cx, y - cy) - r) - thickness / 2


def oval(cx, cy, rx, ry, thickness):
    """An elliptical ring (the distance is only roughly right away from the line, which is all a ring needs)."""
    return lambda x, y: abs(math.hypot((x - cx) / rx, (y - cy) / ry) - 1) * min(rx, ry) - thickness / 2


# --- the pictograms, one per clip (a generic thing of the kind of the service, not its logo) ---------------

GLYPHS = {
    # an open book: two pages with lines of text cut out of them
    'book': lambda: subtract(
        union(box(0.34, 0.5, 0.155, 0.23, 0.03), box(0.66, 0.5, 0.155, 0.23, 0.03)),
        *[box(0.34, y, 0.09, 0.012, 0.01) for y in (0.40, 0.48, 0.56)],
        *[box(0.66, y, 0.09, 0.012, 0.01) for y in (0.40, 0.48, 0.56)]),
    # a speech bubble with a tail
    'bubble': lambda: union(box(0.5, 0.45, 0.27, 0.20, 0.07), polygon((0.36, 0.60), (0.52, 0.60), (0.34, 0.77))),
    # a room: a ring with a small ring inside
    'room': lambda: union(ring(0.5, 0.5, 0.26, 0.075), circle(0.5, 0.5, 0.075), polygon((0.66, 0.66), (0.80, 0.80), (0.60, 0.74))),
    # a map pin
    'pin': lambda: subtract(
        union(circle(0.5, 0.40, 0.21), polygon((0.30, 0.50), (0.70, 0.50), (0.5, 0.83))), circle(0.5, 0.40, 0.085)),
    # a pencil over a line
    'pencil': lambda: union(
        capsule(0.34, 0.62, 0.66, 0.30, 0.065), polygon((0.26, 0.70), (0.31, 0.57), (0.39, 0.65)),
        box(0.62, 0.79, 0.20, 0.018, 0.012)),
    # a video camera
    'camera': lambda: union(box(0.43, 0.5, 0.23, 0.17, 0.045), polygon((0.69, 0.5), (0.82, 0.36), (0.82, 0.64))),
    # a cloud
    'cloud': lambda: union(circle(0.40, 0.52, 0.14), circle(0.57, 0.46, 0.18), circle(0.67, 0.58, 0.115),
                           box(0.50, 0.63, 0.22, 0.075, 0.07)),
    # a globe: a ring, an equator, a meridian and the two parallels
    'globe': lambda: union(ring(0.5, 0.5, 0.27, 0.065), box(0.5, 0.5, 0.27, 0.016), oval(0.5, 0.5, 0.125, 0.27, 0.04),
                           box(0.5, 0.35, 0.22, 0.012), box(0.5, 0.65, 0.22, 0.012)),
}


def hexcolour(text):
    return tuple(int(text[i:i + 2], 16) for i in (1, 3, 5))


def render(glyph, top, bottom):
    """SIZE x SIZE rows of RGB bytes: a vertical gradient with the pictogram in white, with a soft shadow."""
    shape = GLYPHS[glyph]()
    pixel = 1.0 / SIZE
    rows = []
    for j in range(SIZE):
        y = (j + 0.5) / SIZE
        t = j / (SIZE - 1)
        base = [top[k] + (bottom[k] - top[k]) * t for k in range(3)]
        row = bytearray()
        for i in range(SIZE):
            x = (i + 0.5) / SIZE
            shadow = max(0.0, min(1.0, 0.5 - (shape(x - 0.006, y - 0.012)) / (2.5 * pixel))) * 0.22
            inside = max(0.0, min(1.0, 0.5 - shape(x, y) / pixel))
            for k in range(3):
                value = base[k] * (1 - shadow)
                value = value + (255 - value) * inside
                row.append(int(value + 0.5))
        rows.append(bytes(row))
    return rows


def png(rows):
    """A plain 8-bit RGB PNG: the same bytes for the same rows (no ancillary chunks, fixed compression)."""
    def chunk(kind, data):
        body = kind + data
        return struct.pack('>I', len(data)) + body + struct.pack('>I', zlib.crc32(body) & 0xffffffff)
    raw = b''.join(b'\x00' + row for row in rows)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', SIZE, SIZE, 8, 2, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def clip_uuid(clip_id):
    return str(uuid.uuid5(NAMESPACE, clip_id)).upper()


def load(manifest, only=None):
    with open(manifest, encoding='utf-8') as f:
        clips = json.load(f)['clips']
    seen = set()
    for c in clips:
        for key in ('id', 'title', 'url', 'glyph', 'bg'):
            if key not in c:
                sys.exit('mk-webclips.py: %s: a clip without "%s": %r' % (manifest, key, c))
        if c['id'] in seen:
            sys.exit('mk-webclips.py: %s: the id "%s" twice' % (manifest, c['id']))
        seen.add(c['id'])
        if not c['url'].startswith('https://'):
            sys.exit('mk-webclips.py: %s: the address of "%s" is not https' % (manifest, c['id']))
        if c['glyph'] not in GLYPHS:
            sys.exit('mk-webclips.py: %s: no pictogram "%s" (have %s)' % (manifest, c['glyph'], ', '.join(sorted(GLYPHS))))
    if only is not None:
        missing = set(only) - seen
        if missing:
            sys.exit('mk-webclips.py: no such clip: %s' % ', '.join(sorted(missing)))
        clips = [c for c in clips if c['id'] in only]
    return clips


def info(clip, fullscreen):
    """What Safari writes for a clip, less what only a page's own settings give."""
    return plistlib.dumps({
        'Title': clip['title'],
        'URL': clip['url'],
        'FullScreen': bool(clip.get('fullscreen', fullscreen)),
        'IconIsPrecomposed': True,
        'IconIsScreenShotBased': False,
        'ClassicMode': False,
        'IsAppClip': False,
        'ScenelessWebClip': False,
        'Orientations': 0,
    }, fmt=plistlib.FMT_XML, sort_keys=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--out')
    ap.add_argument('--manifest', default=DEFAULT_MANIFEST)
    ap.add_argument('--only')
    ap.add_argument('--fullscreen', action='store_true')
    ap.add_argument('--flat', action='store_true')
    ap.add_argument('--skel')
    ap.add_argument('--list', action='store_true')
    args = ap.parse_args()
    clips = load(args.manifest, set(args.only.split(',')) if args.only else None)
    if args.list:
        for c in clips:
            print(clip_uuid(c['id']), c['id'], c['title'], c['url'])
        return
    if not args.out:
        ap.error('--out is required')
    base = args.out if args.flat else os.path.join(args.out, *CLIPS_IN_VAR)
    skel = []
    for c in clips:
        name = clip_uuid(c['id']) + '.webclip'
        directory = os.path.join(base, name)
        os.makedirs(directory, exist_ok=True)
        with open(os.path.join(directory, 'Info.plist'), 'wb') as f:
            f.write(info(c, args.fullscreen))
        with open(os.path.join(directory, 'icon.png'), 'wb') as f:
            f.write(png(render(c['glyph'], hexcolour(c['bg'][0]), hexcolour(c['bg'][1]))))
        skel.append('d\t755\t501\t501\t/private/var/%s' % '/'.join(CLIPS_IN_VAR + (name,)))
    if args.skel:
        with open(args.skel, 'w') as f:
            f.write('\n'.join(skel) + '\n')
    print('mk-webclips.py: %d web clips in %s' % (len(clips), base))


if __name__ == '__main__':
    main()
