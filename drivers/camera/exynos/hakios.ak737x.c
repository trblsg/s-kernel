// SPDX-License-Identifier: MIT-0
/*
 * The lens of the Galaxy S9's front camera (starlte, Exynos 9810): its motor's driver chip, an AKM AK737x, the module's
 * calibration EEPROM beside it, and an autofocus by contrast over the frames hakios.exynos_camera.c takes. Only with
 * boot-arg cam_af=N in a run with cam_s9=N on this phone: the camera's driver calls in here, and with cam_af absent
 * or 0 nothing below runs (no register of the bus, the chip or the pins is read or written) and the camera is as it
 * was. Every address and number is this phone's, from its device tree and the S9 kernel's actuator driver, read as
 * data.
 *
 * THE HARDWARE
 *   the motor      an AK737x (the device tree expects product 0x0C, an AK7372) at 0x0C (7 bits) on hsi2c@10900000.
 *                  8-bit register addresses:
 *                    0x00  the position's bits [8:1]
 *                    0x01  the position's bit 0, in bit 7
 *                    0x02  the mode: 0x00 active (the coil driven to the position), 0x20 sleep
 *                    0x03  reads the product: 0x09 AK7371, 0x0C AK7372, 0x0E AK7374
 *                  The position is 9 bits, 0 (the furthest: infinity) to 511 (the nearest: macro). Android's order
 *                  after the supplies: the product read, the first position written (120 for this module), 10 ms, the
 *                  mode to active; the mode to sleep before the supplies go.
 *   NEVER WRITTEN  The chip has a memory of its own for its settings, and Android's driver has a one-time path that
 *                  rewrites it (an unlock at 0xAE, the settings at 0x10-0x25, store commands written to 0x03). None
 *                  of that is here: lens_put() takes only 0x00, 0x01 and 0x02, and only the values above, and
 *                  refuses everything else whoever asks. 0x03 and 0x10 are read, for the log.
 *   its supply     VDDAF_2.8V_VT, the S2MPB02's LDO16 at 2.8 V: hakios.exynos_camera.c switches it with the sensor's
 *                  other supplies (Android's order), so the chip has power whenever the sensor has, and none when the
 *                  camera is down. Nothing here writes to the S2MPB02.
 *   the bus        hsi2c@10900000: the USI block of USI09 in PERIC1 ("hsi2c22"), 400 kHz; its mode word usi@10811034
 *                  ("i2c": 4); pins gpp5-4 (SCL) and gpp5-5 (SDA) of PERIC1's GPIO (0x10830020), function 2 while
 *                  the camera is up, function 0 (inputs) when it is down, no pull. In CMU_PERIC1 (0x10800000):
 *                  GATE_CLK_PERIC1_USI09_USI 0x2044, its reset synchroniser's clock 0x20A4, the block's IPCLK 0x2104
 *                  and PCLK 0x2108, QCH_CON_USI09_USI_QCH 0x3048, DIV_CLK_PERIC1_USI09_USI 0x1824. The sensor's own
 *                  bus is the neighbour on pins 2 and 3 of the same bank (hakios.exynos_camera.c)
 *   the EEPROM     the module's calibration memory at 0x51 on the same bus, 16-bit addresses, 16 KB. Read only, and
 *                  only ever as a random read with a repeated start (the two address bytes, no STOP, then the read):
 *                  nothing is stored by a memory of this kind without a STOP after a data byte. Read once a run: the
 *                  header (0x00-0x5F: the sections' addresses, the header's version at 0x30, the map's version at
 *                  0x40, the project's name at 0x4C) and the start of the focus section (0x100-0x11F: the lens's
 *                  position for infinity at 0x104 and for macro at 0x110, as the factory measured them). The module's
 *                  serial number (0xA8 on) is not read.
 *
 * THE STAGES (cam_af=N)
 *   1  a look: the bus, the product read, the registers 0x00-0x02 and 0x10 as found, the EEPROM's header and focus
 *      section. Nothing is written to the chip: the lens does not move.
 *   2  that, and the lens held where it is told: at cam_af_pos (120, Android's first position, unless said) from each
 *      power-up of the camera. With the stream (cam_s9=7): the middle's sharpness in the log, and sysctl
 *      kern.hakios.lens.go (the USB console's "sysctl kern.hakios.lens.go=N"): 0-511 the lens there, 1000 the table,
 *      the sharpness at every cam_af_coarse-th position of the travel, the lens put back after. cam_af_walk=1 makes
 *      the table once by itself when the first stream's exposure has settled.
 *   3  that, and the autofocus: when a stream's exposure has settled the travel is measured in steps of cam_af_coarse
 *      (32), then the stretch around the sharpest in steps of cam_af_fine (8), and the lens put at the sharpest. It
 *      stays there until the middle has been less than cam_af_drop per cent (60) as sharp as it was found for most of
 *      a second; then the search is made again. kern.hakios.lens.go=1001 asks for a search now.
 *   4  that, and iOS: the frames of /dev/hakios-camera carry the lens's position (0 to 1022, twice the chip's), and
 *      HAKIOS_CAMERA_FOCUS is taken: a = 0 to 1023 puts the lens there (half of it, the chip's 9 bits) and stops the
 *      kernel's own search until the stream ends (the capture device in user space searches by itself, a step at
 *      a time, and a search of its own would fight it); a = 0xffffffff (HAKIOS_CAMERA_NO_FOCUS) asks the kernel's
 *      search instead, at once, on a window: with b not 0 its middle is c across and d down, in 1024ths of the picture
 *      as read() gives it (a tap), with b 0 the picture's middle.
 *
 * THE MEASURE. A window of cam_af_win pixels square (384; the middle of the frame, or where a tap asked), read as
 * the WDMA wrote it (RAW10 in 16 bits): the sum of the squares of each pixel's difference from the next of its own
 * colour to the right and below (two pixels away: the Bayer pattern itself is no contrast), over the pixel count and
 * the square of the mean above black, so that a change of exposure changes it little. While a search or a table is
 * being made the exposure is held (the camera's driver is told), and each position is measured on the frame
 * cam_af_settle frames (2) after the one in which the lens was sent there: the frame exposing during the move is
 * smeared, and the chip settles in less than a frame. The lens is walked, at most 48 steps of its 511 in a write and
 * 3 ms between writes, never thrown; before the supplies go it is walked down to the furthest end of the search's
 * travel and the chip put to sleep.
 *
 * SEEN ON THE PHONE (12 November 2026, iOS 16.5 kernel, one scene: a wall with a framed picture about two metres off)
 *   stage 1  the chip answers: product 0x0C; as found after power-up its position registers 0 and its mode 0x40. The
 *            EEPROM answers: header "W08LLKF00PM", map "V003", project "000STARL"; the factory's positions 233
 *            (infinity) and 304 (macro), which are in the chip's own 9-bit steps: the wall was sharpest just past 233
 *   stage 2  the table, twice: the sharpness 300 at 0, 1000 at 240, 330 at 496, one clean peak; camshots at 0, 120, 240
 *            and 400 show it
 *   stage 3  two searches (the second asked by kern.hakios.lens.go=1001): both ended at 248, 71 frames (2.4 s) after
 *            they began, the stream at 30 frames a second throughout. The search-again path (the sharpness dropping)
 *            and stage 4 have not been seen: the phone lay still, and nothing in user space asked.
 *
 * Boot-args (all read only with cam_af not 0):
 *   cam_af=N          1 a look, 2 the lens held at cam_af_pos, 3 the autofocus, 4 that and HAKIOS_CAMERA_FOCUS (0 or
 *                     none: nothing)
 *   cam_af_pos=P      stage 2's position, and where the lens is put first at 3 and 4 (0-511; 120 unless said)
 *   cam_af_lo=P cam_af_hi=P   the travel searched (0 and 511 unless said)
 *   cam_af_coarse=S cam_af_fine=S   the search's two steps (32 and 8)
 *   cam_af_settle=F   frames let go by after a move before the measurement (1-6, 2 unless said)
 *   cam_af_win=W      the window's side in pixels (64-512, 384 unless said)
 *   cam_af_drop=PCT   the sharpness, in per cent of what the search found, below which it is made again (60)
 *   cam_af_walk=1     stage 2: the table once, by itself
 *   cam_af_x=X cam_af_y=Y   the window's middle when nobody has tapped, in 1024ths of the frame as the sensor gives
 *                     it (camshot's picture): 512 and 512, the middle, unless said
 *
 * Sources: the phone's device tree (exynos9810-starlte_eur_open_26.dts: hsi2c@10900000 and its fimc-is-actuator@0C and
 * front_eeprom@51, hsi2c22-bus, usi@10811034) and, of the S9 kernel, fimc-is-actuator-ak737x.c/.h (the registers, the
 * products, the order and the times), vendor/mcd/fimc-is-eeprom-front-3h1_v003.h and fimc-is-sec-define.c (the
 * EEPROM's map and its 16-bit addresses), cal-if/exynos9810/cmucal-sfr.c (CMU_PERIC1's registers)
 * (android_kernel_samsung_exynos9810, lineage-24.0; GPL: read for these facts, not copied).
 */
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <mach/mach_time.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.exynos_hsi2c.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/hakios.xnu.h>
#include <sys/hakios.exynos_soc.h>      /* (dev/hakios.drivers.h on the 16.5 line: which phone this kernel is for) */

