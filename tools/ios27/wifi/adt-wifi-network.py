#!/usr/bin/env python3
"""Give a Starbite STARLTE ADT the Wi-Fi network a TEST image joins at brcmfmac=8: /chosen "hakios-wifi-network".

  adt-wifi-network.py IN.adt OUT.adt NETWORK_FILE

NETWORK_FILE has two lines: the network's name (1-32 bytes) and its WPA2 passphrase (8-63). The ios-27 kernel
(bsd/dev/starlte_wifi.c of the fork's Wi-Fi branch) copies the property at boot, removes it from the registry's
device-tree plane so that no process can read it, and gives it to brcmfmac=8 as scion-starlte's fw/network was given
on 16.5. It is never logged. The ADT and every image made from it then carry the passphrase: keep them on your own
machine, never in a repository or a release (the release root and kernel never have one).
"""
import sys
from pathlib import Path

if len(sys.argv) != 4:
    sys.exit(__doc__)
inp, out, net = sys.argv[1:]
text = Path(net).read_bytes()
lines = text.split(b"\n")
name, passphrase = (lines + [b"", b""])[:2]
name, passphrase = name.rstrip(b"\r"), passphrase.rstrip(b"\r")
if not 1 <= len(name) <= 32 or not 8 <= len(passphrase) <= 63:
    sys.exit("adt-wifi-network: %s: want a name of 1-32 bytes and a passphrase of 8-63 on two lines" % net)
value = name + b"\n" + passphrase + b"\n"

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "herolte"))
import adt  # noqa: E402

root, _ = adt.parse(Path(inp).read_bytes())
chosen = next(c for c in root.children if c.name == "chosen")
chosen.props = [p for p in chosen.props if p[0] != "hakios-wifi-network"]
chosen.props.append(("hakios-wifi-network", 0, value))
Path(out).write_bytes(adt.serialize(root))
Path(out).chmod(0o600)
print("adt-wifi-network: %s: /chosen hakios-wifi-network, a name of %d bytes and a passphrase of %d" % (out, len(name), len(passphrase)))
