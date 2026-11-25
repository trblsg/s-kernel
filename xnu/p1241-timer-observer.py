"""Pinned XNU timer instrumentation; preserve original callbacks and deadlines."""
from pathlib import Path
import sys

ARM_EDITS = (
    ("void\ntimer_intr(__unused int inuser, __unused uint64_t iaddr)",
     "extern void hakios_p1241_timer_observe(unsigned cpu,unsigned reasons);\n\n"
     "void\ntimer_intr(__unused int inuser, __unused uint64_t iaddr)"),
    ("abstime = mach_absolute_time(); /* Get the time now */",
     "abstime = mach_absolute_time(); /* Get the time now */\n"
     "\tunsigned p1241_reasons=abstime<cpu_data_ptr->rtcPop?8u:0u; /* P1241_TIMER_OBSERVER */"),
    ("cpu_data_ptr->idle_timer_deadline = 0x0ULL;\n\t\tnew_idle_timeout_ticks",
     "p1241_reasons|=1u;\n\t\tcpu_data_ptr->idle_timer_deadline = 0x0ULL;\n\t\tnew_idle_timeout_ticks"),
    ("mytimer->has_expired = TRUE;    /* Remember that we popped */",
     "p1241_reasons|=2u;\n\t\tmytimer->has_expired = TRUE;    /* Remember that we popped */"),
    ("(void)running_timers_expire(processor, abstime);",
     "if(running_timers_expire(processor, abstime))p1241_reasons|=4u;\n"
     "\thakios_p1241_timer_observe((unsigned)cpu_data_ptr->cpu_number,p1241_reasons);"),
)
RTC_EDITS = (
    ("int\nsetPop(uint64_t time)",
     "extern void hakios_p1241_pop_observe(unsigned cpu,uint64_t deadline,uint64_t now,unsigned ticks);\n\n"
     "int\nsetPop(uint64_t time)"),
    ("cdp->rtcPop = delay_time + current_time;\n\n\tml_set_decrementer((uint32_t) delay_time);",
     "cdp->rtcPop = delay_time + current_time;\n\n\tml_set_decrementer((uint32_t) delay_time);\n"
     "\thakios_p1241_pop_observe((unsigned)cdp->cpu_number,time,current_time,(unsigned)delay_time); /* P1241_POP_OBSERVER */"),
)
def patch(source, edits, marker):
    if marker in source:
        if source.count(marker)!=1 or any(source.count(new)!=1 for old,new in edits):
            raise ValueError("inconsistent P12.4.1 timer observer")
        return source
    for old,new in edits:
        if source.count(old)!=1:
            raise ValueError("P12.4.1 pinned timer source anchor changed: "+old[:80])
        source=source.replace(old,new,1)
    return source

if __name__ == "__main__":
    arm,rtc=map(Path,sys.argv[1:])
    a=patch(arm.read_text(encoding="utf-8"),ARM_EDITS,"P1241_TIMER_OBSERVER")
    b=patch(rtc.read_text(encoding="utf-8"),RTC_EDITS,"P1241_POP_OBSERVER")
    arm.write_text(a,encoding="utf-8")
    rtc.write_text(b,encoding="utf-8")
    print("P12.4.1 timer observer: actual service reason and programmed setPop only")
