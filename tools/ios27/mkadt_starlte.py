#!/usr/bin/env python3
"""Build the starlte (SM-G960F, Exynos 9810) ADT for the STARLTE XNU config.

Derived from ios-a5xelte tools/mkadt_herolte.py (wardevsa, herolte branch);
uses its adt.py / mkadt.py (tools/ios27/herolte). Addresses come from
the starlte DTB inside the official TWRP image (device/dtb/starlte.dts), the
stock kernel cmdline and the xnuloader/sboot probe. First XNU
boot: the 472 MiB window below the RKP region; four Cortex-A55 CPUs (boot-arg
cpus=N picks how many start).
"""
import argparse
import os
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent / 'herolte'))
import adt     # noqa: E402
import mkadt   # noqa: E402

DRAM_BASE = 0x92000000      # isolated XNU window (after vendor logs at 0x91202000..0x9179c000)
DRAM_SIZE = 0x1d800000      # 472 MiB, ends at the RKP region 0xaf800000
# --bank2: the S9's second DRAM bank (TWRP /proc/iomem 880000000-8ffffffff, 2 GiB, no
# reserved-memory node in it; Windows uses it as RAM stage 3). Needs the bank-aware
# shim (BANK2_* in xnuloader/sboot/xnu_boot.c) and, above 4 GiB, the SMP trampoline at
# 0x8fffe000 (herolte patch 0223, active for STARLTE).
BANK2_BASE, BANK2_SIZE = 0x880000000, 0x80000000
SOC_BASE = 0x10000000       # arm-io ranges base; child regs are relative
FB_BASE, FB_WIDTH, FB_HEIGHT = 0xcc000000, 1440, 2960
TIMEBASE = 26000000         # CNTFRQ_EL0 is 0 at handoff; OSCCLK is 26 MHz
CONSOLE_TOP = 0               # the boot console takes the whole panel once XNU starts
CONSOLE_HEIGHT = FB_HEIGHT    # (was 640..1880, between the menu header and the card slot)
# Panic region (pe_init.c check_for_panic_log): /pram reg + /chosen
# embedded-panic-log-size. Without it xnu-11417's DEVELOPMENT panic path
# asserts panic_info (pe_init.c:711) and every panic nests to the limit.
# Bank 2 above XNU's RAM (--bank2-mb <= 1536) and the GPU tables at 0x8f0000000.
PRAM_BASE, PRAM_SIZE, PANIC_LOG_SIZE = 0x8f8000000, 0x100000, 0x80000
LITTLE_CORES = 4            # Cortex-A55 cluster; cpus=N boot-arg limits what starts
BIG_CORES = 4               # Mongoose M3 cluster (--big)


def set_prop(node, name, value):
    for i, (key, flags, _) in enumerate(node.props):
        if key == name:
            node.props[i] = (key, flags, value)
            return
    node.props.append((name, 0, value))


def child(node, name):
    for candidate in node.children:
        if candidate.prop('name') == mkadt.cstr(name):
            return candidate
    raise ValueError(f'missing donor ADT node {name}')


# --ipad7: iPadOS 18 (iPad7,11 / J171AP userland). The canvas keeps an iPad's
# 3:4 at the panel's full width: 1440x1920 = 720x960 points @2x
# (sweetfb_lw=1440 sweetfb_lh=1920), 94% of the 10.2" iPad's 768x1024 points.
# A real iPad's 1536+ px width does not fit SweetFramebuffer's in-panel surface.
IPAD_CANVAS = (1440, 1920)