/*
 * Two camera drivers share the configuration symbol EXYNOS_CAMERA and the kernel's three entry points: this one (with
 * hakios.ak737x.c and hakios.exynos_camera_rear.c), run on the Galaxy S9, and hakios.exynos_camera_star2.c, Anan Jaser's, run
 * on the Galaxy S9+. A kernel has one of them: the S9+'s configuration (HAKIOS_DEVICE_IS_STAR2LTE, written into
 * dev/hakios.drivers.h by tools/hakios-config.py) compiles this file to nothing, every other one compiles that file to
 * nothing. Bringing the two into one driver waits until both phones can be run (docs/exynos9810-cameras.md).
 */
#ifndef HAKIOS_DEVICE_IS_STAR2LTE

/* what hakios.exynos_camera.c calls (it declares them too) */
uint32_t hakios_ak737x_setup(uint32_t ipclk_mhz, int (*say)(const char *, ...));
void hakios_ak737x_up(void);
void hakios_ak737x_down(void);
bool hakios_ak737x_frame(vm_offset_t raw, uint32_t stride, uint32_t width, uint32_t height, uint64_t number, bool settled);
void hakios_ak737x_shot(vm_offset_t raw, uint32_t stride, uint32_t width, uint32_t height);
uint32_t hakios_ak737x_focus(void);
void hakios_ak737x_ask(uint32_t a, uint32_t b, uint32_t c, uint32_t d);
void hakios_ak737x_closed(void);
bool hakios_ak737x_searching(void);
void hakios_ak737x_rear(struct hakios_hsi2c *bus);

#define LENS                    0x0C
#define REG_POS_HIGH            0x00
#define REG_POS_LOW             0x01
#define REG_MODE                0x02
#define  MODE_ACTIVE            0x00
#define  MODE_SLEEP             0x20
#define REG_PRODUCT             0x03            /* read only here: written, it is the chip's store command */
#define REG_SETTING             0x10            /* read only here: the first of the chip's stored settings */
#define POS_MAX                 511u
#define FIRST_POS               120u
#define FIRST_DELAY_MS          10u
#define STEP_MAX                48u             /* the most the lens is moved by one write */
#define STEP_MS                 3u

