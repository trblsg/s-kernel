#!/usr/bin/env python3
"""Exact private FreeBSD header overlay for brcmfmac only."""
import pathlib
import sys
old = "extern unsigned long jiffies;\t/* defined in sys/kern/subr_ticks.S */"
new = """#ifdef LX_P127_JIFFIES
extern unsigned long lx_os_jiffies_now(void);
#define jiffies (lx_os_jiffies_now())
#else
""" + old + """
#endif /* LX_P127_JIFFIES */"""
def patch(src):
    if src.count(new) == 1 and src.count(old) == 1:
        return src
    if src.count(old) != 1 or "LX_P127_JIFFIES" in src:
        raise ValueError("P12.7 pinned FreeBSD jiffies declaration changed")
    return src.replace(old, new, 1)
if __name__ == "__main__":
    p = pathlib.Path(sys.argv[1])
    src = p.read_text()
    result = patch(src)
    if result != src:
        p.write_text(result)
    print("P12.7 monotonic jiffies header PASS")
