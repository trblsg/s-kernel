#!/usr/bin/env python3
"""SPDX-License-Identifier: MIT-0
fastflash-root.py ADB IMG STATE : write a raw root image to USERDATA through TWRP's adb, 64 MiB chunks pushed raw
(no gzip: USB is faster than the Mac's gzip), zero runs written from /dev/zero on the phone, every chunk read back and
compared (SHA-256). STATE lists finished chunk numbers (same format as starlte-flash-root.py): an interrupted run resumes."""
import hashlib, os, subprocess, sys, tempfile, time
DEV = '/dev/block/by-name/USERDATA'; CHUNK = 64 << 20; MIB = 1 << 20
ADB, IMG, STATE = sys.argv[1], sys.argv[2], sys.argv[3]
def adb(args):
    p = subprocess.run([ADB, *args], capture_output=True, text=True)
    if p.returncode: raise SystemExit('adb %s failed: %s' % (' '.join(args)[:120], p.stderr[:300]))
    return p.stdout
if adb(['get-state']).strip() != 'recovery': raise SystemExit('not in TWRP')
if adb(['shell', "mount | grep -c ' /data ' || true"]).strip() != '0': raise SystemExit('/data is mounted')
size = int(adb(['shell', 'blockdev --getsize64 ' + DEV]).strip())
done = {int(x) for x in open(STATE).read().split()} if os.path.exists(STATE) else set()
state = open(STATE, 'a'); zero = bytes(CHUNK); t0 = time.time(); wrote = zeros = 0
def mark(i):
    state.write('%d\n' % i); state.flush(); done.add(i)
with open(IMG, 'rb') as f, tempfile.TemporaryDirectory() as tmp:
    local = os.path.join(tmp, 'chunk.bin'); index = 0; run = None
    def clear(first, end):
        if all(i in done for i in range(first, end)): return
        adb(['shell', 'dd if=/dev/zero of=%s bs=1048576 seek=%d count=%d conv=fsync 2>/dev/null' % (DEV, first * 64, (end - first) * 64)])
        for i in range(first, end): mark(i)
    while True:
        data = f.read(CHUNK)
        if not data: break
        if len(data) != CHUNK or (index + 1) * CHUNK > size: raise SystemExit('bad size at chunk %d' % index)
        if data == zero:
            run = index if run is None else run; zeros += 1; index += 1; continue
        if run is not None: clear(run, index); run = None
        if index not in done:
            open(local, 'wb').write(data)
            want = hashlib.sha256(data).hexdigest()
            adb(['push', local, '/tmp/chunk.bin'])
            back = adb(['shell', 'dd if=/tmp/chunk.bin of=%s bs=1048576 seek=%d conv=fsync 2>/dev/null; '
                        'dd if=%s bs=1048576 skip=%d count=64 2>/dev/null | sha256sum' % (DEV, index * 64, DEV, index * 64)]).split()[0]
            if back != want: raise SystemExit('chunk %d read-back mismatch' % index)
            mark(index); wrote += 1
            if wrote % 10 == 0: print('chunk %d: %d written, %d zero, %.0f s' % (index, wrote, zeros, time.time() - t0), flush=True)
        index += 1
    if run is not None: clear(run, index)
adb(['shell', 'rm -f /tmp/chunk.bin; sync'])
print('DONE: %d chunks, %d written now, %d zero, %.0f s' % (index, wrote, zeros, time.time() - t0), flush=True)