#define EEPROM                  0x51
#define EEP_HEADER_BYTES        0x60
#define EEP_VERSION             0x30
#define EEP_MAP_VERSION         0x40
#define EEP_PROJECT             0x4C
#define EEP_FOCUS               0x100
#define EEP_FOCUS_BYTES         0x20
#define EEP_FOCUS_FAR           0x104
#define EEP_FOCUS_NEAR          0x110

#define NO_FOCUS                0xffffffffu     /* HAKIOS_CAMERA_NO_FOCUS */
#define GO_NOTHING              (-1)
#define GO_TABLE                1000
#define GO_SEARCH               1001
#define BLACK                   64u
#define WIN_MAX                 512u
#define SAMPLES                 40u

static struct hakios_hsi2c lens_bus = {
	.name = "cam-lens",
	.regs_pa = 0x10900000UL,                        /* hsi2c@10900000: the USI block of USI09 in PERIC1 */
	.usi_mode_pa = 0x10811034UL,                    /* usi@10811034: its mode word */
	.cmu_pa = 0x10800000UL, .cmu_size = 0x4000,     /* CMU_PERIC1 */
	.gates = { 0x2044, 0x20A4, 0x2104, 0x2108 }, .ngates = 4,
	.qch = 0x3048, .div = 0x1824, .mux = 0x0120,    /* DIV_CLK_PERIC1_USI09_USI; MUX_CLKCMU_PERIC1_IP_USER */
	.pins_pa = 0x10830000UL + 0x20,                 /* gpp5: SCL pin 4, SDA pin 5, function 2 */
	.pins_mask = 0xff0000u, .pins_want = 0x220000u,
	.ipclk_mhz = 400, .bus_hz = 400000,             /* clock-frequency = <400000> */
};

/*
 * The bus in use. The rear camera's lens (cam_rear=1) is the same kind of chip at the same address on another bus,
 * hsi2c@10870000, which it shares with the module's OIS controller and aperture driver: hakios.exynos_camera_rear.c
 * owns that bus (its clocks, pins and reset) and hands it over with hakios_ak737x_rear() before the setup. There the
 * device tree expects an AK7374 (0x0E; or an AK7371, 0x09), the first position is 120 as well, and the module's
 * calibration is not in an EEPROM on this bus (it is in an SPI flash): none is read.
 */
static struct hakios_hsi2c *lbus = &lens_bus;
static bool lens_rear;

static int (*say)(const char *, ...);
static uint32_t cam_af;                  /* cam_af */
static uint32_t arg_pos = FIRST_POS, arg_lo, arg_hi = POS_MAX, arg_coarse = 32, arg_fine = 8, arg_settle = 2, arg_win = 384;
static uint32_t arg_drop = 60, arg_walk;
static bool lens_mapped, lens_answers, lens_active, lens_looked, eeprom_looked, refused_said;
static bool lens_broken;                /* a write was not acknowledged: nothing more is written in this run */
static uint8_t lens_product;
static uint32_t lens_at = NO_FOCUS;          /* where the lens was last told to go (the chip's 0-511) */
static uint32_t lens_resume = NO_FOCUS;      /* where it is put at the next power-up */
static uint32_t win_x = 512, win_y = 512;       /* the window's middle in 1024ths of the frame as the sensor gives it */
static uint32_t arg_x = 512, arg_y = 512;       /* ... when nobody has tapped: cam_af_x, cam_af_y */
static uint32_t cal_far, cal_near;      /* the EEPROM's two positions as read (0: not read) */

/* for the USB console and whoever looks: kern.hakios.lens.* */
static int lens_position = -1, lens_sharpness, lens_level, lens_state, lens_go = GO_NOTHING, lens_searches;
SYSCTL_DECL(_kern_hakios);
SYSCTL_NODE(_kern_hakios, OID_AUTO, lens, CTLFLAG_RW | CTLFLAG_LOCKED, 0, "the Galaxy S9's front camera's lens (drivers/camera/exynos/hakios.ak737x.c, cam_af=N)");
SYSCTL_INT(_kern_hakios_lens, OID_AUTO, position, CTLFLAG_RD | CTLFLAG_KERN | CTLFLAG_LOCKED, &lens_position, 0, "where the lens was last sent, 0 (infinity) to 511 (macro); -1: nowhere");
SYSCTL_INT(_kern_hakios_lens, OID_AUTO, sharpness, CTLFLAG_RD | CTLFLAG_KERN | CTLFLAG_LOCKED, &lens_sharpness, 0, "the window's sharpness as last measured");
SYSCTL_INT(_kern_hakios_lens, OID_AUTO, level, CTLFLAG_RD | CTLFLAG_KERN | CTLFLAG_LOCKED, &lens_level, 0, "the window's mean above black as last measured, of 959");
SYSCTL_INT(_kern_hakios_lens, OID_AUTO, state, CTLFLAG_RD | CTLFLAG_KERN | CTLFLAG_LOCKED, &lens_state, 0, "0 off, 1 held, 2 waiting for the exposure, 3 the coarse search, 4 the fine search, 5 in focus, 6 the table");
SYSCTL_INT(_kern_hakios_lens, OID_AUTO, searches, CTLFLAG_RD | CTLFLAG_KERN | CTLFLAG_LOCKED, &lens_searches, 0, "the searches finished in this run");
SYSCTL_INT(_kern_hakios_lens, OID_AUTO, go, CTLFLAG_RW | CTLFLAG_KERN | CTLFLAG_LOCKED, &lens_go, 0, "set: 0-511 the lens there (cam_af=2 and up), 1000 the table of the travel, 1001 a search now (cam_af=3 and up); taken at the stream's next frame");

