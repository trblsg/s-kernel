# a64e-rewrite every arm64e Mach-O on a mounted iOS root (outside the dyld caches)
import os, sys, struct, importlib.util
spec = importlib.util.spec_from_file_location('a64e', sys.argv[1]); m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
root = sys.argv[2]; dry = '--dry-run' in sys.argv
seen = set(); n = 0; tot = {}; errs = 0
skipdirs = ('com.apple.dyld',)
for d, dirs, files in os.walk(root):
    dirs[:] = [x for x in dirs if x not in skipdirs and not (d == root and x in ('dev', 'private') and False)]
    if '/private/var' in d: continue
    for f in files:
        p = os.path.join(d, f)
        try:
            st = os.lstat(p)
            if not os.path.isfile(p) or os.path.islink(p) or st.st_size < 4096: continue
            if (st.st_dev, st.st_ino) in seen: continue
            with open(p, 'rb') as fh: h = fh.read(4)
            if h not in (b'\xcf\xfa\xed\xfe', b'\xca\xfe\xba\xbe', b'\xca\xfe\xba\xbf'): continue
            seen.add((st.st_dev, st.st_ino))
            if not dry and not os.access(p, os.W_OK): os.chmod(p, st.st_mode | 0o200)
            k, t = m.do_macho(p, dry)
            if k:
                n += 1; m.add(tot, t)
        except Exception as e:
            errs += 1; print('ERR', p, e, file=sys.stderr)
print(n, 'files rewritten', tot, 'errors', errs)