def ipad_identity(root, adt_path):
    """Root identity and /product of the real J171AP tree, A10 chip-id."""
    old = child(root, 'product')
    phandle = old.prop('AAPL,phandle')
    root.children = [n for n in root.children if n.prop('name') != mkadt.cstr('product')]
    props = mkadt.d22ap_product(adt_path)
    product = mkadt.node('product', ([('AAPL,phandle', phandle)] if phandle else []) + props)
    root.children.append(product)
    # J171AP leaves artwork-device-subtype empty for iBoot; 2160 = iPad 10.2".
    set_prop(product, 'artwork-device-subtype', mkadt.u32(2160))
    set_prop(product, 'artwork-scale-factor', mkadt.u32(2))
    chosen = child(root, 'chosen')
    set_prop(chosen, 'display-scale', mkadt.u32(2))
    set_prop(chosen, 'chip-id', mkadt.u32(0x8010))
    set_prop(root, 'model', mkadt.cstr('iPad7,11'))
    set_prop(root, 'target-type', mkadt.cstr('J171'))
    set_prop(root, 'target-sub-type', mkadt.cstr('J171AP'))
    return IPAD_CANVAS, ('J171AP', 'iPad7,11')


# --iphone11: iOS 26 (iPhone12,1 / N104AP userland, A13). iPhone 11 screen:
# 414x896 points @2x = 828x1792, scaled up to the panel by the kernel
# (sweetfb_scale=1 sweetfb_lw=828 sweetfb_lh=1792); artwork subtype 1792.
IPHONE11_CANVAS = (828, 1792)


def iphone11_identity(root, adt_path):
    """Root identity and /product of the real N104AP tree, A13 (T8030) chip-id."""
    old = child(root, 'product')
    phandle = old.prop('AAPL,phandle')
    root.children = [n for n in root.children if n.prop('name') != mkadt.cstr('product')]
    props = mkadt.d22ap_product(adt_path)
    product = mkadt.node('product', ([('AAPL,phandle', phandle)] if phandle else []) + props)
    root.children.append(product)
    set_prop(product, 'artwork-device-subtype', mkadt.u32(1792))
    set_prop(product, 'artwork-scale-factor', mkadt.u32(2))
    chosen = child(root, 'chosen')
    set_prop(chosen, 'display-scale', mkadt.u32(2))
    set_prop(chosen, 'chip-id', mkadt.u32(0x8030))
    set_prop(root, 'model', mkadt.cstr('iPhone12,1'))
    set_prop(root, 'target-type', mkadt.cstr('N104'))
    set_prop(root, 'target-sub-type', mkadt.cstr('N104AP'))
    return IPHONE11_CANVAS, ('N104AP', 'iPhone12,1')


# --iphone7: iOS 14 (iPhone9,1 / D10AP userland, A10 T8010, plain arm64, no
# PAC). iPhone 7 screen: 375x667 points @2x = 750x1334, scaled up to the panel
# by the kernel (sweetfb_scale=1 sweetfb_lw=750 sweetfb_lh=1334); artwork
# subtype 1334. Its /product says compatible-device-fallback iPhone8,1 (6s).
IPHONE7_CANVAS = (750, 1334)


def iphone7_identity(root, adt_path):
    """Root identity and /product of the real D10AP tree (iOS 14.8), A10 (T8010) chip-id."""
    old = child(root, 'product')
    phandle = old.prop('AAPL,phandle')
    root.children = [n for n in root.children if n.prop('name') != mkadt.cstr('product')]
    props = mkadt.d22ap_product(adt_path)
    product = mkadt.node('product', ([('AAPL,phandle', phandle)] if phandle else []) + props)
    root.children.append(product)
    set_prop(product, 'artwork-device-subtype', mkadt.u32(1334))
    set_prop(product, 'artwork-scale-factor', mkadt.u32(2))
    chosen = child(root, 'chosen')
    set_prop(chosen, 'display-scale', mkadt.u32(2))
    set_prop(chosen, 'chip-id', mkadt.u32(0x8010))
    set_prop(root, 'model', mkadt.cstr('iPhone9,1'))
    set_prop(root, 'target-type', mkadt.cstr('D10'))
    set_prop(root, 'target-sub-type', mkadt.cstr('D10AP'))
    return IPHONE7_CANVAS, ('D10AP', 'iPhone9,1')