static void
wait_ms(uint32_t ms)
{
	uint64_t until;

	nanoseconds_to_absolutetime((uint64_t)ms * NSEC_PER_MSEC, &until);
	until += mach_absolute_time();
	while (mach_absolute_time() < until) {
	}
}

static uint32_t
lens_get(uint8_t reg, uint8_t *value)
{
	return hakios_hsi2c_read_reg(lbus, LENS, reg, value, 1);
}

/* what may be written to the chip: the position's two registers and the mode's two values. Nothing else, ever. */
static bool
lens_allowed(uint8_t reg, uint8_t value)
{
	switch (reg) {
	case REG_POS_HIGH:
		return true;
	case REG_POS_LOW:
		return (value & 0x7f) == 0;
	case REG_MODE:
		return value == MODE_ACTIVE || value == MODE_SLEEP;
	default:
		return false;
	}
}

/* The only way anything reaches the chip. */
static bool
lens_put(uint8_t reg, uint8_t value)
{
	uint8_t buf[2] = { reg, value };

	if (!lens_answers || lens_broken || cam_af < 2 || !lens_allowed(reg, value)) {
		if (!refused_said) {
			refused_said = true;
			say("ak737x: %#x = %#x refused: not a register and value this driver writes (or the chip was not recognised, "
			    "or cam_af is 1)\n", reg, value);
		}
		return false;
	}
	if (hakios_hsi2c_message(lbus, LENS, false, buf, 2, true) != 0) {
		say("ak737x: the write of %#x = %#x was not acknowledged (%#x): the lens is left alone for the rest of the run\n", reg,
		    value, lbus->last_error);
		lens_broken = true;
		return false;
	}
	return true;
}

/* the lens to a position, in one write of each register */
static bool
lens_position_put(uint32_t pos)
{
	if (pos > POS_MAX) {
		pos = POS_MAX;
	}
	if (!lens_put(REG_POS_HIGH, (uint8_t)(pos >> 1)) || !lens_put(REG_POS_LOW, (uint8_t)((pos & 1) << 7))) {
		return false;
	}
	lens_at = pos;
	lens_position = (int)pos;
	return true;
}

/* ... and walked there, a step at a time */
static bool
lens_walk(uint32_t target)
{
	if (target > POS_MAX) {
		target = POS_MAX;
	}
	if (lens_at == NO_FOCUS) {
		return lens_position_put(target);
	}
	while (lens_at != target) {
		uint32_t next = lens_at < target ? (target - lens_at > STEP_MAX ? lens_at + STEP_MAX : target) :
		    (lens_at - target > STEP_MAX ? lens_at - STEP_MAX : target);

		if (!lens_position_put(next)) {
			return false;
		}
		if (lens_at != target) {
			wait_ms(STEP_MS);
		}
	}
	return true;
}

/* len bytes of the EEPROM from a 16-bit address: the address, a repeated start, the read; eight bytes at a time */
static bool
eeprom_get(uint16_t address, uint8_t *buf, uint32_t len)
{
	for (uint32_t done = 0; done < len; done += 8) {
		uint8_t a[2] = { (uint8_t)((address + done) >> 8), (uint8_t)(address + done) };
		uint32_t n = len - done < 8 ? len - done : 8;

		if (hakios_hsi2c_message(lbus, EEPROM, false, a, 2, false) != 0 ||
		    hakios_hsi2c_message(lbus, EEPROM, true, buf + done, n, true) != 0) {
			return false;
		}
	}
	return true;
}

static void
hex_lines(const char *what, uint32_t address, const uint8_t *b, uint32_t len)
{
	for (uint32_t i = 0; i < len; i += 16) {
		char hex[16 * 3 + 1], text[17];

		for (uint32_t k = 0; k < 16; k++) {
			uint8_t v = i + k < len ? b[i + k] : 0;

			snprintf(hex + 3 * k, 4, "%02x ", v);
			text[k] = v >= 0x20 && v < 0x7f ? (char)v : '.';
		}
		text[16] = 0;
		say("ak737x: 1: %s %#06x: %s|%s|\n", what, address + i, hex, text);
	}
}

