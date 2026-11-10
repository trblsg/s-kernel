#!/usr/bin/env python3
"""run-all.py [--list files284.txt] [--ref ref] [--stage stage] [--mode auto|A|T]  -> stage/<abs path> + manifest.json
Runs macho-rewrite over every listed file (reference copies under ref/<abs path>)."""
import sys, os, json, hashlib, importlib.util, subprocess, re, argparse, time, shutil

HERE = os.path.dirname(os.path.abspath(__file__))
sp = importlib.util.spec_from_file_location('macho_rewrite', os.path.join(HERE, 'macho-rewrite.py'))
mr = importlib.util.module_from_spec(sp)
sp.loader.exec_module(mr)


def cs(path):
    r = subprocess.run(['codesign', '-dvvv', path], capture_output=True, text=True)
    t = r.stderr + r.stdout
    cd = re.search(r'^CDHash=(\S+)', t, re.M)
    ident = re.search(r'^Identifier=(\S+)', t, re.M)
    return (cd.group(1) if cd else None), (ident.group(1) if ident else None)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--list', default=os.path.join(HERE, 'files284.txt'))
    ap.add_argument('--ref', default=os.path.join(HERE, 'ref'))
    ap.add_argument('--stage', default=os.path.join(HERE, 'stage'))
    ap.add_argument('--mode', default='auto', choices=['auto', 'A', 'T'])
    ap.add_argument('--out', default=os.path.join(HERE, 'manifest.json'))
    a = ap.parse_args()
    if a.mode == 'A':
        mr.text_pad_region = lambda sl: None
    shutil.rmtree(a.stage, ignore_errors=True)
    files = [l.strip() for l in open(a.list) if l.strip()]
    man = {'tool': 'macho-rewrite.py', 'mode_arg': a.mode, 'files': {}}
    tot = {'files': len(files), 'staged': 0, 'failed': 0, 'sites': 0, 'by_class': {}, 'left': 0, 'stub_bytes': 0, 'modes': {}}
    t0 = time.time()
    for i, p in enumerate(files):
        src = a.ref + p
        d = open(src, 'rb').read()
        e = {'sha256_before': hashlib.sha256(d).hexdigest(), 'size_before': len(d)}
        try:
            nb, infos = mr.rewrite_file(d, a.mode)
        except Exception as ex:
            nb, infos = d, [{'mode': 'failed', 'why': 'exception %r' % ex}]
        e['slices'] = infos
        modes = [x['mode'] for x in infos if x['mode'] != 'skip']
        e['mode'] = '+'.join(sorted(set(modes))) if modes else 'skip'
        e['sites'] = sum(x.get('sites_total', 0) for x in infos)
        cl = {}
        for x in infos:
            for k, v in x.get('classes', {}).items():
                cl[k] = cl.get(k, 0) + v
        e['sites_by_class'] = cl
        e['left'] = sum(len(x.get('left', [])) for x in infos)
        e['stub_bytes'] = sum(x.get('stub_bytes', 0) for x in infos)
        if any(m == 'failed' for m in modes):
            e['status'] = 'failed'
            tot['failed'] += 1
        elif nb == d:
            e['status'] = 'unchanged'
        else:
            e['status'] = 'staged'
            dst = a.stage + p
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            open(dst, 'wb').write(nb)
            os.chmod(dst, os.stat(src).st_mode & 0o7777)
            e['sha256_after'] = hashlib.sha256(nb).hexdigest()
            e['size_after'] = len(nb)
            e['cdhash_before'], e['identifier_before'] = cs(src)
            e['cdhash_after'], e['identifier_after'] = cs(dst)
            tot['staged'] += 1
        man['files'][p] = e
        tot['sites'] += e['sites']
        tot['left'] += e['left']
        tot['stub_bytes'] += e['stub_bytes']
        for k, v in cl.items():
            tot['by_class'][k] = tot['by_class'].get(k, 0) + v
        tot['modes'][e['mode']] = tot['modes'].get(e['mode'], 0) + 1
        if i % 20 == 0:
            print(i, '/', len(files), p, e['mode'], e['sites'], '%.0fs' % (time.time() - t0), flush=True)
    man['totals'] = tot
    json.dump(man, open(a.out, 'w'), indent=1, default=str)
    print(json.dumps(tot, indent=1))


if __name__ == '__main__':
    main()
