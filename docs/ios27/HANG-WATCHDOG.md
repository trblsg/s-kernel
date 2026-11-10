# iOS 27 on the Galaxy S9: a watchdog for hard hangs (9 November 2026; built, not yet run on a phone)

On 9 November a boot of the S9 stopped dead 66 seconds in: no panic, the USB console gone, nothing in pstore. With `wdt=-1` on the command line
nothing ever brings such a phone back; it takes the key combination. This page says what the kernel has today and what a watchdog for that case
should be. It is built as described under "As built"; no phone has run it yet.

## What `wdt=` is, and is not

`wdt` is XNU's own argument, not a driver of the S9's watchdog timer: `wdt=-1` switches off the interrupt-masked and preemption-disabled time checks
(`osfmk/arm/arm_init.c`) and the panic when the root device does not appear (`iokit/bsddev/IOKitBSDInit.cpp`). Any other value arms those checks
again; none starts a hardware timer, because this kernel has no driver for the Exynos 9810's watchdog (`WDT_CLUSTER0`). `wdt=60` would therefore
not reset a hung phone, and would bring back debug panics the port's polled drivers trip.

## What exists and can be reused

`bsd/dev/herolte_ufs.c` already has the way out: `starlte_panic_to_recovery()` writes the panic log's head and the console's last lines into the
ramoops ring TWRP shows as `/sys/fs/pstore/pmsg-ramoops-0`, then resets through the PMU into recovery (`INFORM2`/`INFORM3`, `SWRESET`). It was
written for panics: it takes no lock, allocates nothing, and its two mappings are made at boot. `sweetreboot=N` uses the same reset after N seconds.

## The proposal: `sweetwd=N`

- **Who fires.** A timer call, so it runs from the timer interrupt and needs no thread to be scheduled: every 2 s it compares the time of the last
  heartbeat with now; N seconds without one and it writes "hang watchdog: no heartbeat for N s" plus the console tail to the ramoops ring and resets
  into recovery, as a panic does. A hang with interrupts still alive (the VM starved, a lock everyone waits for, the scheduler's run queues stuck)
  is caught; a core dead with interrupts masked on every CPU is not (that needs the hardware timer, below).
- **Who beats.** Not a kernel thread alone: in the hang of the 9th the kernel may well have been running while user space was not. Two beats, both
  required: a kernel thread at ordinary priority that wakes every second (the scheduler and timers work), and proof that user space makes progress:
  the count of system calls and Mach traps taken from EL0 on any CPU must have moved since the last check (one counter bump in `sleh.c`'s system-call
  path, per CPU, no lock). launchd alone wakes more than once in N = 60 s; a phone whose every process is stuck does not.
- **What it must not do.** Fire during a long legitimate stall: the first boot's shader compiles and the root's first mount are busy, not silent, so
  the system-call beat covers them; the watchdog starts only once launchd has exec'ed. Off by default (`sweetwd=0`); a test boot uses 60.
- **Where it resets to.** Recovery, with the log, while the phone is a test device: the point is the evidence. For a phone in someone's hand a
  second value (`sweetwd_boot=1`) would restart into the system instead (`INFORM3` for a normal boot).
- **Later, the hardware timer.** The same beat can pet `WDT_CLUSTER0` (a count-down register and a reset mask in the PMU). That covers the dead-core
  case too, but it is a register sequence nobody here has run on this phone; it is a second step, tried first on the bench phone with a long count.

## As built

`wip/ios27-hang-watchdog` of the fork (`3c4be6ee`, on `bb6a6a4`): boot-argument `sweetwdog=SECONDS` (absent or 0: off; less than 60 is 60). The timer
call is armed from the watchdog's own thread and looks every 2 s. User space's sign of life is one variable (`sweet_wdog_mark`): the look sets it to 0,
the next system call or Mach trap from EL0 sets it to 1 (`handle_svc`: one read per call, at most one write per look and CPU; with the watchdog off it
is 1 for good and never written). The first system call arms the watchdog. On a miss the line "STARBITE: HANG WATCHDOG (sweetwdog=N): no sign of life
from the kernel's thread | user space; kernel thread A ms ago, user space B ms ago, up U s" and the console's tail go to the pmsg ring, then the PMU
reset into recovery. `kern.sweetwdog_test` provokes it without hanging anything (1: user space's signs are not counted, 2: the kernel thread stops
noting the time; from the USB console: `sysctl kern.sweetwdog_test=1`), and `kern.sweetwdog_kernel_age_ms` / `kern.sweetwdog_user_age_ms` read the ages
as the last look saw them. A restart into the system instead of recovery, and the hardware timer, are not built.

About 150 lines in `herolte_ufs.c`, six in `sleh.c`.

The same sign of life serves the loader (`bce1bead`, boot-argument `scionup=1`, off by default): at the first system call two words of the loader's
64-byte boot record at `0x91ffe000` are written (its sequence number copied, and "SNUP"), only when the record's magic is there, so the loader can
tell a boot that reached user space from one that did not. The loader also reads the watchdog's line from the pmsg ring: its first bytes stay
"STARBITE: HANG WATCHDOG (sweetwdog=". The test: `sweetwd=60` idle for ten minutes with no reset; then a deliberate hang (a
kernel thread that takes the VM's page-queue lock and sleeps, from a console verb) must end in TWRP within 62 s with the watchdog's line in pstore.
