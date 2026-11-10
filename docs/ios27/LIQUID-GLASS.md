# iOS 27 on the Galaxy S9: Liquid Glass on CoreAnimation's Metal renderer, and the frame cadence (9 November 2026)

iOS 27 draws its glass materials with CoreAnimation's Metal renderer. On the S9 the default compositor is the OpenGL ES one ([GPU-PATH.md](GPU-PATH.md)),
which has no shader for several of them; the Metal one works ([METAL.md](METAL.md): `sbgl=0`, backboardd in `mtl2-clients`) but was much slower. This page
is where the time went, what was changed, and what each change measured on a phone. Everything new is off by default.

All numbers are from one Galaxy S9 (the second test phone, `cpus=8`, the AOT-rewritten root of [AOT-REWRITE.md](AOT-REWRITE.md)), the scripted input of
`sweetspeed&4096` (sixteen home-screen swipes, then sixteen vertical drags), and the framebuffer's present-gap histogram (`sweetspeed&16384`,
`tools/ios27/perf/gaps.py`): **on time** is the share of frames shown one refresh after the frame before, **motion fps** is 60 x frames / refreshes over the
frames that came within three refreshes of the one before. One run each; none of it is a benchmark.

## Where the Metal path lost its time

1. **Every GPU job was synchronous.** PanVK reaches the kernel's StarbiteMali through `tools/ios27/mesa-ios/starbite-drm`. The copy the Metal bundle of 8
   November was built with predates that shim's asynchronous submits (the OpenGL stack has had them since the night run), so each `SUBMIT` ran its chain to
   the end inside the call: 5,814 submits in ten seconds of swiping, 463 us each, 27 % of the render thread's wall time, on top of its own CPU time. Rebuilt
   with the repository's shim (and a line on the console, `SBDRM[pid name]: async submits on`), PanVK asks for them.
2. **The kernel's "asynchronous" submit was only that for one chain.** It returned at once while the GPU was free; the next submit, and every wait, spun in
   `delay()` under the GPU lock until the chain before was done (`STARLTE GPU ASYNC: ... next submit waited 4202 x avg 347 us` of 4,387 chains). A frame is a
   dozen chains or more (a render pass is a vertex/tiler chain and a fragment chain, a blur is many passes), so the render thread still sat out nearly all of
   the GPU's time on a core, and this frame's GPU work never overlapped the next frame's CPU work. A fence wait also held the client's lock, so a completion
   thread blocked the render thread's next submit. **The fix is a queue** (the fork's `bsd/dev/starlte_gpu.c`, boot-arg `sweetspeed&262144`, as Scion's
   iOS 16.5 Mali driver has it): a submit appends and returns, one kernel thread starts each chain in order and naps until it is done (50 us naps, boot-arg
   `sgu_nap_us`), waiters sleep until their chain's number is done, and the waits no longer hold the client's lock.
