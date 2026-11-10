/* SPDX-License-Identifier: MIT-0 */
/*
 * scion-splash-preview: draws the boot screen the way the loader does, on the host, into a PPM, to look at it
 * (no phone, no QEMU): the same scion_splash.h and scion_splash_logo.h as the loader's, the same unpacking and scaling.
 *
 *   cc -O2 -I "$(tools/xnu-tree.py added)/osfmk/console" -o scion-splash-preview tools/scion-splash-preview.c && ./scion-splash-preview [-w 1440] [-h 2960] [-p 400] > shot.ppm
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../xnuloader/scion_splash_logo.h"
#include "scion_splash.h"                  /* the fork of XNU's osfmk/console/scion_splash.h: -I the directory (tools/xnu-tree.py added) */

int
main(int argc, char **argv)
{
	uint32_t w = 1440, h = 2960, p = 400, rgb = 0;
	for (int i = 1; i + 1 < argc; i += 2) {
		if (!strcmp(argv[i], "-w")) w = (uint32_t)atoi(argv[i + 1]);
		else if (!strcmp(argv[i], "-h")) h = (uint32_t)atoi(argv[i + 1]);
		else if (!strcmp(argv[i], "-p")) p = (uint32_t)atoi(argv[i + 1]);
	}
	uint8_t *fb = calloc((size_t)w * h, 4);
	struct scion_splash s;
	scion_splash_layout(&s, fb, w, h, w * 4, rgb, SPLASH_LOGO_HW);
	scion_splash_background(&s);
	scion_splash_logo(&s);
	scion_splash_bar(&s, p);
	printf("P6\n%u %u\n255\n", w, h);
	for (uint32_t y = 0; y < h; y++)
		for (uint32_t x = 0; x < w; x++) {
			uint32_t v = *(uint32_t *)(fb + (size_t)y * w * 4 + x * 4);
			putchar((v >> 16) & 255), putchar((v >> 8) & 255), putchar(v & 255);
		}
	return 0;
}
