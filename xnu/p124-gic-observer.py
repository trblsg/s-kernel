#!/usr/bin/env python3
"""Observe already acknowledged GIC IRQs; never read IAR twice or alter dispatch."""
from pathlib import Path
import sys

MARKER = "/* P12.4_IRQ_OBSERVER */"
ANCHOR = "\t\tgic_active_iar[cpu] = iar | GIC_IAR_TAKEN;\n"
DECL = "static void\ngic_irq_handler(__unused void *target,"
CALL = ANCHOR + "\t\thakios_p124_irq_observe(cpu,intid,intid==GIC_INTID_VTIMER); " + MARKER + "\n"
PROTOTYPE = "extern void hakios_p124_irq_observe(int cpu, uint32_t intid, bool timer);\n\n"
def patch(source):
    if MARKER in source:
        if source.count(MARKER) != 1 or source.count(CALL) != 1 or source.count(PROTOTYPE) != 1:
            raise ValueError("P12.4 GIC observer inconsistent")
        return source
    if source.count(ANCHOR) != 1 or source.count(DECL) != 1:
        raise ValueError("P12.4 GIC pinned source anchors changed")
    return source.replace(ANCHOR,CALL).replace(DECL,PROTOTYPE+DECL)

if __name__ == "__main__":
    path=Path(sys.argv[1])
    original=path.read_text(encoding="utf-8")
    changed=patch(original)
    if changed!=original:
        path.write_text(changed,encoding="utf-8")
    print("P12.4 GIC observer: acknowledged IRQ only")