def build(dram=(DRAM_BASE, DRAM_SIZE), scale2=False, big=False, ipad=None, iphone11=None, iphone7=None, canvas_override=None,
          scale_override=None, subtype_override=None):
    root = mkadt.build(seed=os.urandom(256), qemu=True)  # xnu-12377 wants 256
    canvas = (FB_WIDTH, FB_HEIGHT)
    identity = ('D22AP', 'iPhone10,3')
    if ipad or iphone11 or iphone7:
        if ipad:
            canvas, identity = ipad_identity(root, ipad)
        elif iphone11:
            canvas, identity = iphone11_identity(root, iphone11)
        else:
            canvas, identity = iphone7_identity(root, iphone7)
        if canvas_override:
            canvas = canvas_override
        product, chosen = child(root, 'product'), child(root, 'chosen')
        if scale_override:
            set_prop(chosen, 'display-scale', mkadt.u32(scale_override))
            set_prop(product, 'artwork-scale-factor', mkadt.u32(scale_override))
        if subtype_override:
            set_prop(product, 'artwork-device-subtype', mkadt.u32(subtype_override))
    elif scale2:
        # As mkadt_herolte.py --scale2: a whole iPhone XR screen (414x896
        # points @2x = 828x1792), 2.9x fewer pixels than the panel for the CPU
        # renderer; the kernel scales it up (sweetfb_scale=1 sweetfb_lw=828
        # sweetfb_lh=1792). Needs the D22AP /product node for the artwork.
        set_prop(child(root, 'chosen'), 'display-scale', mkadt.u32(2))
        product = child(root, 'product')
        set_prop(product, 'artwork-device-subtype', mkadt.u32(1792))
        set_prop(product, 'artwork-scale-factor', mkadt.u32(2))
        canvas = (828, 1792)
    # iPhone X identity, as herolte (or the iPad's, --ipad7); 'samsung,a5xelte'
    # keeps the shared SweetPlatformExpert IONameMatch.
    set_prop(root, 'compatible', mkadt.cstr(identity[0]) + mkadt.cstr(identity[1]) +
             mkadt.cstr('samsung,a5xelte') + mkadt.cstr('samsung,herolte') +
             mkadt.cstr('samsung,starlte') + mkadt.cstr('samsung,exynos9810') +
             mkadt.cstr('AppleARM'))
    chosen = child(root, 'chosen')
    set_prop(chosen, 'dram-base', mkadt.u64(dram[0]))
    set_prop(chosen, 'dram-size', mkadt.u64(dram[1]))
    set_prop(chosen, 'firmware-version', mkadt.cstr('starlte-entry'))
    set_prop(chosen, 'system-firmware-version', mkadt.cstr('starlte-entry'))
    set_prop(chosen, 'embedded-panic-log-size', mkadt.u32(PANIC_LOG_SIZE))
    # Synthetic, locally administered addresses (see mkadt_herolte.py).
    for name, last in (('wifi0', 1), ('bluetooth0', 2), ('ethernet0', 3)):
        set_prop(chosen, f'mac-address-{name}', bytes((0x02, 0x53, 0x54, 0x41, 0x52, last)))
    cpus = child(root, 'cpus')
    cpu0 = child(cpus, 'cpu0')
    # S-Boot starts on Cortex-A55 core 0: MPIDR 0x81000000, Aff1|Aff0 = 0.
    # SweetCPU matches IONameMatch 'arm,cortex-a53'. The other seven cores
    # stay undescribed until their MPIDRs are measured.
    set_prop(cpu0, 'compatible', mkadt.cstr('arm,cortex-a55') + mkadt.cstr('arm,cortex-a53') +
             mkadt.cstr('ARM,v8'))
    set_prop(cpu0, 'reg', mkadt.u32(0))
    set_prop(cpu0, 'timebase-frequency', mkadt.u32(TIMEBASE))
    cpus.children = [cpu0]
    # Cortex-A55 cores 1..3: MPIDR_EL1 Aff1 = core (0x81000n00), measured on
    # the phone with starsmpprobe=1; PSCI CPU_ON takes n (SweetCPU.cpp).
    # One cluster, like mkadt_herolte.py.
    # --big: the Mongoose M3 cores as logical 4..7. They need the vendor PMU
    # defaults before PSCI CPU_ON 0x100..0x103 (Linux S9+ port; SweetCPU.cpp),
    # and their MPIDR is 0x100+n without the MT bit: the same Aff1:Aff0 as
    # A55 core 1, so XNU's reset vector adds bit 16 for them (reg 0x10100+n).
    for logical in range(1, LITTLE_CORES + (BIG_CORES if big else 0)):
        m3 = logical >= LITTLE_CORES
        cpus.children.append(mkadt.node(f'cpu{logical}', [
            ('device_type', mkadt.cstr('cpu')),
            ('compatible', (mkadt.cstr('samsung,mongoose-m3') if m3 else mkadt.cstr('arm,cortex-a55')) +
             mkadt.cstr('arm,cortex-a53') + mkadt.cstr('ARM,v8')),
            ('reg', mkadt.u32(0x10100 + logical - LITTLE_CORES if m3 else logical << 8)),
            ('cpu-id', mkadt.u32(logical)),
            ('cluster-id', mkadt.u32(0)),
            ('cluster-type', mkadt.u32(0)),
            ('die-cluster-id', mkadt.u32(0)),
            ('cluster-core-id', mkadt.u32(logical)),
            ('l2-cache-id', mkadt.u32(1 if m3 else 0)),
            ('l3-cache-id', mkadt.u32(0)),
            ('state', mkadt.cstr('stopped')),
            ('timebase-frequency', mkadt.u32(TIMEBASE)),
            ('clock-frequency', mkadt.u32(2704000000 if m3 else 1794000000)),
            ('bus-frequency', mkadt.u32(100000000)),
            ('memory-frequency', mkadt.u32(1794000000)),
            ('peripheral-frequency', mkadt.u32(TIMEBASE)),
            ('fixed-frequency', mkadt.u32(TIMEBASE)),
        ]))
    arm_io = child(root, 'arm-io')
    set_prop(arm_io, 'device_type', mkadt.cstr('starlte-io'))
    set_prop(arm_io, 'compatible', mkadt.cstr('samsung,exynos9810-io') + mkadt.cstr('arm-io'))
    set_prop(arm_io, 'ranges', mkadt.u64(0, SOC_BASE, 0xf0000000))
    arm_io.children = [n for n in arm_io.children if n.prop('name') == mkadt.cstr('wdt')]
    # GIC-400 (starlte DTB interrupt-controller@10100000): distributor
    # 0x10101000, CPU interface 0x10102000; virtual timer PPI 11 = INTID 27.
    arm_io.children.append(mkadt.node('gic', [
        ('compatible', mkadt.cstr('arm,cortex-a15-gic')),
        ('interrupt-controller', mkadt.cstr('master')),
        ('virtual-timer-intid', mkadt.u32(27)),
        ('reg', mkadt.u64(0x10101000 - SOC_BASE, 0x1000, 0x10102000 - SOC_BASE, 0x1000)),
    ]))
    arm_io.children.append(mkadt.node('disp0', [
        ('device_type', mkadt.cstr('display-subsystem')),
        ('compatible', mkadt.cstr('samsung,a5xelte-display') +
         mkadt.cstr('samsung,herolte-display') + mkadt.cstr('samsung,starlte-display')),
        ('canvas-width', mkadt.u32(canvas[0])),
        ('canvas-height', mkadt.u32(canvas[1])),
    ]))
    root.children = [n for n in root.children if n.prop('name') not in
                     (mkadt.cstr('sweet-fbcon'), mkadt.cstr('sweet-ufs'))]
    # Exynos 9810 UFSHCI (starlte DTB ufs@0x11120000): standard HCI at +0,
    # vendor HCI at +0x1100 (0x11121100). Absolute address, read by
    # bsd/dev/herolte_ufs.c (ufsprobe=1; read-only on STARLTE).
    root.children.append(mkadt.node('herolte-ufs', [
        ('reg', mkadt.u64(0x11120000, 0x1300)),
    ]))
    root.children.append(mkadt.node('pram', [('reg', mkadt.u64(PRAM_BASE, PRAM_SIZE))]))
    # pe_serial.c looks this node up by name for the kprintf console.
    root.children.append(mkadt.node('a5xelte-fbcon', [
        # The whole panel (CONSOLE_TOP 0): the console clears it when it starts.
        ('reg', mkadt.u64(FB_BASE + CONSOLE_TOP * FB_WIDTH * 4,
                          FB_WIDTH * (CONSOLE_HEIGHT - CONSOLE_TOP) * 4)),
        ('width', mkadt.u32(FB_WIDTH)),
        ('height', mkadt.u32(CONSOLE_HEIGHT - CONSOLE_TOP)),
        ('stride', mkadt.u32(FB_WIDTH * 4)),
    ]))
    return root


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--scale2', action='store_true',
                        help='iPhone XR canvas 828x1792 @2x, scaled by the kernel')
    parser.add_argument('--big', action='store_true',
                        help='describe the four Mongoose M3 cores too (logical 4..7, boot-arg cpus=8)')
    parser.add_argument('--bank2', action='store_true',
                        help='XNU RAM = the second bank instead of the 472 MiB window')
    parser.add_argument('--bank2-mb', type=int, default=1024,
                        help='MiB of the second bank; on herolte 1536 boots with boot-arg '
                             'vm_compression_limit=98304 and 2048 panics in write_pte_fast')
    parser.add_argument('--ipad7', type=Path, metavar='J171AP_ADT',
                        help='iPadOS 18 identity from the real J171AP device tree '
                             '(build/ipsw-18/j171ap.adt), 1440x1920 @2x canvas')
    parser.add_argument('--iphone11', type=Path, metavar='N104AP_ADT',
                        help='iOS 26 identity from the real N104AP device tree '
                             '(build/ipsw-26-n104/n104ap.adt), 828x1792 @2x canvas')
    parser.add_argument('--iphone7', type=Path, metavar='D10AP_ADT',
                        help='iOS 14 identity from the real D10AP device tree '
                             '(build/ipsw-14/d10ap.adt), 750x1334 @2x canvas')
    parser.add_argument('--canvas', metavar='WxH',
                        help='with --ipad7: another canvas, e.g. 1440x2960 = the whole panel '
                             '(720x1480 points @2x); boot-args sweetfb_lw/sweetfb_lh must match')
    parser.add_argument('--scale', type=int, choices=(2, 3),
                        help='with --ipad7: display and artwork scale (phone UI: 3)')
    parser.add_argument('--artwork-subtype', type=int,
                        help='with --ipad7: /product artwork-device-subtype (iPhone 8 Plus: 2208)')
    args = parser.parse_args()
    # The base tree takes its /product node from a donor ADT. Without the iPhone X one mkadt.py names (not in git: Apple's),
    # the donor given here serves: its /product replaces the base's anyway (ipad7/iphone11/iphone7_identity).
    donor = args.iphone11 or args.ipad7 or args.iphone7
    if donor and not os.path.exists(mkadt.D22AP_ADT):
        mkadt.D22AP_ADT = str(donor)
    canvas_override = tuple(int(v) for v in args.canvas.lower().split('x')) if args.canvas else None
    dram = (BANK2_BASE, min(args.bank2_mb << 20, BANK2_SIZE)) if args.bank2 else (DRAM_BASE, DRAM_SIZE)
    assert dram[0] + dram[1] <= PRAM_BASE or dram[0] >= PRAM_BASE + PRAM_SIZE, 'XNU RAM overlaps /pram'
    blob = adt.serialize(build(dram=dram, scale2=args.scale2, big=args.big, ipad=args.ipad7,
                              iphone11=args.iphone11, iphone7=args.iphone7,
                              canvas_override=canvas_override, scale_override=args.scale,
                              subtype_override=args.artwork_subtype))
    args.output.write_bytes(blob)
    print(f'{args.output}: {len(blob)} bytes')
