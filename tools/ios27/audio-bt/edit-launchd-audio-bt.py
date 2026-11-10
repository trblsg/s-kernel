#!/usr/bin/env python3
"""edit-launchd-audio-bt.py ROOT [OS_LAUNCHD_PLIST]: put the audio server and Bluetooth jobs back into an iOS 27 root's
/System/Library/xpc/launchd.plist for the Galaxy S9 (Scion, private iOS 27 port), idempotently:
  com.apple.audiomxd   as Apple's (it hosts CoreAudio's HAL, which loads HakiosAudio.driver and BTAudioHALPlugin.driver)
  com.apple.bluetoothd started through /usr/libexec/hakios-inject with /usr/lib/hakios.bluetoothd.dylib, and
                       BT_HCI_TRANSPORT=H4:POSIX:com.apple.uart.bluetooth,4000000 (the kernel's bt=7 UART control)
  com.apple.BlueTool   started through hakios-inject with /usr/lib/hakios.bluetool.dylib (ReleaseType Vendor: our scripts)
A job missing from launchd.plist is taken from launchd-disabled.plist beside it (where the S9's tune scripts keep the jobs
they take out), else from OS_LAUNCHD_PLIST (the IPSW's own launchd.plist)."""
import os, plistlib, sys
root = sys.argv[1]
LOG = os.environ.get('AUDIO_BT_LOG') == '1'   # test runs: stderr and os_log mirrored into /private/var/local/oslog/
xpc = root + '/System/Library/xpc/'
d = plistlib.load(open(xpc + 'launchd.plist', 'rb'))
try:
    kept = plistlib.load(open(xpc + 'launchd-disabled.plist', 'rb'))
except FileNotFoundError:
    kept = None
osj = plistlib.load(open(sys.argv[2], 'rb'))['LaunchDaemons'] if len(sys.argv) > 2 else {}
jobs = d['LaunchDaemons']
INJ = '/usr/libexec/hakios-inject'
want = {
    '/System/Library/LaunchDaemons/com.apple.audiomxd.plist': None,
    '/System/Library/LaunchDaemons/com.apple.bluetoothd.plist': '/usr/lib/hakios.bluetoothd.dylib',
    '/System/Library/LaunchDaemons/com.apple.BlueTool.plist': '/usr/lib/hakios.bluetool.dylib',
}
for key, lib in want.items():
    src = 'launchd.plist'
    if key not in jobs:
        if kept is not None and key in kept:
            jobs[key] = kept.pop(key); src = 'launchd-disabled.plist'
        elif key in osj:
            jobs[key] = osj[key]; src = 'the IPSW'
        else:
            sys.exit(f'{key}: in neither launchd-disabled.plist nor the IPSW launchd.plist given')
    job = jobs[key]
    if lib is not None:
        args = job['ProgramArguments']
        if args[0] != INJ:
            job['ProgramArguments'] = [INJ, lib] + args
    if key.endswith('bluetoothd.plist'):
        job.setdefault('EnvironmentVariables', {})['BT_HCI_TRANSPORT'] = 'H4:POSIX:com.apple.uart.bluetooth,4000000'
    job.pop('Disabled', None)
    if LOG:
        name = job['Label'].split('.')[-1]
        job['StandardErrorPath'] = f'/private/var/local/oslog/{name}.err'
        job['StandardOutPath'] = f'/private/var/local/oslog/{name}.out'
        job.setdefault('EnvironmentVariables', {})['OS_ACTIVITY_DT_MODE'] = 'YES'
    print(f'{job["Label"]}: from {src}: {" ".join(job["ProgramArguments"])}')
with open(xpc + 'launchd.plist', 'r+b') as f:
    f.truncate(0); plistlib.dump(d, f, fmt=plistlib.FMT_BINARY)
if kept is not None:
    with open(xpc + 'launchd-disabled.plist', 'wb') as f:
        plistlib.dump(kept, f, fmt=plistlib.FMT_BINARY)
