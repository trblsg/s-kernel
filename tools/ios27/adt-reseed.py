#!/usr/bin/env python3
"""Give an ADT's /chosen a fresh random-seed of N bytes (default 256).

xnu-12377 asks the boot loader for 4 * SHA-512 = 256 seed bytes and panics
("Expected 256 seed bytes from bootloader") with fewer; mkadt.py made 64.
  tools/ios27/adt-reseed.py out/starlte-ipad7-phone.adt [N]   (rewrites the file)"""
import os, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'herolte'))
import adt  # noqa: E402

path = sys.argv[1]
n = int(sys.argv[2]) if len(sys.argv) > 2 else 256
root, end = adt.parse(open(path, 'rb').read())
chosen = next(c for c in root.children if c.name == 'chosen')
for i, (name, flags, val) in enumerate(chosen.props):
    if name == 'random-seed':
        old = len(val)
        chosen.props[i] = (name, flags, os.urandom(n))
        break
else:
    raise SystemExit('no /chosen/random-seed')
open(path, 'wb').write(adt.serialize(root))
print(f'{path}: random-seed {old} -> {n} bytes')
