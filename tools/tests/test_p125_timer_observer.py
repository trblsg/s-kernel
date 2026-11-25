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
# Header export and driver-map are part of the real integration contract.
from tempfile import TemporaryDirectory
anchor=m["EXPORT_ANCHOR"]
entry=m["EXPORT_ENTRY"]
original=anchor+"\tarcade.h \\"+"\n\tworkload_config.h\n"
patched=m["patch_export"](original)
assert patched==m["EXPORT_REPLACEMENT"]+original[len(anchor):]
assert m["patch_export"](patched)==patched
for bad in (original.replace(anchor,""), original+original,
            patched+entry, "OTHER = "+entry+original):
    try:
        m["patch_export"](bad)
    except ValueError:
        pass
    else:
        raise AssertionError("invalid export accepted")
driver_map=(root/"drivers/map").read_text()
assert driver_map.splitlines().count("power/exynos bsd/dev/hakios.p125_sysctl.h")==1
copy_map=(root/"profiles/ios-16.5.copy").read_text()
for line in ("xnu/files/hakios_p125.h osfmk/kern/hakios_p125.h",
             "xnu/files/hakios_p125_impl.h osfmk/arm64/hakios_p125_impl.h"):
    assert copy_map.splitlines().count(line)==1
with TemporaryDirectory() as tmp:
    tree=Path(tmp)
    makefile=tree/"osfmk/kern/Makefile"
    makefile.parent.mkdir(parents=True)
    makefile.write_text(patched)
    headers={
        "osfmk/kern/hakios_p125.h":"xnu/files/hakios_p125.h",
        "osfmk/arm64/hakios_p125_impl.h":"xnu/files/hakios_p125_impl.h",
        "bsd/dev/hakios.p125_sysctl.h":"drivers/power/exynos/hakios.p125_sysctl.h"
    }
    for dst,src in headers.items():
        target=tree/dst
        target.parent.mkdir(parents=True,exist_ok=True)
        target.write_bytes((root/src).read_bytes())
    m["verify_tree"](tree)
    for dst,src in headers.items():
        target=tree/dst
        target.unlink()
        try:
            m["verify_tree"](tree)
        except ValueError:
            pass
        else:
            raise AssertionError("missing staged header accepted: "+dst)
        target.write_bytes((root/src).read_bytes())
    makefile.write_text(original)
    try:
        m["verify_tree"](tree)
    except ValueError:
        pass
    else:
        raise AssertionError("missing export accepted")
print("P12.5 overlay, header staging and XNU export contract PASS")
