#!/usr/bin/env python3
"""usb-console.py (serial SERIAL | loc LOCATIONID) : which USB console device belongs to which phone.
  serial S -> prints the IOKit locationID of the USB device whose 'USB Serial Number' is S (the phone in TWRP: adb serial)
  loc L    -> prints /dev/cu.* device(s) below the USB device at locationID L (the HakiOS console after the phone booted iOS)
The locationID is the physical port, so it is the same before and after the reboot."""
import plistlib, subprocess, sys
def devices():
    out = subprocess.run(['ioreg', '-a', '-r', '-c', 'IOUSBHostDevice', '-l'], capture_output=True).stdout
    return plistlib.loads(out) if out.strip() else []
def walk(n):
    yield n
    for c in n.get('IORegistryEntryChildren', []): yield from walk(c)
mode, arg = sys.argv[1:3]
for d in devices():
    if mode == 'serial' and d.get('USB Serial Number') == arg: print(d.get('locationID'))
    if mode == 'loc' and d.get('locationID') == int(arg, 0):
        for n in walk(d):
            if 'IOCalloutDevice' in n: print(n['IOCalloutDevice'])
