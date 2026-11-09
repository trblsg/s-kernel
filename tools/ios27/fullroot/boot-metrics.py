#!/usr/bin/env python3
"""boot-metrics.py LOG [--from-line N] : boot summary of a console capture. LOG lines are 'EPOCH text' (tools/verify-boot.sh) or plain text.
Prints: kernel init done, first frame, SpringBoard/backboardd exec/exit counts, crash/panic/jetsam lines, STARLTE TOP CPU (all cores, %) in windows,
daemon crash-loop table (top 15 by CRASH/EXIT count)."""
import collections, re, statistics, sys
path = sys.argv[1]; start = int(sys.argv[sys.argv.index('--from-line') + 1]) if '--from-line' in sys.argv else 0
rows = []
for i, l in enumerate(open(path, 'rb').read().decode('latin1').split('\n')):
    if i < start: continue
    l = l.rstrip('\r')
    m = re.match(r'(\d{10}\.\d+) (.*)$', l)
    rows.append((float(m.group(1)), m.group(2)) if m else (None, l))
t0 = next((t for t, l in rows if t), None)
def first(rx):
    for t, l in rows:
        if re.search(rx, l): return t, l
    return None, None
def rel(t): return '%.1f s' % (t - t0) if (t and t0) else 'n/a'
tb, _ = first(r'SBBOOT \d+ ms: bsd_init: done'); print('log start (first line):', t0)
print('kernel bsd_init done:', rel(tb))
tf, lf = first(r'A5XELTE FB: frames=[1-9]'); print('first frame presented:', rel(tf), (lf or '')[:100])
for name in ('SpringBoard', 'backboardd', 'Preferences', 'configd', 'lsd', 'runningboardd'):
    ex = sum(1 for t, l in rows if re.search(r'SWEET LIMITS: %s\b' % name, l))
    ee = [l for t, l in rows if re.search(r'SWEET EXIT: %s\[' % name, l)]
    print(f'{name:14s} spawns logged {ex:3d}  exits logged {len(ee):3d}', (ee[0][:100] if ee else ''))
cnt = collections.Counter(); pan = []
for t, l in rows:
    m = re.search(r'CRASH[^A-Za-z]*([A-Za-z0-9_.\-]+)', l) if 'CRASH' in l else None
    if m: cnt['CRASH ' + m.group(1)] += 1
    m = re.search(r'SWEET EXIT: ([^\[ ]+)\[\d+\] signal (\d+)', l)
    if m: cnt['EXIT %s sig%s' % m.groups()] += 1
    if re.search(r'panic|Panic|PANIC', l) and 'PanicOn' not in l: pan.append(l[:140])
print('panic lines:', len(pan), pan[:3])
print('crash/exit table:', cnt.most_common(15))
tops = []
for t, l in rows:
    m = re.search(r'STARLTE TOP \(10 s, all (\d+)%\): (.*)', l)
    if m: tops.append((t, int(m.group(1)), m.group(2)[:110]))
if tops:
    vals = [v for _, v, _ in tops]; print(f'STARLTE TOP samples {len(tops)}: first {vals[:3]} median {statistics.median(vals):.0f} last10 median {statistics.median(vals[-10:]):.0f}')
    for t, v, s in tops[-3:]: print('  ', rel(t), v, s)
mem = [l for t, l in rows if 'cpufreq_gov:' in l and ' s: lit' in l]
print('last cpufreq_gov:', (mem[-1][:200] if mem else None))
