from pathlib import Path
import runpy
p = runpy.run_path(str(Path(__file__).resolve().parents[1] / 'p127-jiffies-header.py'))
old, new, patch = p['old'], p['new'], p['patch']
source = '#ifndef J\n#define J\n' + old + '\n#endif\n'
out = patch(source)
assert out == source.replace(old, new)
assert patch(out) == out
for bad in ('', old+'\n'+old, old+'\n#define LX_P127_JIFFIES 1'):
    try:
        patch(bad)
    except ValueError:
        pass
    else:
        raise AssertionError('source drift accepted')
print('P12.7 jiffies overlay exact/idempotent/drift guards PASS')
