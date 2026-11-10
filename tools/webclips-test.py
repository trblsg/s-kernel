#!/usr/bin/env python3
# SPDX-License-Identifier: MIT-0
"""Check tools/mk-webclips.py and userland/webclips/manifest.json without a phone, an IPSW or a network.

usage: tools/webclips-test.py        exit 1 and the list of what failed (CI runs it)

What it holds to:
  - the manifest parses; ids are unique; every address is https; every pictogram exists;
  - a run writes, for each clip, a <UUID>.webclip with an Info.plist that plistlib reads back (title, address,
    FullScreen false unless asked) and an icon.png that is a 180 x 180 8-bit RGB PNG with a valid CRC and a zlib
    stream that inflates to exactly that many bytes;
  - two runs write the same bytes (an image built twice is the same image);
  - --flat and --only give the shape the Sileo repository's packages are built from;
  - the skeleton lines are `d 755 501 501 /private/var/mobile/Library/WebClips/<UUID>.webclip`, one per clip, and the
    skeleton of the data volume with the clips' files in it stays inside what init reads (src/init.c reads 16384
    bytes of etc/var.skel; build.sh refuses 15000), counted on the worst case of the template's own size, 7000.
Every check prints its own line, and the total counts what ran, so that a check over nothing cannot pass.
"""
import hashlib
import json
import os
import plistlib
import re
import struct
import subprocess
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
TOOL = os.path.join(HERE, 'mk-webclips.py')
MANIFEST = os.path.join(ROOT, 'userland', 'webclips', 'manifest.json')
TEMPLATE_SKEL_WORST = 7000      # etc/var.skel of the system image is about 6000 bytes before ours (measured on a built image)
SKEL_LIMIT = 15000

ran = 0
failed = []


def check(ok, what):
    global ran
    ran += 1
    print(('ok    ' if ok else 'FAIL  ') + what)
    if not ok:
        failed.append(what)


def run(*args):
    return subprocess.run([sys.executable, '-I', TOOL] + list(args), capture_output=True, text=True)


def tree_hash(path):
    h = hashlib.sha256()
    for dirpath, dirnames, filenames in os.walk(path):
        dirnames.sort()
        for name in sorted(filenames):
            full = os.path.join(dirpath, name)
            h.update(os.path.relpath(full, path).encode() + b'\0' + open(full, 'rb').read())
    return h.hexdigest()


def png_ok(data):
    if not data.startswith(b'\x89PNG\r\n\x1a\n'):
        return False
    pos, idat, header, end = 8, b'', None, False
    while pos < len(data):
        n = struct.unpack('>I', data[pos:pos + 4])[0]
        kind, body = data[pos + 4:pos + 8], data[pos + 8:pos + 8 + n]
        if struct.unpack('>I', data[pos + 8 + n:pos + 12 + n])[0] != zlib.crc32(kind + body) & 0xffffffff:
            return False
        if kind == b'IHDR':
            header = struct.unpack('>IIBBBBB', body)
        elif kind == b'IDAT':
            idat += body
        elif kind == b'IEND':
            end = True
        pos += 12 + n
    return (end and header == (180, 180, 8, 2, 0, 0, 0) and len(zlib.decompress(idat)) == 180 * (1 + 180 * 3))


def main():
    manifest = json.load(open(MANIFEST, encoding='utf-8'))
    clips = manifest['clips']
    check(len(clips) > 0, 'the manifest has clips (%d)' % len(clips))
    check(len({c['id'] for c in clips}) == len(clips), 'ids are unique')
    check(all(c['url'].startswith('https://') for c in clips), 'every address is https')
    check(all(re.fullmatch(r'#[0-9a-f]{6}', colour) for c in clips for colour in c['bg']), 'every colour is #rrggbb')

    listing = run('--list')
    check(listing.returncode == 0 and len(listing.stdout.strip().splitlines()) == len(clips), '--list names every clip')

    with tempfile.TemporaryDirectory() as tmp:
        a, b, skel = os.path.join(tmp, 'a'), os.path.join(tmp, 'b'), os.path.join(tmp, 'skel')
        r = run('--out', a, '--skel', skel)
        check(r.returncode == 0, 'a run succeeds: %s' % (r.stderr.strip() or r.stdout.strip()))
        base = os.path.join(a, 'mobile', 'Library', 'WebClips')
        found = sorted(os.listdir(base)) if os.path.isdir(base) else []
        check(len(found) == len(clips) and all(re.fullmatch(r'[0-9A-F-]{36}\.webclip', n) for n in found),
              'one <UUID>.webclip for each clip (%d)' % len(found))
        info_ok = icon_ok = 0
        titles = set()
        for name in found:
            try:
                info = plistlib.loads(open(os.path.join(base, name, 'Info.plist'), 'rb').read())
                info_ok += info.get('FullScreen') is False and info['URL'].startswith('https://') and bool(info['Title'])
                titles.add(info['Title'])
            except Exception:
                pass
            icon_ok += png_ok(open(os.path.join(base, name, 'icon.png'), 'rb').read())
        check(info_ok == len(clips), 'every Info.plist reads back, with FullScreen false (%d of %d)' % (info_ok, len(clips)))
        check(icon_ok == len(clips), 'every icon.png is a 180 x 180 8-bit RGB PNG (%d of %d)' % (icon_ok, len(clips)))
        check(len(titles) == len(clips), 'titles differ (%d)' % len(titles))

        lines = open(skel).read().splitlines() if os.path.exists(skel) else []
        check(len(lines) == len(clips) and all(re.fullmatch(
            r'd\t755\t501\t501\t/private/var/mobile/Library/WebClips/[0-9A-F-]{36}\.webclip', line) for line in lines),
            'one skeleton line for each clip')
        files = 2 * len(clips)
        per_file = len('f\t644\t501\t501\t/private/var/mobile/Library/WebClips/' + 'X' * 36 + '.webclip/Info.plist\t'
                       '/var/mobile/Library/WebClips/' + 'X' * 36 + '.webclip/Info.plist') + 1
        worst = TEMPLATE_SKEL_WORST + sum(len(line) + 1 for line in lines) + files * per_file
        check(worst < SKEL_LIMIT, 'the clips fit in etc/var.skel: %d bytes of the %d build.sh allows' % (worst, SKEL_LIMIT))

        r = run('--out', b)
        check(r.returncode == 0 and tree_hash(a) == tree_hash(b), 'two runs write the same bytes')

        flat = os.path.join(tmp, 'flat')
        r = run('--out', flat, '--flat', '--only', clips[0]['id'], '--fullscreen')
        names = os.listdir(flat) if os.path.isdir(flat) else []
        check(r.returncode == 0 and len(names) == 1 and names[0].endswith('.webclip'), '--flat --only gives one clip directly in DIR')
        if names:
            info = plistlib.loads(open(os.path.join(flat, names[0], 'Info.plist'), 'rb').read())
            check(info['FullScreen'] is True, '--fullscreen sets FullScreen')
        bad = run('--out', os.path.join(tmp, 'x'), '--only', 'no-such-clip')
        check(bad.returncode != 0, 'an unknown --only is refused')

    print('%d checks run, %d failed' % (ran, len(failed)))
    return 1 if failed or ran < 10 else 0


if __name__ == '__main__':
    sys.exit(main())
