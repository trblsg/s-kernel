#!/usr/bin/env python3
"""scripted-ui.py LOCATIONID T0_EPOCH 'SECONDS:console command' ... : write HakiOS console commands (swipe/touch/button, thousandths of the screen)
to the console of the phone at USB port LOCATIONID at T0+SECONDS (the device is looked up again for every command: it re-enumerates after boot)."""
import subprocess, sys, time, os
here = os.path.dirname(os.path.abspath(__file__))
loc, t0 = sys.argv[1], float(sys.argv[2])
for spec in sys.argv[3:]:
    s, cmd = spec.split(':', 1)
    while time.time() < t0 + float(s): time.sleep(0.2)
    dev = subprocess.run(['python3', '-I', here + '/usb-console.py', 'loc', loc], capture_output=True, text=True).stdout.split('\n')[0]
    try:
        with open(dev, 'w') as f: f.write(cmd + '\n')
        print('%.1f %s <- %s' % (time.time() - t0, dev, cmd), flush=True)
    except Exception as e:
        print('FAILED', cmd, e, flush=True)