3. **Whole-surface cache maintenance, twice a frame.** The Mali is not IO-coherent. The Metal frontend cleaned and invalidated (`DC CIVAC`) every IOSurface a
   command buffer named at commit, and every written one again at completion: 25 GB in 183 s of that run, 1.3 ms a frame for the framebuffer alone, on the
   render and completion threads. `tools/ios27/metal/patches/frontend-flush.diff` adds `SWEET_MTL2_FLUSH` (in `mtl2-env`; 0, the default, is as before):
   `1` cleans a surface at commit only when it is new to the process or its IOSurface seed moved since the last clean (every CPU writer unlocks with a seed
   bump); `2` also skips the invalidate after completion for a surface the GPU has written four times in a row with no seed change (the swap chain,
   CoreAnimation's render targets: nothing reads them on the CPU), except every 64th time. The state lives on the IOSurface object, so a recycled surface
   ID never inherits it. The kernel's GPU present did the same clean (6 MiB of `DC CVAC` a frame on the vsync thread); `sweetspeed&524288` does it only for a
   surface that is new or whose seed moved.
4. **Start-up.** The first frame came 26 to 28 s after the kernel's first line, against 16 s with OpenGL ES: 5 to 6 s to create the device, and 8.3 s in 98
   pipeline creations (85 ms each) with the shader cache reporting 0 hits, 200 misses. Not fixed here (below).

## The frame cadence

With the queue, the framebuffer no longer shows "whatever the GPU has finished": a swap records the number of the last chain queued when it came, and the
vsync thread sleeps until that chain is done before it draws the frame to the panel (its own frame's chains, not the next frame's, which may already be
queued). A swap also waits for the chains of the swap before it, so **at most two frames are in flight**: the window server builds frame N+1 while the GPU
draws frame N, and cannot run further ahead (without that, a scene the GPU needs more than a refresh for would fill the queue's 256 chains and every touch
would be answered a dozen frames late).

What a frame still pays after the queue: the present itself is a GPU job (the frame scaled from 828 x 1792 to the panel's 1440 x 2960, 2.3 to 2.4 ms, in
turn with the app's chains) drawn into the buffer the display controller is reading, so it can tear. **Next step (designed, not built):** the scan-out
service the 16.5 kernel uses (`drivers/display/dpu/hakios.exynos_dpu.c`, already built into the iOS 27 kernel, `dpu_probe=1` reads it): the window server's
display surfaces made of the DPU's pool (`hakios_scanout_carve`), VGF0 scanning out the very surface CoreAnimation drew (`hakios_scanout_show`, flipped at
the panel's TE, scaled by the DPP), nothing copied. The steps: (1) `SweetSurface.cpp` allocates backboardd's display-sized BGRA surfaces from the pool
(physically contiguous, below 4 GB, outside XNU's RAM, mapped write-combined; the Mali's page tables take them as they take any page); (2)
`SweetFramebuffer.cpp` attaches mode 2 (828 x 1792 at 3,328 bytes a row scaled to the panel) and, per swap, waits for the swap's chains and for the flip
before to be latched, then calls `hakios_scanout_show`; a surface is not drawn into again until the flip away from it is latched (two in the pool for
CoreAnimation, as on 16.5); (3) the 60 Hz timer follows the DECON's refresh count instead of free-running. Two things stand in the way on iOS 27 as booted
today: the extra-RAM ranges give `0xD0000000`-`0xD7FFFFFF` to the VM (`extra_ram_mask=127`), which holds the pool's first slot and the scan-out buffers, so
either that range is dropped (`extra_ram_mask=95`, 128 MB less) or the pool keeps to its three slots above `0xD8000000`; and every failure must fall back
to the present of today (the service already goes back to the boot loader's channel on a configuration error).

## Measured

| Boot | What | Swipes: on time, motion fps | Drags: on time, motion fps | First frame | Notes |
|---|---|---|---|---|---|
| g0 | OpenGL ES compositor, kernel as before | 69 %, 45.7 | 66 %, 42.1 | +15.8 s | the default; GPU 37 % busy while swiping |
| m1 | Metal compositor as it was (`sbgl=0`, bundle of 8 November) | 29 %, 33.9 | 28 %, 30.8 | +28.0 s | 5,814 synchronous submits / 10 s at 463 us |
| m2 | m1 + this bundle (PanVK asks for async submits, `SWEET_MTL2_FLUSH=2`), same kernel | 42 %, 36.6 | 34 %, 32.8 | +25.7 s | 96 % of submits still wait for the chain before |
| m3 | m2 + the kernel's chain queue, the present's clean on a seed change, two frames in flight (`sweetspeed&262144`, `&524288`) | 67 %, 44.5 | 64 %, 41.1 | +18.1 s | a submit costs 1 us; one GPU fault and reset (below) |
| m4 | m3 + the shim's timeline query waits for the queue, and a buffer is closed only after every queued chain | 66 %, 44.3 and 67 %, 44.6 | 64 %, 40.5 and 66 %, 41.7 | +21.9 s | two passes of the script, no GPU fault; a buffer close may now block (up to 90 ms seen) |

So the Metal compositor, which draws the glass, is where the OpenGL ES one is (within two or three points of frames on time), from less than half of
it. What the two draw is not the same picture: at the same moment of the script the OpenGL ES compositor shows no dock platter, no folder glass, no search
pill and no panel behind the keyboard; the Metal one draws them all.

**The fault of m3, and why the queue found it.** One chain of the run faulted in the GPU's MMU at address 0 and the kernel reset the GPU (nothing died).
PanVK's shim has no real sync objects: every fence is "signalled", every timeline "reached". That was true while a submit ran its chain to the end, and
nearly true with one chain in flight. With chains really queued, a thread that asked a queue's timeline for its value right after another thread's submit
was told "all reached", took chains that had not run yet for finished, and reused their memory under the GPU. The shim's timeline query now waits for the
chains queued so far before it answers (as its fence wait does), and the kernel closes a buffer only once every chain queued by then is done (a chain
reaches buffers its submit does not list). Honest sync objects (a chain number per fence and timeline point, so that a wait is for that chain and not for
the queue to drain, and a close can be deferred instead of waited for) are the next step.

**The shader cache does work.** The 0 hits of m1 were a first boot of a new configuration (the cache's names include the device's profile; the extra-RAM
change since the bundle's last runs changed it). The boots after it: 121 hits and 32 misses, then 100 hits and no miss; pipeline creation fell from 8.3 s
to 1.5 s, the first frame from +28 s to about +20 s. What is left of the start is the device itself (5 to 6 s before the first GPU call).

## On the first phone, and what a fuller home screen costs

The owner's phone (a root with more on its home screen: widgets, a weather widget's renderer, a wallpaper poster) ran the same script on the kernel
that merges this line with the data-protection work (`rc1/ios27-gpu-keystore` `414af8e2`, tag `ios27-s9-rc1-20261109`), with the Metal compositor and
the cap below: 43 % of swipe frames on time (motion 37.4 fps) and 63 % of the drags, no GPU fault, no exit of backboardd or SpringBoard in 7.5 minutes,
and it is what that phone was left on. The bench phone's 66 % and this 43 % are the same code. The difference is in the frames: 35 chains a frame
there against 25 on the bench phone, at 0.42 to 0.48 ms a chain, so 14.7 ms of GPU time a frame against 12.2, before the 2.4 ms of the present. At
60 Hz that does not fit; the GPU, not the CPU, is what a heavy glass frame waits for now. A chain's 0.4 ms is mostly not drawing: every chain is started,
polled for (10 us looks, then 50 us naps; `sgu_spin_us` sets how long it looks) and flushed on its own, and a pass is two of them. What would shorten it,
none of it built: the fragment chain of one pass and the vertex/tiler chain of the next on the two job slots at once; the job interrupt instead of the
poll; fewer passes from CoreAnimation for the same picture.

## A cap on the GPU's wired memory

Every buffer StarbiteMali makes is wired kernel memory that nothing can page out, and a PanVK device wires 57 MiB when it is made (a 32 MiB tiler heap,
17 MiB of varyings, 8 MiB of TLS). `sweetspeed&134217728` (or `sbm_cap_mb=`, `sbm_client_cap_mb=`) bounds the sum: all clients together 35 % of the
memory, one client half of that (1029 and 514 MiB on a phone with 2940 MiB). A buffer beyond it is refused (the client sees an allocation failure) with
a line on the console. backboardd peaked at 182 MiB in the runs above, so the cap never fired; it is there for the day several Metal clients start at once.
Which processes get a Metal device at all is the list in `/private/var/local/mtl2-clients`; for the compositor it should hold backboardd and nothing else
unless a process needs one (SpringBoard's renderer service asserts without a device once posters are in use).

## The start

backboardd took 4 to 6 s to make its Metal device, nearly all of it before PanVK's first call to the kernel: `dlopen()` of the plug-in and of PanVK
(38 MB outside the shared cache) while every other daemon starts. chronod, started meanwhile, asks the render server for the displays, gives up and
aborts once (QuartzCore's `query_displays`), and runs when it is started again. The kernel's prewarm thread (`sweetspeed&128`) read the GL stack into the
page cache at mount; with `sbgl=0` on the command line it now reads the Metal plug-in and PanVK instead (`2db66861` of the fork's branch). Timed
since (below): no gain.

## Later the same night: the close that waited, and frames that are on time but late

- **A buffer close no longer waits.** The kernel's belt of m4 (a close waits for every chain queued by then) held the closing thread for as long as
  the queue took to drain: up to 90 ms. The buffer is now only marked with the chain it has to outlive and is unmapped and freed at one of the client's
  next calls (`af8ac0d0`). On the bench phone, with the full root of the release candidate: closes of 1 to 2 us, the longest 2.4 ms, no GPU fault, no
  exit; 47 % of swipe frames on time on that fuller home screen (the first phone: 43 to 46 %), 95 % (56.8 fps) while a list scrolled in an app.
- **On time is not the same as soon.** In that 95 % run the queue held 60 chains on average and up to 199: about three frames, 55 ms between a chain's
  submit and its start. The frames came one refresh apart, each of them late. "Two frames in flight" at the swap does not bound it (the window server
  commits more than it swaps). `sgu_depth=N` (`2abb4031`; default 256, the whole queue, as before) makes a submit sleep when N chains wait, until half
  of them are done. **Not yet run**: 48 (a frame or two) is the first value to try, against the touch-to-picture delay, not against frames on time.
- **Three knobs for a chain's cost, built and not yet run**, all off by default: `sgu_flushred=1` (`0537327e`: the Mali's flush reduction, as
  Linux's Panfrost driver has it on this GPU: a chain carries the GPU's flush count from its submit and the cache flush at its start is left out
  when one happened since, which for a queued chain is always), `sgu_spin_us=600` (the worker looks longer before its first nap) and `sgu_depth`
  above. What to read from a run of each is the queue's own line: time per chain, time queued, depth.
- The prewarm of the Metal plug-in did not shorten the device's start (4.4 s again), and chronod's abort comes 4.6 s after the device exists, just
  before the first frame: it waits for the render server's first display state, not for the device. Harmless (it is started again and runs).

## Not done

- **Honest sync objects in the shim** (above): today a wait is for the whole queue.
- **The device's start** (5 to 6 s), and the twelve no-copy buffers PanVK refuses (`HostMemory: NotRepresentable`; CoreAnimation copies instead): five a
  boot here.
- The zero-copy scan-out above.
- The CPU side: backboardd uses 0.19 to 0.25 of a core while swiping on Metal against 0.13 on OpenGL ES (metal2vulkan's and PanVK's per-draw work).
- A chain's fixed cost (above), which is what a fuller home screen now waits for.

## How to turn it on

Kernel: the fork's `wip/ios27-gpu-queue` (on `bb6a6a4`; draft pull request 6 of Scion-Project/xnu). Command line: `sbgl=0` (with `sbmtl=1`)
and `sweetspeed` with 262144, 524288 and 134217728 added. Root: this bundle (`tools/ios27/metal/mk-bundle27.sh` applies `frontend-flush.diff`; PanVK built with the
repository's `starbite-drm`), `SWEET_MTL2_DEXR=1`, `SWEET_MTL2_FLUSH=2` and `STARBITE_DRM_LEAKS=0` in `/private/var/local/mtl2-env`, backboardd in
`/private/var/local/mtl2-clients`, and `/private/var/local/m2v-cache` (mode 0777) for the pipeline cache. Back to OpenGL ES: backboardd out of
`mtl2-clients` and no `sbgl=0`.

## Open: Settings row icons under the Metal compositor (10 Nov, seen, not explained)

On the RC1 full root with the Metal compositor (kernel q12, `SWEET_MTL2_FLUSH=2`) the top level of Settings draws some row icons right,
four as horizontal coloured stripes (Bluetooth, Appearance, Camera, Control Center) and eight not at all (Wi-Fi, Battery, Sounds, Focus,
Screen Time, Passcode, Emergency SOS, Privacy & Security). No picture of the same screen under the GL renderer was taken the same night.
Two suspects, each with a switch, neither run on a phone yet:

- kernel: a surface over the app's own memory that could not be wrapped is a one-time copy; what the app draws afterwards never reaches
  it. `sweetspeed` bit 8388608 copies again at use.
- frontend: modes 1 and 2 of `SWEET_MTL2_FLUSH` skip the commit-time clean of a surface whose seed did not move; a surface over client
  memory is written with no lock, so its seed never moves. Control: mode 0. `frontend-flush.diff` now cleans at every commit any surface
  with pixel format 0, a base that is not 16 KiB aligned, or under 1 MiB (unmeasured; cleaning more cannot draw wrong, an icon is 12 KiB).

The console of that boot points at the frontend: the kernel wrapped the icons' memory (56x56, pixel format 0, 16 KiB surfaces made by
the render server) rather than copying it, with one copy in the whole boot; and one of those surfaces was empty when it was wrapped
(`0 of 12544 bytes set`), so its picture was written afterwards with no seed change. Cleaned once at its first commit and never again,
such a surface shows whatever cache lines happen to have been evicted: none (blank), some (runs of 16 pixels: stripes) or all (right).

Test order: the rebuilt bundle with mode 2 first; if the icons are still wrong, mode 0 as the control, then the kernel bit alone, then
one GL picture.