static uint32_t
word_le(const uint8_t *b)
{
	return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static void
eeprom_look(void)
{
	static uint8_t header[EEP_HEADER_BYTES], focus[EEP_FOCUS_BYTES];
	char version[12], map[5], project[9];

	if (eeprom_looked) {
		return;
	}
	eeprom_looked = true;
	if (!eeprom_get(0, header, sizeof(header))) {
		say("ak737x: 1: the EEPROM at %#x does not answer (%#x): no calibration read\n", EEPROM, lbus->last_error);
		return;
	}
	hex_lines("EEPROM", 0, header, sizeof(header));
	for (uint32_t i = 0; i < 11; i++) {
		uint8_t v = header[EEP_VERSION + i];

		version[i] = v >= 0x20 && v < 0x7f ? (char)v : '.';
	}
	version[11] = 0;
	for (uint32_t i = 0; i < 4; i++) {
		uint8_t v = header[EEP_MAP_VERSION + i];

		map[i] = v >= 0x20 && v < 0x7f ? (char)v : '.';
	}
	map[4] = 0;
	for (uint32_t i = 0; i < 8; i++) {
		uint8_t v = header[EEP_PROJECT + i];

		project[i] = v >= 0x20 && v < 0x7f ? (char)v : '.';
	}
	project[8] = 0;
	say("ak737x: 1: THE EEPROM ANSWERS: header version \"%s\" (0x30), map version \"%s\" (0x40), project \"%s\" (0x4C)\n",
	    version, map, project);
	if (!eeprom_get(EEP_FOCUS, focus, sizeof(focus))) {
		say("ak737x: 1: the EEPROM's focus section (%#x) could not be read (%#x)\n", EEP_FOCUS, lbus->last_error);
		return;
	}
	hex_lines("EEPROM", EEP_FOCUS, focus, sizeof(focus));
	cal_far = word_le(focus + (EEP_FOCUS_FAR - EEP_FOCUS));
	cal_near = word_le(focus + (EEP_FOCUS_NEAR - EEP_FOCUS));
	say("ak737x: 1: the factory's focus positions, as 32-bit little-endian words: infinity (0x104) %u (%#x), macro (0x110) %u "
	    "(%#x); the search's travel stays cam_af_lo to cam_af_hi (%u to %u)\n", cal_far, cal_far, cal_near, cal_near,
	    arg_lo, arg_hi);
}

/* the rear camera's lens: its bus, powered and reset by its owner before hakios_ak737x_up() */
void
hakios_ak737x_rear(struct hakios_hsi2c *bus)
{
	lbus = bus;
	lens_rear = true;
}

/* the stage and its numbers; the camera's own `say' for the log. 0: nothing asked, nothing done. */
uint32_t
hakios_ak737x_setup(uint32_t ipclk_mhz, int (*say_fn)(const char *, ...))
{
	say = say_fn;
	if (!PE_parse_boot_argn("cam_af", &cam_af, sizeof(cam_af)) || cam_af == 0) {
		cam_af = 0;
		return 0;
	}
	if (cam_af > 4) {
		cam_af = 4;
	}
	lens_bus.ipclk_mhz = ipclk_mhz;         /* (the rear camera's bus has its owner's) */
	PE_parse_boot_argn("cam_af_pos", &arg_pos, sizeof(arg_pos));
	PE_parse_boot_argn("cam_af_lo", &arg_lo, sizeof(arg_lo));
	PE_parse_boot_argn("cam_af_hi", &arg_hi, sizeof(arg_hi));
	if (arg_hi > POS_MAX || arg_lo >= arg_hi) {
		arg_lo = 0;
		arg_hi = POS_MAX;
	}
	if (arg_pos > POS_MAX) {
		arg_pos = FIRST_POS;
	}
	PE_parse_boot_argn("cam_af_coarse", &arg_coarse, sizeof(arg_coarse));
	if (arg_coarse < 4 || arg_coarse > 128) {
		arg_coarse = 32;
	}
	while ((arg_hi - arg_lo) / arg_coarse + 1 > SAMPLES) {
		arg_coarse *= 2;
	}
	PE_parse_boot_argn("cam_af_fine", &arg_fine, sizeof(arg_fine));
	if (arg_fine < 1 || arg_fine >= arg_coarse) {
		arg_fine = arg_coarse >= 8 ? arg_coarse / 4 : 1;
	}
	PE_parse_boot_argn("cam_af_settle", &arg_settle, sizeof(arg_settle));
	if (arg_settle < 1 || arg_settle > 6) {
		arg_settle = 2;
	}
	PE_parse_boot_argn("cam_af_win", &arg_win, sizeof(arg_win));
	if (arg_win < 64 || arg_win > WIN_MAX) {
		arg_win = 384;
	}
	arg_win &= ~1u;
	PE_parse_boot_argn("cam_af_drop", &arg_drop, sizeof(arg_drop));
	if (arg_drop < 10 || arg_drop > 95) {
		arg_drop = 60;
	}
	PE_parse_boot_argn("cam_af_walk", &arg_walk, sizeof(arg_walk));
	PE_parse_boot_argn("cam_af_x", &arg_x, sizeof(arg_x));
	PE_parse_boot_argn("cam_af_y", &arg_y, sizeof(arg_y));
	if (arg_x > 1023 || arg_y > 1023) {
		arg_x = arg_y = 512;
	}
	win_x = arg_x;
	win_y = arg_y;
	lens_resume = arg_pos;
	return cam_af;
}

/* ---- the measure ---- */

static uint16_t win_pixels[(WIN_MAX + 2) * (WIN_MAX + 2)];
static bool win_tapped;

/* the window's sharpness (see the top) and its mean above black */
static uint32_t
sharpness(vm_offset_t raw, uint32_t stride, uint32_t width, uint32_t height, uint32_t *level)
{
	uint32_t side = arg_win, x0, y0, n;
	uint64_t squares = 0, sum = 0, mean;

	if (side + 2 > width || side + 2 > height) {
		side = ((width < height ? width : height) - 2) & ~1u;
	}
	x0 = win_x * width / 1024;
	y0 = win_y * height / 1024;
	x0 = x0 > side / 2 ? x0 - side / 2 : 0;
	y0 = y0 > side / 2 ? y0 - side / 2 : 0;
	x0 = (x0 + side + 2 > width ? width - side - 2 : x0) & ~1u;
	y0 = (y0 + side + 2 > height ? height - side - 2 : y0) & ~1u;
	/* out of the frame's uncached mapping a line at a time */
	for (uint32_t y = 0; y < side + 2; y++) {
		memcpy(win_pixels + (size_t)y * (side + 2), (const void *)(raw + (uint64_t)(y0 + y) * stride + 2 * (uint64_t)x0),
		    (size_t)(side + 2) * 2);
	}
	for (uint32_t y = 0; y < side; y++) {
		const uint16_t *line = win_pixels + (size_t)y * (side + 2), *below = line + 2 * (size_t)(side + 2);

		for (uint32_t x = 0; x < side; x++) {
			int32_t v = line[x] & 0x3ff, dx = (int32_t)(line[x + 2] & 0x3ff) - v, dy = (int32_t)(below[x] & 0x3ff) - v;

			squares += (uint64_t)(dx * dx + dy * dy);
			sum += (uint32_t)v > BLACK ? (uint32_t)v - BLACK : 0;
		}
	}
	n = side * side;
	mean = sum / n;
	*level = (uint32_t)mean;
	lens_level = (int)mean;
	squares = squares * 65536 / n / (mean * mean + 1);
	lens_sharpness = squares > 0x7fffffff ? 0x7fffffff : (int)squares;
	return (uint32_t)lens_sharpness;
}

/* ---- the camera's power-up and power-down ---- */

/* after the sensor's supplies (VDDAF among them) are on and the sensor has answered */
void
hakios_ak737x_up(void)
{
	uint8_t id = 0, found[3] = { 0, 0, 0 }, setting = 0;

	if (cam_af == 0) {
		return;
	}
	if (!lens_mapped && !lens_rear) {
		say("ak737x: 1: the lens's bus: hsi2c@10900000 (USI09 of PERIC1), pins gpp5-4 and gpp5-5\n");
		if (!hakios_hsi2c_map(&lens_bus, say)) {
			cam_af = 0;
			return;
		}
	}
	lens_mapped = true;
	if (!lens_rear) {
		hakios_hsi2c_power(&lens_bus);          /* its clocks, the mode word, the pins to function 2 */
		hakios_hsi2c_reset(&lens_bus);
	}
	wait_ms(1);
	if (lens_get(REG_PRODUCT, &id) != 0) {
		wait_ms(10);
		if (lens_get(REG_PRODUCT, &id) != 0) {
			if (!lens_looked) {
				say("ak737x: 1: nothing answers at %#x (%#x; INT_STAT: 0x200 no ack, 0x400 no device, 0x800 time-out): no lens "
				    "in this run\n", LENS, lbus->last_error);
			}
			lens_looked = true;
			lens_answers = false;
			return;
		}
	}
	lens_product = id;
	lens_answers = !lens_broken && (id == 0x09 || id == 0x0C || id == 0x0E);
	if (!lens_looked) {
		(void)lens_get(REG_POS_HIGH, &found[0]);
		(void)lens_get(REG_POS_LOW, &found[1]);
		(void)lens_get(REG_MODE, &found[2]);
		(void)lens_get(REG_SETTING, &setting);
		say("ak737x: 1: THE LENS'S CHIP ANSWERS at %#x: product %#x (%s); as found: position registers %#x %#x (position %u), "
		    "mode %#x (0 active, 0x20 sleep), setting 0x10 = %#x; %llu transfers, %llu errors on the bus\n", LENS, id,
		    id == 0x0C ? (lens_rear ? "AK7372" : "AK7372, the one the device tree expects") : id == 0x09 ? "AK7371" :
	    id == 0x0E ? (lens_rear ? "AK7374, the one the device tree expects" : "AK7374") :
		    "NOT an AK737x this driver knows: nothing will be written to it", found[0], found[1],
		    ((uint32_t)found[0] << 1) | (found[1] >> 7), found[2], setting, lbus->transfers, lbus->errors);
		lens_looked = true;
	}
	if (!lens_rear) {
		eeprom_look();
	}
	if (cam_af < 2 || !lens_answers) {
		return;
	}
	/* Android's order: the first position, 10 ms, active. The lens starts where the last run of it left off. */
	lens_at = NO_FOCUS;
	if (!lens_position_put(lens_resume > arg_hi && cam_af >= 3 ? arg_hi : lens_resume)) {
		return;
	}
	wait_ms(FIRST_DELAY_MS);
	if (!lens_put(REG_MODE, MODE_ACTIVE)) {
		return;
	}
	lens_active = true;
	say("ak737x: 2: the lens at %u (of %u: 0 infinity, %u macro), the chip active\n", lens_at, POS_MAX, POS_MAX);
}

/* before the supplies go (also on the run's way out: nothing here sleeps) */
void
hakios_ak737x_down(void)
{
	if (cam_af == 0 || !lens_mapped) {
		return;
	}
	if (lens_active) {
		uint32_t was = lens_at;

		lens_resume = lens_at;
		if (lens_answers && !lens_broken) {
			(void)lens_walk(lens_at > arg_lo + STEP_MAX ? arg_lo + STEP_MAX : lens_at);
			wait_ms(STEP_MS);
			(void)lens_put(REG_MODE, MODE_SLEEP);
		}
		lens_active = false;
		lens_state = 0;
		say("ak737x: the lens walked down from %u to %u and the chip put to sleep%s\n", was, lens_at,
		    lens_answers && !lens_broken ? "" : " (NOT acknowledged)");
	}
	/* "off_i2c": the two pins back to inputs, as the sensor's bus is left (the rear camera's bus: its owner does it) */
	if (!lens_rear) {
		*(volatile uint32_t *)lens_bus.pins &= ~lens_bus.pins_mask;
	}
}

/* ---- the search ---- */

enum { ST_OFF, ST_HELD, ST_WAIT, ST_COARSE, ST_FINE, ST_FOCUSED, ST_TABLE };

static struct {
	int state;
	uint64_t measure_at;            /* the frame the next measurement is made on */
	uint64_t began, found_at, waited_from, said_at;
	uint32_t from, to, step, back;  /* the scan under way; where the lens goes back to after a table */
	uint32_t pos[SAMPLES], score[SAMPLES], level[SAMPLES], n;
	uint32_t best, best_score;
	uint32_t found_score, found_n;  /* in focus: the sharpness there, the mean of the first three measurements */
	uint32_t low;                   /* measurements in a row under the drop */
	bool reader;                    /* user space sets the position (HAKIOS_CAMERA_FOCUS): no search of the kernel's */
	bool walked;                    /* cam_af_walk's table was made */
} search;

/* asked from user space (write()'s thread): taken by the camera's thread at its next frame */
static volatile uint32_t ask_position = NO_FOCUS;       /* 0-1023 */
static volatile uint32_t ask_search, ask_window, ask_x, ask_y;

static void
scan_begin(int state, uint32_t from, uint32_t to, uint32_t step, uint64_t number)
{
	search.state = state;
	search.from = from;
	search.to = to;
	search.step = step;
	search.n = 0;
	(void)lens_walk(from);
	search.measure_at = number + 1 + arg_settle + 1;        /* (a longer way than a step: a frame more) */
	lens_state = state;
}

static void
scan_say(const char *what)
{
	say("ak737x: %s: %u positions from %u to %u in steps of %u; the window's level %u at the first, %u at the last\n", what,
	    search.n, search.from, search.to, search.step, search.n ? search.level[0] : 0, search.n ? search.level[search.n - 1] : 0);
	for (uint32_t i = 0; i < search.n; i += 10) {
		char table[10 * 20 + 1];
		uint32_t len = 0;

		table[0] = 0;
		for (uint32_t k = i; k < search.n && k < i + 10; k++) {
			len += (uint32_t)snprintf(table + len, sizeof(table) - len, " %u:%u", search.pos[k], search.score[k]);
		}
		say("ak737x: %s, position:sharpness:%s\n", what, table);
	}
}

static uint32_t
scan_best(void)
{
	uint32_t best = 0;

	for (uint32_t i = 1; i < search.n; i++) {
		if (search.score[i] > search.score[best]) {
			best = i;
		}
	}
	search.best = search.pos[best];
	search.best_score = search.score[best];
	return best;
}

/*
 * A frame of the stream, from the camera's thread: `number' counts the sensor's frames, `settled' says the exposure
 * is at its aim. true: a search or a table is under way and the exposure is to be left as it is.
 */
bool
hakios_ak737x_frame(vm_offset_t raw, uint32_t stride, uint32_t width, uint32_t height, uint64_t number, bool settled)
{
	uint32_t level = 0, score, position;
	int go;

	if (cam_af < 2 || !lens_active || !lens_answers || lens_broken) {
		return false;
	}
	if (search.state == ST_OFF) {
		/* a stream has begun */
		search.state = cam_af >= 3 && !search.reader ? ST_WAIT : ST_HELD;
		search.waited_from = number;
		search.said_at = 0;
		search.measure_at = number + 20;
		lens_state = search.state;
	}
	if (cam_af >= 4) {
		position = __atomic_exchange_n(&ask_position, NO_FOCUS, __ATOMIC_ACQ_REL);
		if (position != NO_FOCUS) {
			if (!search.reader) {
				say("ak737x: 4: a position asked through /dev/hakios-camera (%u of 1023): the lens is the reader's until the "
				    "stream ends\n", position);
			}
			search.reader = true;
			search.state = ST_HELD;
			lens_state = ST_HELD;
			(void)lens_walk((position > 1023 ? 1023 : position) / 2);
			return false;
		}
		if (__atomic_exchange_n(&ask_search, 0, __ATOMIC_ACQ_REL)) {
			win_tapped = __atomic_load_n(&ask_window, __ATOMIC_ACQUIRE) != 0;
			/* read() turns the frame half a turn: the window's place turned back */
			win_x = win_tapped ? 1023 - (__atomic_load_n(&ask_x, __ATOMIC_ACQUIRE) & 1023) : arg_x;
			win_y = win_tapped ? 1023 - (__atomic_load_n(&ask_y, __ATOMIC_ACQUIRE) & 1023) : arg_y;
			search.reader = false;
			say("ak737x: 4: a search asked through /dev/hakios-camera, the window's middle at %u, %u of 1024 of the sensor's "
			    "frame%s\n", win_x, win_y, win_tapped ? " (a tap)" : "");
			scan_begin(ST_COARSE, arg_lo, arg_hi, arg_coarse, number);
			search.began = number;
			return true;
		}
	}
	go = __atomic_exchange_n(&lens_go, GO_NOTHING, __ATOMIC_ACQ_REL);
	if (go >= 0 && go <= (int)POS_MAX) {
		say("ak737x: kern.hakios.lens.go=%d: the lens from %u to %d, held there\n", go, lens_at, go);
		(void)lens_walk((uint32_t)go);
		search.state = ST_HELD;
		lens_state = ST_HELD;
		search.measure_at = number + 1 + arg_settle;
		search.said_at = 0;                 /* (its sharpness said once it has settled) */
		return false;
	} else if (go == GO_TABLE || (arg_walk && !search.walked && cam_af == 2 && search.state == ST_HELD && (settled ||
	    number - search.waited_from > 45))) {
		search.walked = true;
		search.back = lens_at;
		search.began = number;
		scan_begin(ST_TABLE, arg_lo, arg_hi, arg_coarse, number);
		return true;
	} else if (go == GO_SEARCH && cam_af >= 3) {
		search.reader = false;
		search.began = number;
		scan_begin(ST_COARSE, arg_lo, arg_hi, arg_coarse, number);
		return true;
	}

	switch (search.state) {
	case ST_HELD:
		/* the lens where it was told: its sharpness for whoever looks, and in the log now and then */
		if (number < search.measure_at || number % 3 != 0) {
			return false;
		}
		score = sharpness(raw, stride, width, height, &level);
		if (search.said_at == 0 || number - search.said_at >= 300) {
			search.said_at = number;
			say("ak737x: 2: the lens held at %u: sharpness %u (level %u of 959) at frame %llu\n", lens_at, score, level, number);
		}
		return false;
	case ST_WAIT:
		/* before a search: the exposure at its aim (or a second and a half gone by) */
		if (!settled && number - search.waited_from < 45) {
			return false;
		}
		search.began = number;
		scan_begin(ST_COARSE, arg_lo, arg_hi, arg_coarse, number);
		return true;
	case ST_COARSE:
	case ST_FINE:
	case ST_TABLE:
		if (number < search.measure_at) {
			return true;
		}
		score = sharpness(raw, stride, width, height, &level);
		if (search.n < SAMPLES) {
			search.pos[search.n] = lens_at;
			search.score[search.n] = score;
			search.level[search.n] = level;
			search.n++;
		}
		if (lens_at + search.step <= search.to && search.n < SAMPLES) {
			(void)lens_walk(lens_at + search.step);
			search.measure_at = number + 1 + arg_settle;
			return true;
		}
		if (search.state == ST_TABLE) {
			scan_say("2: THE TABLE of the travel");
			(void)scan_best();
			say("ak737x: 2: the sharpest of the table at %u (%u); the lens back to %u, held\n", search.best, search.best_score, search.back);
			(void)lens_walk(search.back);
			search.state = ST_HELD;
			lens_state = ST_HELD;
			search.measure_at = number + 1 + arg_settle + 1;
			search.said_at = 0;
			return false;
		}
		if (search.state == ST_COARSE) {
			uint32_t lo, hi;

			scan_say("3: the coarse search");
			(void)scan_best();
			lo = search.best > arg_lo + arg_coarse - arg_fine ? search.best - (arg_coarse - arg_fine) : arg_lo;
			hi = search.best + (arg_coarse - arg_fine) < arg_hi ? search.best + (arg_coarse - arg_fine) : arg_hi;
			scan_begin(ST_FINE, lo, hi, arg_fine, number);
			return true;
		}
		scan_say("3: the fine search");
		(void)scan_best();
		(void)lens_walk(search.best);
		search.state = ST_FOCUSED;
		lens_state = ST_FOCUSED;
		search.found_at = search.said_at = number;
		search.found_score = search.found_n = search.low = 0;
		search.measure_at = number + 1 + arg_settle + 1;
		lens_searches++;
		say("ak737x: 3: IN FOCUS at %u (sharpness %u there in the search), %llu frames after the search began%s\n", search.best,
		    search.best_score, number - search.began, win_tapped ? ", on the window asked for" : "");
		return false;
	case ST_FOCUSED:
		if (number < search.measure_at || number % 3 != 0) {
			return false;
		}
		score = sharpness(raw, stride, width, height, &level);
		if (search.found_n < 3) {
			search.found_score += score;
			if (++search.found_n == 3) {
				search.found_score /= 3;
				say("ak737x: 3: the lens at %u: sharpness %u (level %u), the measure to watch; a search again below %u\n", lens_at,
				    search.found_score, level, search.found_score * arg_drop / 100);
			}
			return false;
		}
		if (number - search.said_at >= 300) {
			search.said_at = number;
			say("ak737x: 3: in focus at %u: sharpness %u now, %u when found (level %u)\n", lens_at, score, search.found_score, level);
		}
		search.low = (uint64_t)score * 100 < (uint64_t)search.found_score * arg_drop ? search.low + 1 : 0;
		if (search.low >= 8 && number - search.found_at >= 60 && !search.reader) {
			say("ak737x: 3: the middle has been under %u %% of the sharpness found (%u now, %u then) for %u measurements: "
			    "a search again\n", arg_drop, score, search.found_score, search.low);
			search.state = ST_WAIT;
			lens_state = ST_WAIT;
			search.waited_from = number;
		}
		return false;
	default:
		return false;
	}
}

/* a search is waiting to begin or under way (or a table): a picture taken now would be of a lens on its way */
bool
hakios_ak737x_searching(void)
{
	/* (in focus, but the frames since the lens was put there not yet gone by: still on its way) */
	return cam_af >= 2 && lens_active && (search.state == ST_WAIT || search.state == ST_COARSE || search.state == ST_FINE ||
	       search.state == ST_TABLE || (search.state == ST_FOCUSED && search.found_n == 0));
}

/* a camshot's frame: its sharpness in the log, with where the lens was */
void
hakios_ak737x_shot(vm_offset_t raw, uint32_t stride, uint32_t width, uint32_t height)
{
	uint32_t level = 0, score;

	if (cam_af < 2 || !lens_answers || lens_at == NO_FOCUS) {
		return;
	}
	score = sharpness(raw, stride, width, height, &level);
	say("ak737x: camshot: the lens at %u: sharpness %u of the window (%u pixels square, level %u of 959)\n", lens_at, score, arg_win,
	    level);
}

/* the stream has ended (from the camera's thread, before it powers down): the next one begins anew, the kernel's own */
void
hakios_ak737x_closed(void)
{
	search.state = ST_OFF;
	search.reader = false;
	win_x = arg_x;
	win_y = arg_y;
	win_tapped = false;
	lens_state = 0;
	__atomic_store_n(&ask_position, NO_FOCUS, __ATOMIC_RELEASE);
	__atomic_store_n(&ask_search, 0, __ATOMIC_RELEASE);
}

/* the frames' "focus": the lens's position in the protocol's 0-1023, at stage 4; no lens otherwise, as before */
uint32_t
hakios_ak737x_focus(void)
{
	uint32_t position = lens_at;

	return cam_af >= 4 && lens_active && position != NO_FOCUS ? position * 2 : NO_FOCUS;
}

/* HAKIOS_CAMERA_FOCUS, from write(): see the top (stage 4) */
void
hakios_ak737x_ask(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
	if (cam_af < 4) {
		return;
	}
	if (a == NO_FOCUS) {
		__atomic_store_n(&ask_window, b != 0, __ATOMIC_RELEASE);
		__atomic_store_n(&ask_x, c, __ATOMIC_RELEASE);
		__atomic_store_n(&ask_y, d, __ATOMIC_RELEASE);
		__atomic_store_n(&ask_search, 1, __ATOMIC_RELEASE);
	} else {
		__atomic_store_n(&ask_position, a > 1023 ? 1023 : a, __ATOMIC_RELEASE);
	}
}

#endif /* HAKIOS_DEVICE_IS_STAR2LTE */
