<!-- SPDX-License-Identifier: MIT-0 -->
# The boot screen

`splash=1` replaces the wall of text on the screen while the phone starts with Scion's sprout, a thin progress line and, under it, a few
dim lines of what the kernel is saying. **Off unless asked for** (the default is today's screen), and **not yet seen on a phone**: the
drawing is checked on a host and in QEMU (below), the phone's run is the open item.

```
sprout (centred, a little above the middle)
progress line (30 % of the width)
  the last four kernel lines, the newest brightest
```

## Turning it on and off

| | |
|---|---|
| `splash=1` | in the payload's command line (`devices/starlte/mkimage.sh -q ... splash=1`): the boot screen |
| `-v` | the log scrolls on the screen as it always did; `-v` wins over `splash=1` (and `mkimage.sh` without `-q` gives `-v`) |
| hold **Volume Down** while the phone starts | the same as `-v` for that run (the loader reads the key, the S9's GPA0 pin 4, as the kernel's key driver does, and gives the kernel `-v`) |
| `splash_lines=N` | how many kernel lines the line's second part is spread over (default 900): a guess, see below |

The loader's own log is unchanged: it goes to the log in the pstore RAM and over USB, whether or not the screen shows it.
Anything that ends the run badly (the kernel hung, did not start, came back) puts the text back on the screen.

## How it works

- **The loader** (`xnuloader/loader.c`) reads `splash=1` from the payload before it prints its first line and keeps its text off
  the screen from then on (`screen_quiet`). When the display is known it paints the ground, the sprout and the first of the line
  over the framebuffer, and moves the line at its stages (power-on to the payload: up to 25 %). Hold Volume Down or give `-v`:
  none of that happens.
- **The kernel** (`scion/xnu-8796:osfmk/console/scion_splash.c`, `commit 1517-scion-boot-screen of scion/xnu-8796`) is started by the loader in
  the console's text mode, as `-v` is, and takes the characters that would be drawn as text: each finished line goes to the dim lines
  and moves the progress line on. The line is a guess and says so: the kernel's lines, `splash_lines` of them, take it from 25 % to
  95 %; from the start of IOKit a timer lets it creep to 99 % while the system comes up; the first frame the display driver puts on
  the panel ends it (`scion_splash_end`), and nothing is drawn after that.
- **The picture** is Scion's own logo (the sprout and chip of `scion-project.github.io`'s `logo.svg`, without its background):
  `tools/scion-splash/scion-sprout.svg`, packed by `tools/scion-splash-logo.py` into `xnuloader/scion_splash_logo.h` (34 KB, a
  64-colour palette and a coverage byte per pixel, in runs). Nothing of Apple's: no logo, no font of theirs (the kernel's text is
  the console's own `iso_font`, as `-v` has it). The drawing (`scion_splash.h`) is one file for the loader and the kernel, and
  lays out everything from the screen's size, so they agree without talking.

## Looking at it without a phone

```sh
cc -O2 -I "$(tools/xnu-tree.py added)/osfmk/console" -o scion-splash-preview tools/scion-splash-preview.c
./scion-splash-preview -w 1440 -h 2960 -p 400 > shot.ppm     # the S9's panel, the line at 40 %
```

draws what the loader draws, with the same code. `qemu/run.sh` with `splash=1` among the arguments and a ramfb display shows the whole
path, loader and kernel; `qemu/screendump.sh` saves it.

## What is not known

- The phone: the key read (Volume Down while S-Boot hands over), the time the line takes on the phone, and how it looks on the panel.
- How good the progress guess is. 900 lines is a figure for the S9's default arguments; a run with more drivers says more.
- Framebuffer format: the sprout assumes the usual blue-green-red-reserved the S9's UEFI reports (the loader reads the GOP's format
  and swaps for red-green-blue; the kernel assumes blue-green-red).
