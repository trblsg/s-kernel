#!/usr/bin/env python3
# SPDX-License-Identifier: MIT-0
"""Put firmware files into the device tree an ios-27 boot image is packed with.

  adt-firmware.py ADT-IN ADT-OUT FILE [FILE ...]

The ios-27 kernel has no RAM disk beside it, so the chips whose RAM image a driver loads at boot (the Galaxy S9's sensor
hub: drivers/input/ssp/hakios.ssp.c, shub=4 and up) get their file through the device tree: a node /scion-firmware with
one property for each file, the property's name the file's name (at most 31 characters), its value the file's bytes.
The fork's bsd/dev/starlte_fw.c answers the drivers' hakios_image_file("fw", NAME) from it.

The files are their makers' and are in no repository of ours: devices/<device>/tools/fetch-firmware.sh makes them from
your own copy or the pinned public trees and checks them against SHA256SUMS. Give the fetched files here; the output
tree goes to the packing step (mkpayload.py KERNEL ADT-OUT ...) in place of the plain one. Keep images made with it
private, as the firmware's terms are not ours to pass on.

The device tree's format (Apple's flattened tree, as the loader hands it over): a node is <u32 properties, u32 children>,
then its properties, each <char name[32], u32 length (bit 31: a flag), value padded to 4 bytes>, then its children.
"""
import os
import struct
import sys


def parse(blob, at=0):
    nprops, nkids = struct.unpack_from('<II', blob, at)
    at += 8
    props = []
    for _ in range(nprops):
        name = blob[at:at + 32]
        length, = struct.unpack_from('<I', blob, at + 32)
        size = length & 0x7fffffff
        props.append((name, length, blob[at + 36:at + 36 + size]))
        at += 36 + (size + 3 & ~3)
    kids = []
    for _ in range(nkids):
        kid, at = parse(blob, at)
        kids.append(kid)
    return (props, kids), at


def flat(node):
    props, kids = node
    out = struct.pack('<II', len(props), len(kids))
    for name, length, value in props:
        out += name + struct.pack('<I', length) + value + b'\0' * (-len(value) % 4)
    for kid in kids:
        out += flat(kid)
    return out


def prop(name, value):
    raw = name.encode()
    if len(raw) > 31:
        sys.exit('%s: a property name holds at most 31 characters' % name)
    return (raw + b'\0' * (32 - len(raw)), len(value), value)


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    blob = open(sys.argv[1], 'rb').read()
    root, end = parse(blob)
    if end != len(blob):
        sys.exit('%s: %d bytes after the tree' % (sys.argv[1], len(blob) - end))
    for props, _ in root[1]:
        if any(n.rstrip(b'\0') == b'name' and v.rstrip(b'\0') == b'scion-firmware' for n, _, v in props):
            sys.exit('%s already has /scion-firmware' % sys.argv[1])
    props = [prop('name', b'scion-firmware\0')]
    for path in sys.argv[3:]:
        data = open(path, 'rb').read()
        props.append(prop(os.path.basename(path), data))
        print('%s: %d bytes' % (os.path.basename(path), len(data)))
    root[1].append((props, []))
    out = flat(root)
    open(sys.argv[2], 'wb').write(out)
    print('%s: %d bytes (was %d)' % (sys.argv[2], len(out), len(blob)))


if __name__ == '__main__':
    main()
