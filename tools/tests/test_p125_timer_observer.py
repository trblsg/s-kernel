import runpy
from pathlib import Path
root=Path(__file__).resolve().parents[2]
m=runpy.run_path(str(root/"xnu/p125-timer-observer.py"))
for file,edits in m["EDITS"].items():
    original="\n/* fixture separator */\n".join(old for old,new in edits)
    patched=m["patch"](original,edits)
    assert m["patch"](patched,edits)==patched, file
    restored=patched
    for old,new in reversed(edits):
        restored=restored.replace(new,old,1)
    assert restored==original, file
    try:
        m["patch"](original.replace(edits[-1][0],"",1),edits)
    except ValueError:
        pass
    else:
        raise AssertionError(file+" accepted missing anchor")
print("P12.5 overlay exact anchors and round-trip PASS")
