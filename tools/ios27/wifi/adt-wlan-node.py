#!/usr/bin/env python3
"""Give a Starbite STARLTE ADT the device-tree node iOS's Wi-Fi daemon is gated on: /arm-io/wlan.

  adt-wlan-node.py IN.adt OUT.adt

iOS 27's /System/Library/LaunchDaemons/com.apple.wifid.plist has `_LimitLoadToDeviceTree` = wlan (or marconi-wifi,
centauri-alpha, ...): launchd starts wifid only on a device whose device tree has such a node. The S9's ADT
(tools/ios27/mkadt_starlte.py) has none, so on 10 Nov 2026 (phone 2, Wi-Fi joined and pinging in the kernel) wifid was
never started and Settings showed Wi-Fi off. The node also is where drivers/wifi/hakios.wlan.cpp attaches its
IO80211Controller and puts the radio's address as local-mac-address (MobileGestalt reads it there); until the radio is up
it holds the synthetic address of /chosen mac-address-wifi0.

Opt-in (Wi-Fi test images) until a phone run shows wifid on it is harmless: with the node and no Wi-Fi driver up, wifid
runs and finds no interface.
"""
import sys
from pathlib import Path

if len(sys.argv) != 3:
    sys.exit(__doc__)
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "herolte"))
import adt  # noqa: E402


def cstr(s):
    return s.encode() + b"\0"


root, _ = adt.parse(Path(sys.argv[1]).read_bytes())
chosen = next(c for c in root.children if c.name == "chosen")
arm_io = next(c for c in root.children if c.name == "arm-io")
mac = chosen.prop("mac-address-wifi0") or bytes((2, 0x53, 0x54, 0x41, 0x52, 1))
arm_io.children = [c for c in arm_io.children if c.name != "wlan"]
wlan = adt.Node()
wlan.props = [
    ("name", 0, cstr("wlan")),
    ("device_type", 0, cstr("wlan")),
    ("compatible", 0, cstr("wlan-pcie,bcm4361") + cstr("wlan-pcie,starlte")),
    ("local-mac-address", 0, mac),
]
arm_io.children.append(wlan)
Path(sys.argv[2]).write_bytes(adt.serialize(root))
print("adt-wlan-node: %s: /arm-io/wlan (compatible wlan-pcie,bcm4361; local-mac-address from /chosen mac-address-wifi0)" % sys.argv[2])
