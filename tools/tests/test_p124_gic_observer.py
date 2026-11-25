import runpy
from pathlib import Path

root=Path(__file__).resolve().parents[2]
module=runpy.run_path(str(root/"xnu/p124-gic-observer.py"))
patch=module["patch"]
anchor=module["ANCHOR"]
decl=module["DECL"]
original=decl+" int source)\n{\n"+anchor+"\t\tif (intid == GIC_INTID_VTIMER) {}\n}\n"
out=patch(original)
assert out.count(module["MARKER"])==1
assert patch(out)==out
assert out.replace(module["PROTOTYPE"],"").replace(module["CALL"],anchor)==original
for bad in (original.replace(anchor,""),original+original, out.replace(module["PROTOTYPE"],"")):
    try:
        patch(bad)
    except ValueError:
        pass
    else:
        raise AssertionError("ambiguous or damaged GIC source accepted")
print("P12.4 GIC observer exact-anchor, idempotence and unchanged IRQ dispatch PASS")
