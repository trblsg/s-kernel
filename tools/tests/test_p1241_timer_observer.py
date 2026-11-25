import runpy
from pathlib import Path
import sys

root=Path(__file__).resolve().parents[2]
m=runpy.run_path(str(root/"xnu/p1241-timer-observer.py"))
for key,marker in (("ARM_EDITS","P1241_TIMER_OBSERVER"),("RTC_EDITS","P1241_POP_OBSERVER")):
    edits=m[key]
    original="\n".join(old for old,new in edits)
    patched=m["patch"](original,edits,marker)
    assert patched.count(marker)==1
    assert m["patch"](patched,edits,marker)==patched
    restored=patched
    for old,new in reversed(edits):
        restored=restored.replace(new,old,1)
    assert restored==original
    for bad in (original+original, original.replace(edits[-1][0],""),patched.replace(edits[-1][1],"")):
        try:
            m["patch"](bad,edits,marker)
        except ValueError:
            pass
        else:
            raise AssertionError("bad pinned timer source accepted")
if len(sys.argv)==3:
    for path,key,marker in ((sys.argv[1],"ARM_EDITS","P1241_TIMER_OBSERVER"),(sys.argv[2],"RTC_EDITS","P1241_POP_OBSERVER")):
        actual=Path(path).read_text()
        out=m["patch"](actual,m[key],marker)
        assert m["patch"](out,m[key],marker)==out
        assert out.count(marker)==1
print("P12.4.1 timer observer exact anchors, idempotence, unchanged original operations PASS")
