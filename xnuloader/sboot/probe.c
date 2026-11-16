/* SPDX-License-Identifier: GPL-2.0-only
 * starlte stage-0 probe: paint the screen, print the S-Boot handoff on the
 * firmware framebuffer, then return so probe.S can request Download mode.
 * Based on ios-a5xelte shim/herolte/probe.c (wardevsa, herolte branch).
 */
#include <stdint.h>
#include "fdt.h"
#include "fbtext.h"

#define DOWNLOAD_DELAY_S 45U

static uint64_t counter(void)
{
    uint64_t value;
    __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r" (value));
    return value;
}

void star_probe_main(uintptr_t fdt, uint64_t el, uint64_t sctlr, uint64_t image)
{
    uint64_t midr, mpidr, mmfr0, pfr0, isar0, freq, start = 0, end = 0;
    uint32_t trig_before = star_fb_enable();

    /* First sign of life even if the text below is wrong: a teal screen. */
    star_fb_fill(0xff008080U);

    __asm__ volatile("mrs %0, midr_el1" : "=r" (midr));
    __asm__ volatile("mrs %0, mpidr_el1" : "=r" (mpidr));
    __asm__ volatile("mrs %0, id_aa64mmfr0_el1" : "=r" (mmfr0));
    __asm__ volatile("mrs %0, id_aa64pfr0_el1" : "=r" (pfr0));
    __asm__ volatile("mrs %0, id_aa64isar0_el1" : "=r" (isar0));
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r" (freq));

    unsigned row = 0;
    star_fb_text(row++, "STARLTE XNU PROBE 1");
    star_fb_hex(row++, "EL", el >> 2);
    star_fb_hex(row++, "SCTLR", sctlr);
    star_fb_hex(row++, "FDT", fdt);
    star_fb_hex(row++, "IMAGE", image);
    star_fb_hex(row++, "MIDR", midr);
    star_fb_hex(row++, "MPIDR", mpidr);
    star_fb_hex(row++, "MMFR0", mmfr0);
    star_fb_hex(row++, "PFR0", pfr0);
    star_fb_hex(row++, "ISAR0", isar0);
    star_fb_hex(row++, "CNTFRQ", freq);
    star_fb_hex(row++, "TRIG", trig_before);
    if (fdt && !(fdt & 7) && fdt_chosen_u64(fdt, "linux,initrd-start", &start) &&
        fdt_chosen_u64(fdt, "linux,initrd-end", &end)) {
        star_fb_hex(row++, "INITRD", start);
        star_fb_hex(row++, "INITRD END", end);
    } else {
        star_fb_text(row++, "INITRD ?");
    }
    /* MMFR0[23:20] TGran16: 0 means no 16 KiB pages (needs the 4 KiB XNU). */
    star_fb_text(row++, ((mmfr0 >> 20) & 15U) ? "16K PAGES: YES" : "16K PAGES: NO");

    if (!freq)
        freq = 26000000U;   /* Exynos9810 OSCCLK */
    uint64_t t0 = counter();
    unsigned shown = ~0U;
    for (;;) {
        uint64_t elapsed = (counter() - t0) / freq;
        if (elapsed >= DOWNLOAD_DELAY_S)
            return;
        unsigned left = DOWNLOAD_DELAY_S - (unsigned)elapsed;
        if (left != shown) {
            char line[] = "DOWNLOAD MODE IN 00 S";
            line[17] = (char)('0' + left / 10);
            line[18] = (char)('0' + left % 10);
            star_fb_text(row + 1, line);
            shown = left;
        }
    }
}
