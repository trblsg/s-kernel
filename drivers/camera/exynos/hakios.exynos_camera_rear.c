// SPDX-License-Identifier: MIT-0
/*
 * The Galaxy S9's rear camera (starlte, Exynos 9810): a Samsung S5K2L3 (Android's camera 0, "fimc-is_sensor_2l3"), its
 * supplies, clock, reset and bus, and its register tables. hakios.exynos_camera.c, the front camera's driver, does the
 * rest (the ISPPRE domain, the CSIS, its DMA, the frames, /dev/hakios-camera) and calls in here for the rear camera's own
 * parts. Only with boot-arg cam_rear=1 in a run with cam_s9=N on this phone: with cam_rear absent or 0 nothing below
 * runs, no register named here is read or written, and the front camera is as it was. Every address and number is this
 * phone's, from its device tree and the S9 kernel's camera driver (fimc-is2), read as data.
 *
 * THE HARDWARE
 *   the sensor     0x10 (7 bits; 0x20 as Samsung's node name writes it) on hsi2c@10860000, "I2C_CAM0" of PERIC1 (an
 *                  HSI2C of its own: no USI mode word), 400 kHz; its pins gpc0-0 (SCL) and gpc0-1 (SDA) of PERIC1's GPIO
 *                  (0x10830060), function 2 with the pins' pull-ups (PUD 3) while it is on ("hsi2c0-bus"), inputs
 *                  without a pull when off ("hsi2c0-bus-in"). In CMU_PERIC1: GATE_CLK_PERIC1_I2C_CAM0 0x2020, its reset
 *                  synchroniser's clock 0x2004, IPCLK 0x2060, PCLK 0x2064, QCH_CON_I2C_CAM0_QCH 0x3008,
 *                  DIV_CLK_PERIC1_I2C_CAM0 0x1800. 16-bit register addresses, MIPI CCS's map as the front sensor's:
 *                  the model ID at 0x0000 (16 bits), the revision at 0x0002, the frame count at 0x0005
 *   reset          gpg2-6 of PERIC0's GPIO (0x104300C0), an output, high = running. On this board it is the OIS
 *                  controller's reset as well (the module file: "star1: use cam_reset and ois_reset in common")
 *   MCLK           CIS_CLK0 of CMU_TOP: MUX_CLKCMU_CIS_CLK0 0x1018 [0] (0: OSCCLK, 26 MHz), the divider 0x1814 [4:0],
 *                  the gate 0x2018 (bit 20 manual, 21 on), its Q-channel 0x3004; 26 MHz (EXT_CLK_Mhz). Out on gpc1-4
 *                  of PERIC1's GPIO (0x10830080), function 2, no pull ("fimc_is_mclk0_fn"); an output, low, pulled
 *                  down, when off ("fimc_is_mclk0_out")
 *   the supplies   seven on the S2MPB03 (0x56 on hsi2c@14300000, USI00 of the CMGP block: mode word usi@14212000,
 *                  "i2c"; pins gpm0-0 (SCL) and gpm1-0 (SDA) of CMGP's GPIO, 0x14220000 and 0x14220020, function 2, no
 *                  pull; in CMU_CMGP: GATE_CLK_USI_CMGP00 0x2008, IPCLK 0x2090, PCLK 0x2094, the reset synchroniser's
 *                  clock 0x2068, QCH_CON_USI_CMGP00_QCH 0x3044, the divider 0x1808, the mux 0x100C). Its registers:
 *                  0x00 the ID, 0x01 status, 0x02 control, 0x03-0x09 LDO1-7 (bit 7 on, [5:0] the voltage), 0x0A
 *                  LDO_SLEW1, 0x0B LDO_SLEW2. The voltage: LDO1, 2 and 4 700 mV + 12.5 mV n, LDO3 700 mV + 25 mV n,
 *                  LDO5, 6 and 7 1.8 V + 25 mV n:
 *                    VDDD_1.0V_CAM      LDO1, 0x03, 1.025 V = 26 (the module file asks 1.025 V of 0.975-1.025)
 *                    VDDQ_1.1V_CAM      LDO2, 0x04, 1.1 V   = 32
 *                    VDDIO_1.8V_CAM     LDO3, 0x05, 1.8 V   = 44
 *                    VDDD_RET_1.0V_CAM  LDO4, 0x06, 1.025 V = 26 (the module file asks 1.025 V when the camera is on)
 *                    VDDA_2.15V_CAM     LDO5, 0x07, 2.15 V  = 14
 *                    VDDA_2.95V_CAM     LDO6, 0x08, 2.95 V  = 46
 *                    VDD_VM_2.8V_OIS    LDO7, 0x09, 2.8 V   = 40 (the OIS controller's)
 *                  and what Linux's driver sets once from the device tree's s2mpb03_pmic@56: LDO_SLEW1 bit 0 cleared
 *                  ("disable_ldo2_remote_sense"), LDO_SLEW2 bit 1 set ("slew2,dram_dsch2" = 1) and [7:6], [5:4]
 *                  cleared ("slew2,l5_slew", "slew2,l6_slew" = 0); written here only where a bit is found otherwise.
 *                  Its low-voltage LDOs are made from VDD_EXT_1P3_PB03, the S2MPB02's BUCK1 (always on in the device
 *                  tree): hakios.exynos_camera.c looks at it with its own BUCK2 and switches it on if it is off.
 *                  Three more on the S2MPB02 (0x59 on hsi2c@10920000, the front camera's chip: its writes stay in
 *                  hakios.exynos_camera.c's one function, which takes these three registers only with cam_rear=1):
 *                    VDDAF_COMMON_CAM   LDO14, 0x2B, 3.3 V = 108 (0x6C): the lens's motor (and the aperture's)
 *                    VDDD_1.8V_OIS      LDO10, 0x27, 1.8 V = 48 (the OIS controller's)
 *                    VDDD_2.8V_OIS      LDO15, 0x2C, 2.8 V = 88 (the OIS controller's)
 *   power-up       (the module file's NORMAL ON with CONFIG_OIS_USE, fimc-is-device-module-2l3.c, star's): reset low; VDDD_RET_1.0V_CAM at
 *                  1.025 V; VDDA_2.95V_CAM; VDDA_2.15V_CAM; VDDD_1.0V_CAM at 1.025 V; VDDAF_COMMON_CAM;
 *                  VDDIO_1.8V_CAM; VDDQ_1.1V_CAM, 1 ms; (the OIS's three: VDDD_1.8V_OIS, 100 us, VDD_VM_2.8V_OIS,
 *                  VDDD_2.8V_OIS, 1 ms); the bus pins to I2C; MCLK out on its pin, 1.5 ms; reset high, 10 ms. Each
 *                  supply is given 200 us (the S2MPB03's enable time is 150 us). CIS_CLK0 is switched on in CMU_TOP
 *                  before the first supply, as the front camera's is
 *   power-down     (NORMAL OFF): VDDAF_COMMON_CAM; VDDQ_1.1V_CAM; VDDD_1.0V_CAM; VDDA_2.95V_CAM; VDDA_2.15V_CAM; MCLK's
 *                  pin an output, low; reset low; the bus pins inputs; (VDDD_2.8V_OIS, VDD_VM_2.8V_OIS,
 *                  VDDD_1.8V_OIS); VDDD_RET_1.0V_CAM; VDDIO_1.8V_CAM; CIS_CLK0 as found. Android keeps
 *                  VDDD_RET_1.0V_CAM at 0.7 V between uses (the sensor's retention): not here, everything goes off
 *
 * THE OIS, THE APERTURE, THE LENS. The module also holds an optical stabiliser's controller (0x24 on hsi2c@10870000),
 * an aperture's driver (0x4C there) and the lens's motor (0x0C there: hakios.ak737x.c). The OIS controller's three
 * supplies are switched with the sensor's, in Android's order, in every run with cam_rear=1: its reset is the sensor's
 * (one pin for both on this board), and a controller held out of reset without its supplies would be fed through that
 * pin. Powered, it sits idle on the firmware in its own flash; nothing is written to it unless cam_rear_ois=2 asks for
 * its servo, and nothing ever to its flash. The aperture stays where it rests unless cam_rear_ap asks. Both are
 * further down, with the bus they share.
 *
 * Limits. Every write to the S2MPB03 goes through one function that takes only the seven LDO registers at the values
 * above (on or off) and the two slew registers' bits named above, and reads each back; a write that does not read back
 * stops the run's writing and everything is powered down in the power-down's order. A supply found on is left as it is
 * and left on at the end; one found on at another voltage stops the run before anything is written. Nothing else of the
 * chip is written. Every write is in the log.
 *
 * SEEN ON THE PHONE (12 November 2026, the iOS 27 kernel; the phone on a desk, its back up, a room with a window)
 *   the supplies   the S2MPB03 answers (ID 2); its seven voltage codes and both slew registers were the device tree's
 *                  already (the recovery's Linux leaves them so), BUCK1 on: only the enables were written
 *   the sensor     model ID 0x20C3, revision 0xA5 (setfile B's). It streams 2016 x 1512 at 30 frames a second WITHOUT
 *                  the firmware patch; CSIS0 took 1862 frames in a row with no link error
 *   the frames     at CLKCMU_ISPPRE_BUS 233 MHz every frame overflowed; at 466 MHz none. The test pattern generator
 *                  does not reach memory on that kernel for either camera, so none was seen through CSIS0
 *   the stream     30.1 frames a second, 0 lost, 0 FIFO errors; read() through the uncached mapping gave 5.8 frames a
 *                  second (102 ms to copy a frame out), with cam_s9_cached=1 450 of 450 frames at 29.6 a second
 *   the lens       an AK7374 (0x0E) at 0x0C. The table over the travel: sharpness 1173 at 0, 3100 at 160, 228 at 480;
 *                  the search ended at 160 three times running (another scene later: 120 and 144)
 *   the OIS        answers at 0x24: status 0x13, firmware "P84KL01", 26 MHz, error 0. The servo on in still mode: status
 *                  2; off again and the gyro asleep before the supplies went. With the phone still, of 0x0000-0x025F only
 *                  the 16 bits at 0x0070 changed between two looks; 0x0248 and 0x024A did not (stored offsets, it seems,
 *                  not the gyro's output). Nobody has moved the phone with the servo on yet
 *   the aperture   an AK7372 (0x0C) at 0x4C. Without 0xAE the iris did not move. With it, at one exposure: the middle of
 *                  the frame 96 of 959 at rest, 45 after F2.4, 101 after the close put it back at F1.5
 * Not done: the module's calibration (an SPI flash, "FROM", on SPI_CAM0: fimc-is-from-rear-2l3_v008.h) is not read; the
 * other modes of the sensor (4032 x 3024 with the phase-detection channel, the 120 and 240 frames a second ones) have
 * no table here; nothing in iOS's user space has read the device.
 *
 * Boot-args:
 *   cam_rear=1         the camera of this run (cam_s9=N's steps, camshot, the stream) is the rear one
 *   cam_rear_ois=N     the stabiliser: 1 its status, version and clock read, 2 that and its servo on (0 or none: powered
 *                      and left alone)
 *   cam_rear_ois_mode=M  with 2: 0 still (the default), 1 video, 5 centring
 *   cam_rear_ap=N      the iris: 1 a look, 15 or 24 put at F1.5 or F2.4 at each power-up (0 or none: left where it rests)
 *   cam_rear_addr=A    the sensor's 7-bit I2C address, if not 0x10 (the device tree's)
 *
 * Sources: the phone's device tree (exynos9810-starlte_eur_open_26.dts: hsi2c@10860000 and fimc-is-2l3@10,
 * fimc-is_sensor_2l3@20, hsi2c0-bus and hsi2c0-bus-in, the fimc_is_mclk0 pin states, hsi2c@14300000 and
 * s2mpb03_pmic@56 with its regulators, hsi2c34-bus, usi@14212000, s2mpb02_pmic@59's ldo10, ldo14, ldo15 and buck1) and,
 * of the S9 kernel, fimc-is-device-module-2l3.c (the order and the times), fimc-is-cis-2l3.c/.h,
 * drivers/regulator/s2mpb03.c and include/linux/regulator/s2mpb03.h, include/linux/mfd/s2mpb02-private.h,
 * cal-if/exynos9810/cmucal-sfr.c (CMU_PERIC1's, CMU_CMGP's and CMU_TOP's registers), pinctrl-exynos.c
 * (android_kernel_samsung_exynos9810, lineage-24.0; GPL: read for these facts, not copied).
 */
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <mach/mach_time.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.exynos_hsi2c.h>
#include <sys/hakios.exynos_soc.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/hakios.xnu.h>

/*
 * Two camera drivers share the configuration symbol EXYNOS_CAMERA and the kernel's three entry points: this one (with
 * hakios.ak737x.c and hakios.exynos_camera_rear.c), run on the Galaxy S9, and hakios.exynos_camera_star2.c, Anan Jaser's, run
 * on the Galaxy S9+. A kernel has one of them: the S9+'s configuration (HAKIOS_DEVICE_IS_STAR2LTE, written into
 * dev/hakios.drivers.h by tools/hakios-config.py) compiles this file to nothing, every other one compiles that file to
 * nothing. Bringing the two into one driver waits until both phones can be run (docs/exynos9810-cameras.md).
 */
#ifndef HAKIOS_DEVICE_IS_STAR2LTE

/* what hakios.exynos_camera.c calls (it declares them too) */
uint32_t hakios_s9rear_setup(uint32_t ipclk_mhz, int (*say)(const char *, ...));
bool hakios_s9rear_map(vm_offset_t peric0, vm_offset_t peric1, vm_offset_t cmu_top);
bool hakios_s9rear_look(void);
bool hakios_s9rear_up(void);
void hakios_s9rear_down(void);
struct hakios_hsi2c *hakios_s9rear_bus(uint8_t *addr);
bool hakios_s9rear_mode(void);
struct hakios_hsi2c *hakios_s9rear_lens_bus(void);
void hakios_s9rear_lens_up(void);
void hakios_s9rear_lens_down(void);
void hakios_s9rear_poll(uint64_t frame);
/* ... and what it gives: the rear camera's three supplies on the S2MPB02 (its chip, its one function) */
bool hakios_s9cam_pb02_on(uint32_t which);
void hakios_s9cam_pb02_off(uint32_t which);
#define PB02_VDDAF_COMMON       0
#define PB02_VDDD_18_OIS        1
#define PB02_VDDD_28_OIS        2

/* ---- the S2MPB03 ---- */

#define PB03                    0x56
#define PB03_ID                 0x00
#define PB03_STATUS             0x01
#define PB03_CTRL               0x02
#define PB03_SLEW1              0x0A
#define PB03_SLEW2              0x0B
#define PB03_ON                 0x80
#define PB03_VSEL               0x3f
#define SLEW1_REMOTE_SENSE      0x01            /* LDO2's remote sense: cleared ("disable_ldo2_remote_sense") */
#define SLEW2_MASK              0xf2            /* [7:6] LDO5's slew, [5:4] LDO6's, bit 1 "dram_dsch2" */
#define SLEW2_WANT              0x02

static const struct pb03_ldo {
	const char *name;
	uint8_t reg, vsel;
	bool ois;
} pb03_ldos[] = {
	{ "VDDD_1.0V_CAM (LDO1, 1.025 V)", 0x03, 26, false },
	{ "VDDQ_1.1V_CAM (LDO2, 1.1 V)", 0x04, 32, false },
	{ "VDDIO_1.8V_CAM (LDO3, 1.8 V)", 0x05, 44, false },
	{ "VDDD_RET_1.0V_CAM (LDO4, 1.025 V)", 0x06, 26, false },
	{ "VDDA_2.15V_CAM (LDO5, 2.15 V)", 0x07, 14, false },
	{ "VDDA_2.95V_CAM (LDO6, 2.95 V)", 0x08, 46, false },
	{ "VDD_VM_2.8V_OIS (LDO7, 2.8 V)", 0x09, 40, true },
};
#define VDDD            0
#define VDDQ            1
#define VDDIO           2
#define VDDD_RET        3
#define VDDA_215        4
#define VDDA_295        5
#define VDD_VM_OIS      6
#define NPB03           (sizeof(pb03_ldos) / sizeof(pb03_ldos[0]))

static struct hakios_hsi2c pb03_bus = {
	.name = "cam-s2mpb03",
	.regs_pa = 0x14300000UL,                        /* hsi2c@14300000: USI00 of CMGP (hsi2c34) */
	.usi_mode_pa = 0x14212000UL,                    /* usi@14212000: its mode word ("i2c") */
	.cmu_pa = EXYNOS_CMU_CMGP_PA, .cmu_size = 0x4000,
	/* GATE_CLK_USI_CMGP00, USI_CMGP00 IPCLK and PCLK, its RSTnSYNC */
	.gates = { 0x2008, 0x2090, 0x2094, 0x2068 }, .ngates = 4,
	.qch = 0x3044, .div = 0x1808, .mux = 0x100C,
	.pins_pa = 0,                                   /* gpm0-0 and gpm1-0: a bank each, set in pb03_pins() */
	.ipclk_mhz = 400, .bus_hz = 100000,             /* the device tree gives this bus no speed: Linux's default */
};

/* ---- the sensor's bus ---- */

#define SENSOR                  0x10            /* the device tree's (fimc-is-2l3@10, reg = <16>): 7 bits */
#define MODEL_2L3               0x20C3

static struct hakios_hsi2c sensor_bus = {
	.name = "cam-2l3",
	.regs_pa = 0x10860000UL,                        /* hsi2c@10860000: I2C_CAM0 of PERIC1 (hsi2c0) */
	.usi_mode_pa = 0,                               /* an HSI2C of its own: no USI block, no mode word */
	.cmu_pa = EXYNOS_CMU_PERIC1_PA, .cmu_size = 0x4000,
	/* GATE_CLK_PERIC1_I2C_CAM0, RSTNSYNC_CLK_PERIC1_I2C_CAM0, I2C_CAM0 IPCLK, I2C_CAM0 PCLK */
	.gates = { 0x2020, 0x2004, 0x2060, 0x2064 }, .ngates = 4,
	.qch = 0x3008, .div = 0x1800, .mux = 0x0120,    /* QCH_CON_I2C_CAM0_QCH; DIV_CLK_PERIC1_I2C_CAM0 */
	.pins_pa = EXYNOS_GPIO_PERIC1_PA + 0x60,        /* gpc0: SCL pin 0, SDA pin 1, function 2 */
	.pins_mask = 0xffu, .pins_want = 0x22u,
	.ipclk_mhz = 400, .bus_hz = 400000,             /* clock-frequency = <400000> */
};

/* ---- pins and the clock ---- */

#define GPG2                    0x0C0           /* PERIC0: the reset, pin 6 */
#define GPC0                    0x060           /* PERIC1: the sensor's bus, pins 0 and 1 */
#define GPC1                    0x080           /* PERIC1: MCLK0, pin 4 */
#define RESET_PIN               6
#define MCLK_PIN                4
#define PIN_PUD                 8               /* a bank's pull register: a nibble a pin, 0 none, 1 down, 3 up */
#define CMGP_GPIO_PA            0x14220000UL    /* gpm0 at +0x000, gpm1 at +0x020 */

#define CIS0_MUX                0x1018          /* CMU_TOP: [0]: 0 OSCCLK */
#define CIS0_DIV                0x1814          /* [4:0]: the ratio less one */
#define CIS0_GATE               0x2018          /* bit 20 manual, bit 21 on */
#define CIS0_QCH                0x3004          /* DMYQCH_CON_DFTMUX_TOP_QCH_CIS_CLK0 */
#define GATE_MANUAL             (1u << 20)
#define GATE_ON                 (1u << 21)

static struct hakios_hsi2c lens_bus;     /* (the lens's bus: further down) */
static int (*say)(const char *, ...);
static uint32_t cam_rear, cam_rear_ois = 0, cam_rear_ois_mode, cam_rear_ap;
static uint8_t sensor_addr = SENSOR;
static vm_offset_t peric0, peric1, cmu_top, cmgp_gpio;
static uint8_t pb03_found[NPB03], slew_found[2];
static bool pb03_mine[NPB03], pb02_mine[3];
static bool pb03_broken;                /* a write did not read back: no more writing in this run */
static bool looked, powered;
static uint32_t gate_found, mux_found, div_found, qch_found;
static bool clock_mine;
static uint32_t cmgp_con_found[2], cmgp_pud_found[2];
static bool cmgp_pins_mine;

static inline uint32_t
rd(vm_offset_t base, uint32_t off)
{
	return *(volatile uint32_t *)(base + off);
}

static inline void
wr(vm_offset_t base, uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(base + off) = v;
}

static void
spin_us(uint32_t us)
{
	uint64_t until;

	nanoseconds_to_absolutetime((uint64_t)us * NSEC_PER_USEC, &until);
	until += mach_absolute_time();
	while (mach_absolute_time() < until) {
	}
}

static void
pin_out(vm_offset_t bank, uint32_t number, bool high)
{
	uint32_t data = rd(bank, 4);

	wr(bank, 4, high ? data | (1u << number) : data & ~(1u << number));
	wr(bank, 0, (rd(bank, 0) & ~(0xfu << (4 * number))) | (1u << (4 * number)));
}

static void
pin_function(vm_offset_t bank, uint32_t number, uint32_t function)
{
	wr(bank, 0, (rd(bank, 0) & ~(0xfu << (4 * number))) | (function << (4 * number)));
}

static void
pin_pull(vm_offset_t bank, uint32_t number, uint32_t pull)
{
	wr(bank, PIN_PUD, (rd(bank, PIN_PUD) & ~(0xfu << (4 * number))) | (pull << (4 * number)));
}

static uint32_t
pb03_get(uint8_t reg, uint8_t *value)
{
	return hakios_hsi2c_read_reg(&pb03_bus, PB03, reg, value, 1);
}

/* what may be written to the S2MPB03: an LDO at the device tree's voltage, on or off; the slew registers' named bits */
static bool
pb03_allowed(uint8_t reg, uint8_t value)
{
	for (uint32_t i = 0; i < NPB03; i++) {
		if (reg == pb03_ldos[i].reg) {
			return (value & PB03_VSEL) == pb03_ldos[i].vsel && (value & 0x40) == (pb03_found[i] & 0x40) &&
			       true;
		}
	}
	if (reg == PB03_SLEW1) {
		return value == (uint8_t)(slew_found[0] & ~SLEW1_REMOTE_SENSE);
	}
	if (reg == PB03_SLEW2) {
		return value == (uint8_t)((slew_found[1] & ~SLEW2_MASK) | SLEW2_WANT);
	}
	return false;
}

/* The only way anything reaches the S2MPB03: a register and a value pb03_allowed() takes; read back; said. */
static bool
pb03_put(uint8_t reg, uint8_t value, const char *name)
{
	uint8_t buf[2] = { reg, value }, back = 0;

	if (pb03_broken || !looked || !pb03_allowed(reg, value)) {
		if (!pb03_broken) {
			say("exynos_camera: rear: S2MPB03 %#x (%s) = %#x refused: not a value this driver writes\n", reg, name, value);
		}
		return false;
	}
	if (hakios_hsi2c_message(&pb03_bus, PB03, false, buf, 2, true) != 0 || pb03_get(reg, &back) != 0 || back != value) {
		say("exynos_camera: rear: S2MPB03 %#x (%s): wrote %#x, read back %#x (bus %#x): no more writing for this run\n",
		    reg, name, value, back, pb03_bus.last_error);
		pb03_broken = true;
		return false;
	}
	say("exynos_camera: rear: S2MPB03 write: %#x = %#x (%s), read back the same\n", reg, value, name);
	return true;
}

static bool
pb03_on(uint32_t i)
{
	uint8_t off = (uint8_t)((pb03_found[i] & 0x40) | pb03_ldos[i].vsel);

	if (pb03_found[i] & PB03_ON) {
		say("exynos_camera: rear: %s was on already (%#x): left as it is\n", pb03_ldos[i].name, pb03_found[i]);
		return true;
	}
	/* the voltage first, off; then on at it */
	if ((pb03_found[i] & PB03_VSEL) != pb03_ldos[i].vsel && !pb03_put(pb03_ldos[i].reg, off, pb03_ldos[i].name)) {
		return false;
	}
	pb03_mine[i] = true;
	if (!pb03_put(pb03_ldos[i].reg, (uint8_t)(PB03_ON | off), pb03_ldos[i].name)) {
		return false;
	}
	spin_us(200);
	return true;
}

static void
pb03_off(uint32_t i)
{
	if (pb03_mine[i]) {
		bool was_broken = pb03_broken;

		pb03_broken = false;    /* switching off is always tried */
		if (pb03_put(pb03_ldos[i].reg, (uint8_t)((pb03_found[i] & 0x40) | pb03_ldos[i].vsel), pb03_ldos[i].name)) {
			pb03_mine[i] = false;
		}
		pb03_broken = pb03_broken || was_broken;
	}
}

static bool
pb02_on(uint32_t which)
{
	if (!hakios_s9cam_pb02_on(which)) {
		return false;
	}
	pb02_mine[which] = true;
	return true;
}

static void
pb02_off(uint32_t which)
{
	if (pb02_mine[which]) {
		hakios_s9cam_pb02_off(which);
		pb02_mine[which] = false;
	}
}

/* the S2MPB03's bus pins: gpm0-0 (SCL) and gpm1-0 (SDA) to function 2, no pull ("hsi2c34-bus"); as found at the end */
static void
pb03_pins(bool on)
{
	for (uint32_t b = 0; b < 2; b++) {
		vm_offset_t bank = cmgp_gpio + 0x20 * b;

		if (on) {
			if (!cmgp_pins_mine) {
				cmgp_con_found[b] = rd(bank, 0);
				cmgp_pud_found[b] = rd(bank, PIN_PUD);
			}
			if ((cmgp_con_found[b] & 0xfu) != 2 || (cmgp_pud_found[b] & 0xfu) != 0) {
				wr(bank, 0, (cmgp_con_found[b] & ~0xfu) | 2);
				wr(bank, PIN_PUD, cmgp_pud_found[b] & ~0xfu);
			}
		}
	}
	if (on && !cmgp_pins_mine) {
		say("exynos_camera: rear: the S2MPB03's bus pins: gpm0-0 CON %x -> %x, PUD %x -> %x; gpm1-0 CON %x -> %x, PUD %x -> %x "
		    "(function 2, no pull)\n", cmgp_con_found[0], rd(cmgp_gpio, 0), cmgp_pud_found[0], rd(cmgp_gpio, PIN_PUD),
		    cmgp_con_found[1], rd(cmgp_gpio + 0x20, 0), cmgp_pud_found[1], rd(cmgp_gpio + 0x20, PIN_PUD));
		cmgp_pins_mine = true;
	}
}

uint32_t
hakios_s9rear_setup(uint32_t ipclk_mhz, int (*say_fn)(const char *, ...))
{
	uint32_t addr = 0;

	say = say_fn;
	if (!PE_parse_boot_argn("cam_rear", &cam_rear, sizeof(cam_rear)) || cam_rear == 0) {
		cam_rear = 0;
		return 0;
	}
	cam_rear = 1;
	PE_parse_boot_argn("cam_rear_ois", &cam_rear_ois, sizeof(cam_rear_ois));
	if (cam_rear_ois > 2) {
		cam_rear_ois = 2;
	}
	PE_parse_boot_argn("cam_rear_ois_mode", &cam_rear_ois_mode, sizeof(cam_rear_ois_mode));
	if (cam_rear_ois_mode != 1 && cam_rear_ois_mode != 5) {
		cam_rear_ois_mode = 0;
	}
	PE_parse_boot_argn("cam_rear_ap", &cam_rear_ap, sizeof(cam_rear_ap));
	if (cam_rear_ap != 1 && cam_rear_ap != 15 && cam_rear_ap != 24) {
		cam_rear_ap = 0;
	}
	lens_bus.ipclk_mhz = ipclk_mhz;
	if (PE_parse_boot_argn("cam_rear_addr", &addr, sizeof(addr)) && addr >= 0x08 && addr <= 0x77 &&
	    (addr < 0x50 || addr > 0x57)) {
		sensor_addr = (uint8_t)addr;
	}
	pb03_bus.ipclk_mhz = sensor_bus.ipclk_mhz = ipclk_mhz;
	return cam_rear;
}

struct hakios_hsi2c *
hakios_s9rear_bus(uint8_t *addr)
{
	*addr = sensor_addr;
	return &sensor_bus;
}

bool
hakios_s9rear_map(vm_offset_t peric0_va, vm_offset_t peric1_va, vm_offset_t cmu_top_va)
{
	if (!cam_rear) {
		return false;
	}
	if (cmgp_gpio != 0) {
		return true;
	}
	peric0 = peric0_va;
	peric1 = peric1_va;
	cmu_top = cmu_top_va;
	say("exynos_camera: rear: 1: map the two buses (USI00 of CMGP for the S2MPB03, I2C_CAM0 of PERIC1 for the sensor) and "
	    "CMGP's GPIO\n");
	if (!hakios_hsi2c_map(&pb03_bus, say) || !hakios_hsi2c_map(&sensor_bus, say)) {
		return false;
	}
	cmgp_gpio = ml_io_map(CMGP_GPIO_PA, 0x1000);
	return cmgp_gpio != 0;
}

static void
look_pins(const char *when)
{
	say("exynos_camera: rear: %s: reset gpg2-6 CON %x DAT %u; MCLK0 gpc1-4 CON %x DAT %u PUD %x; bus gpc0-0/1 CON %x %x PUD "
	    "%x %x\n", when, (rd(peric0 + GPG2, 0) >> (4 * RESET_PIN)) & 0xf, (rd(peric0 + GPG2, 4) >> RESET_PIN) & 1,
	    (rd(peric1 + GPC1, 0) >> (4 * MCLK_PIN)) & 0xf, (rd(peric1 + GPC1, 4) >> MCLK_PIN) & 1,
	    (rd(peric1 + GPC1, PIN_PUD) >> (4 * MCLK_PIN)) & 0xf, rd(peric1 + GPC0, 0) & 0xf, (rd(peric1 + GPC0, 0) >> 4) & 0xf,
	    rd(peric1 + GPC0, PIN_PUD) & 0xf, (rd(peric1 + GPC0, PIN_PUD) >> 4) & 0xf);
}

/* step 1's reads: the pins, CIS_CLK0, the S2MPB03's ID, its seven LDOs and two slew registers as found */
bool
hakios_s9rear_look(void)
{
	uint8_t id = 0, status = 0, ctrl = 0;

	if (!cam_rear || cmgp_gpio == 0) {
		return false;
	}
	look_pins("1");
	say("exynos_camera: rear: 1: CIS_CLK0: mux %x, divider %x, gate %x, Q-channel %x\n", rd(cmu_top, CIS0_MUX),
	    rd(cmu_top, CIS0_DIV), rd(cmu_top, CIS0_GATE), rd(cmu_top, CIS0_QCH));
	hakios_hsi2c_power(&pb03_bus);
	pb03_pins(true);
	hakios_hsi2c_reset(&pb03_bus);
	if (pb03_get(PB03_ID, &id) != 0) {
		say("exynos_camera: rear: 1: the S2MPB03 does not answer at %#x (%x)\n", PB03, pb03_bus.last_error);
		return false;
	}
	(void)pb03_get(PB03_STATUS, &status);
	(void)pb03_get(PB03_CTRL, &ctrl);
	say("exynos_camera: rear: 1: S2MPB03 ID %#x, status %#x, control %#x\n", id, status, ctrl);
	for (uint32_t i = 0; i < NPB03; i++) {
		if (pb03_get(pb03_ldos[i].reg, &pb03_found[i]) != 0) {
			say("exynos_camera: rear: 1: %s cannot be read (%x)\n", pb03_ldos[i].name, pb03_bus.last_error);
			return false;
		}
		say("exynos_camera: rear: 1: %s: %#x (%s, voltage code %u, the device tree's %u)\n", pb03_ldos[i].name,
		    pb03_found[i], (pb03_found[i] & PB03_ON) ? "on" : "off", pb03_found[i] & PB03_VSEL, pb03_ldos[i].vsel);
	}
	if (pb03_get(PB03_SLEW1, &slew_found[0]) != 0 || pb03_get(PB03_SLEW2, &slew_found[1]) != 0) {
		say("exynos_camera: rear: 1: the slew registers cannot be read (%x)\n", pb03_bus.last_error);
		return false;
	}
	say("exynos_camera: rear: 1: LDO_SLEW1 %#x (bit 0, LDO2's remote sense: the device tree wants 0), LDO_SLEW2 %#x (the "
	    "device tree wants bit 1 set, [7:4] 0)\n", slew_found[0], slew_found[1]);
	looked = true;
	return true;
}

static void
clock_on(void)
{
	gate_found = rd(cmu_top, CIS0_GATE);
	mux_found = rd(cmu_top, CIS0_MUX);
	div_found = rd(cmu_top, CIS0_DIV);
	qch_found = rd(cmu_top, CIS0_QCH);
	if ((mux_found & 1) != 0 || (div_found & 0x1f) != 0) {
		wr(cmu_top, CIS0_MUX, mux_found & ~1u);
		wr(cmu_top, CIS0_DIV, div_found & ~0x1fu);
	}
	wr(cmu_top, CIS0_QCH, qch_found | 3);
	wr(cmu_top, CIS0_GATE, gate_found | GATE_MANUAL | GATE_ON);
	clock_mine = true;
	say("exynos_camera: rear: 2: CIS_CLK0: mux %x -> %x, divider %x -> %x, gate %x -> %x (26 MHz from OSCCLK)\n", mux_found,
	    rd(cmu_top, CIS0_MUX), div_found, rd(cmu_top, CIS0_DIV), gate_found, rd(cmu_top, CIS0_GATE));
}

/* The power-down, in the module file's order; only what this run switched on. */
void
hakios_s9rear_down(void)
{
	bool left = false;

	if (!powered) {
		return;
	}
	pb02_off(PB02_VDDAF_COMMON);
	spin_us(10);
	pb03_off(VDDQ);
	pb03_off(VDDD);
	spin_us(10);
	pb03_off(VDDA_295);
	pb03_off(VDDA_215);
	pin_out(peric1 + GPC1, MCLK_PIN, false);
	pin_pull(peric1 + GPC1, MCLK_PIN, 1);
	pin_out(peric0 + GPG2, RESET_PIN, false);
	spin_us(10);
	pin_function(peric1 + GPC0, 0, 0);
	pin_function(peric1 + GPC0, 1, 0);
	pin_pull(peric1 + GPC0, 0, 0);
	pin_pull(peric1 + GPC0, 1, 0);
	pb02_off(PB02_VDDD_28_OIS);
	pb03_off(VDD_VM_OIS);
	pb02_off(PB02_VDDD_18_OIS);
	pb03_off(VDDD_RET);
	spin_us(100);
	pb03_off(VDDIO);
	if (clock_mine) {
		wr(cmu_top, CIS0_GATE, gate_found);
		wr(cmu_top, CIS0_QCH, qch_found);
		wr(cmu_top, CIS0_DIV, div_found);
		wr(cmu_top, CIS0_MUX, mux_found);
		clock_mine = false;
	}
	powered = false;
	for (uint32_t i = 0; i < NPB03; i++) {
		left = left || pb03_mine[i];
	}
	say("exynos_camera: rear: powered down: MCLK and reset low, the bus pins inputs, the S2MPB03's LDOs and the S2MPB02's "
	    "three %s; CIS_CLK0's gate %x again\n", (left || pb02_mine[0] || pb02_mine[1] || pb02_mine[2]) ?
	    "NOT ALL OFF (a write failed)" : "off as found", gate_found);
}

static uint32_t
sensor_get(uint16_t reg, uint8_t *buf, uint32_t len)
{
	uint8_t address[2] = { (uint8_t)(reg >> 8), (uint8_t)reg };
	uint32_t error = hakios_hsi2c_message(&sensor_bus, sensor_addr, false, address, 2, false);

	return error ? error : hakios_hsi2c_message(&sensor_bus, sensor_addr, true, buf, len, true);
}

/* the bus scanned, reads only (a one-byte read at each address): who is there when the sensor does not answer */
static void
bus_scan(void)
{
	char list[3 * 112 + 1];
	uint32_t n = 0, len = 0;
	uint8_t b;

	list[0] = 0;
	for (uint8_t a = 0x08; a <= 0x77; a++) {
		if (hakios_hsi2c_message(&sensor_bus, a, true, &b, 1, true) == 0) {
			n++;
			len += (uint32_t)snprintf(list + len, sizeof(list) - len, " %02x", a);
		}
	}
	say("exynos_camera: rear: 2: the bus scanned (a one-byte read at 0x08-0x77, nothing written): %u address(es) answer%s\n",
	    n, n ? list : ": none");
	say("exynos_camera: rear: 2: the sensor's HSI2C (0x10860000): CTL %x CONF %x INT_STAT %x TRANS_STATUS %x TIMING_FS3 %x; "
	    "CMU_PERIC1: divider %x, gates %x %x %x %x, Q-channel %x; %llu transfers, %llu errors, the last %x\n",
	    rd(sensor_bus.regs, 0x00), rd(sensor_bus.regs, 0x40), rd(sensor_bus.regs, 0x24), rd(sensor_bus.regs, 0x50),
	    rd(sensor_bus.regs, 0x68), rd(sensor_bus.cmu, sensor_bus.div), rd(sensor_bus.cmu, sensor_bus.gates[0]),
	    rd(sensor_bus.cmu, sensor_bus.gates[1]), rd(sensor_bus.cmu, sensor_bus.gates[2]),
	    rd(sensor_bus.cmu, sensor_bus.gates[3]), rd(sensor_bus.cmu, sensor_bus.qch), sensor_bus.transfers,
	    sensor_bus.errors, sensor_bus.last_error);
}

/* step 2: the rear camera's power-up in Android's order, and its ID read; false (and powered down) if it does not answer */
bool
hakios_s9rear_up(void)
{
	uint8_t model[2] = { 0, 0 }, revision = 0, frames = 0, mode = 0;
	uint32_t error, id;

	if (!cam_rear || !looked) {
		return false;
	}
	for (uint32_t i = 0; i < NPB03; i++) {
		if ((pb03_found[i] & PB03_ON) && (pb03_found[i] & PB03_VSEL) != pb03_ldos[i].vsel) {
			say("exynos_camera: rear: 2: %s is on at another voltage: someone else has it; nothing is written\n",
			    pb03_ldos[i].name);
			return false;
		}
	}
	say("exynos_camera: rear: 2: the rear camera's power-up (Android's order for star: VDDD_RET, VDDA 2.95, VDDA 2.15, VDDD, "
	    "VDDAF_COMMON, VDDIO, VDDQ, then the OIS controller's three: VDDD_1.8V_OIS, VDD_VM_2.8V_OIS, VDDD_2.8V_OIS)\n");
	powered = true;
	/* what Linux's S2MPB03 driver sets once from the device tree, where it is not so already */
	if ((slew_found[0] & SLEW1_REMOTE_SENSE) != 0 &&
	    !pb03_put(PB03_SLEW1, (uint8_t)(slew_found[0] & ~SLEW1_REMOTE_SENSE), "LDO_SLEW1: LDO2's remote sense off")) {
		goto fail;
	}
	if ((slew_found[1] & SLEW2_MASK) != SLEW2_WANT &&
	    !pb03_put(PB03_SLEW2, (uint8_t)((slew_found[1] & ~SLEW2_MASK) | SLEW2_WANT), "LDO_SLEW2: the device tree's")) {
		goto fail;
	}
	clock_on();
	pin_out(peric0 + GPG2, RESET_PIN, false);
	if (!pb03_on(VDDD_RET) || !pb03_on(VDDA_295) || !pb03_on(VDDA_215) || !pb03_on(VDDD) ||
	    !pb02_on(PB02_VDDAF_COMMON) || !pb03_on(VDDIO) || !pb03_on(VDDQ)) {
		goto fail;
	}
	spin_us(1000);
	{
		if (!pb02_on(PB02_VDDD_18_OIS)) {
			goto fail;
		}
		spin_us(100);
		if (!pb03_on(VDD_VM_OIS) || !pb02_on(PB02_VDDD_28_OIS)) {
			goto fail;
		}
		spin_us(1000);
	}
	hakios_hsi2c_power(&sensor_bus);        /* "on_i2c": the pins to function 2 (and the bus's clocks) ... */
	pin_pull(peric1 + GPC0, 0, 3);          /* ... with their pull-ups, as the device tree's hsi2c0-bus has them */
	pin_pull(peric1 + GPC0, 1, 3);
	hakios_hsi2c_reset(&sensor_bus);
	pin_pull(peric1 + GPC1, MCLK_PIN, 0);
	pin_function(peric1 + GPC1, MCLK_PIN, 2);
	delay_for_interval(1500, NSEC_PER_USEC);
	pin_out(peric0 + GPG2, RESET_PIN, true);
	delay_for_interval(10, NSEC_PER_MSEC);
	look_pins("2");

	error = sensor_get(0x0000, model, 2);
	if (error != 0) {
		say("exynos_camera: rear: 2: the sensor does not answer at %#x (%x): 20 ms more, and again\n", sensor_addr, error);
		delay_for_interval(20, NSEC_PER_MSEC);
		error = sensor_get(0x0000, model, 2);
	}
	if (error != 0) {
		say("exynos_camera: rear: 2: the sensor does not answer at %#x (%x)\n", sensor_addr, error);
		bus_scan();
		goto fail;
	}
	(void)sensor_get(0x0002, &revision, 1);
	(void)sensor_get(0x0005, &frames, 1);
	(void)sensor_get(0x0100, &mode, 1);
	id = (uint32_t)((model[0] << 8) | model[1]);
	say("exynos_camera: rear: 2: THE SENSOR ANSWERS at %#x: model ID %#06x (%s), revision %#x, frame count %u, mode_select "
	    "%#x (0: standby)\n", sensor_addr, id, id == MODEL_2L3 ? "a Samsung S5K2L3" : "NOT the S5K2L3's 0x20c3", revision,
	    frames, mode);
	return true;
fail:
	hakios_s9rear_down();
	return false;
}

/*
 * ---- the sensor's tables (setfile B, for revision 0xA5: fimc-is-cis-2l3-setB.h; the sensor's own register values, as
 * data; every one a 16-bit write) ----
 *
 * Android's order at the camera's entrance (sensor_2l3_cis_set_global_setting_internal, then _mode_change, without the
 * retention): the reset (sensor_2l3_setfile_B_Reset_TnP's first nine entries: 0x6010 = 1 is the software reset, 5 ms),
 * the Global table, the mode's table, each after 3 ms. Reset_TnP then goes on with 1590 words written in one burst to
 * the sensor's memory from 0x2000F2BC: a patch of the sensor's own firmware. That is code, not a table of register
 * values, and it is NOT carried here: the sensor runs its ROM as it is. (A tree that has a file hakios.s5k2l3_tnp.h
 * beside this one, defining tnp_words[], sends it; this repository has none.) After the mode: 0x021E = 0 (no WDR) and
 * 0x0702 = 0 (no shift of the integration time), which Android writes with every exposure.
 */
#if __has_include("hakios.s5k2l3_tnp.h")
#include "hakios.s5k2l3_tnp.h"
#define HAVE_TNP        1
#else
#define HAVE_TNP        0
#endif

static const uint16_t reset_table[][2] = {
	{ 0x6028, 0x2001 }, { 0x602A, 0x39F4 }, { 0x6F12, 0x0000 }, { 0x6760, 0x0000 }, { 0x6010, 0x0001 },
};
static const uint16_t reset_table2[][2] = {
	{ 0x6004, 0x0001 }, { 0x6028, 0x2000 }, { 0x602A, 0xF2BC },
};

/* sensor_2l3_setfile_B_Global: 199 16-bit writes */
static const uint16_t global_table[][2] = {
	{ 0xFCFC, 0x4000 }, { 0x6000, 0x0005 }, { 0x6214, 0x79F1 }, { 0x6218, 0x7150 }, { 0x0136, 0x1A00 },
	{ 0x0304, 0x0003 }, { 0x0306, 0x00AF }, { 0x0314, 0x0003 }, { 0x0316, 0x00BB }, { 0x030E, 0x0004 },
	{ 0x0310, 0x013C }, { 0xFCFC, 0x2000 }, { 0x8682, 0x0100 }, { 0xFCFC, 0x4000 }, { 0x0B04, 0x0101 },
	{ 0x307C, 0x0940 }, { 0x301A, 0x0001 }, { 0xFCFC, 0x2000 }, { 0x6720, 0x0001 }, { 0x8670, 0x1400 },
	{ 0x8672, 0x0100 }, { 0x9A28, 0x0014 }, { 0x9A2A, 0x0FE3 }, { 0xFCFC, 0x4000 }, { 0x6B5E, 0xFF00 },
	{ 0x6B60, 0x0FFF }, { 0xFCFC, 0x2000 }, { 0x5E94, 0x0002 }, { 0x5E96, 0x1840 }, { 0x5E46, 0x040D },
	{ 0x5E50, 0x0E11 }, { 0x240E, 0x0033 }, { 0x2420, 0x0027 }, { 0x2432, 0x0039 }, { 0x2444, 0x002D },
	{ 0x2456, 0x0039 }, { 0x27A4, 0x003C }, { 0x2A2C, 0x0025 }, { 0x13C2, 0x4020 }, { 0x13FA, 0x0067 },
	{ 0x13FE, 0x0067 }, { 0x1402, 0x009A }, { 0x1406, 0x009A }, { 0x14E0, 0x7430 }, { 0x14E2, 0x0000 },
	{ 0x14E4, 0x7CF8 }, { 0x14E6, 0x0000 }, { 0x14E8, 0x7426 }, { 0x14EA, 0x0000 }, { 0x14EC, 0x7CEE },
	{ 0x14EE, 0x0000 }, { 0x14F0, 0x7458 }, { 0x14F2, 0x0000 }, { 0x14F4, 0x7D20 }, { 0x14F6, 0x0000 },
	{ 0x14F8, 0x748A }, { 0x14FA, 0x0000 }, { 0x14FC, 0x7D52 }, { 0x14FE, 0x0000 }, { 0x1500, 0xFCFC },
	{ 0x1502, 0x4000 }, { 0x1504, 0xF488 }, { 0x1506, 0x0004 }, { 0x1508, 0xF48A }, { 0x150A, 0x0004 },
	{ 0x150C, 0xF480 }, { 0x150E, 0x000C }, { 0x1510, 0xF482 }, { 0x1512, 0x000C }, { 0x1514, 0xF46E },
	{ 0x1516, 0x001A }, { 0x1518, 0xF470 }, { 0x151A, 0x000A }, { 0x151C, 0xF600 }, { 0x151E, 0x0001 },
	{ 0x1520, 0xF602 }, { 0x1522, 0x0001 }, { 0x1524, 0xF60E }, { 0x1526, 0x0005 }, { 0x1528, 0xF628 },
	{ 0x152A, 0x0800 }, { 0x152C, 0xF626 }, { 0x152E, 0x0800 }, { 0x164A, 0x0000 }, { 0x164E, 0x0000 },
	{ 0x1652, 0x0200 }, { 0x1656, 0x0200 }, { 0x165A, 0x0333 }, { 0x165E, 0x0333 }, { 0x1738, 0x7430 },
	{ 0x173A, 0x0266 }, { 0x173C, 0x7CF8 }, { 0x173E, 0x0266 }, { 0x1740, 0x7426 }, { 0x1742, 0x0466 },
	{ 0x1744, 0x7CEE }, { 0x1746, 0x0466 }, { 0x1748, 0x7458 }, { 0x174A, 0x0199 }, { 0x174C, 0x7D20 },
	{ 0x174E, 0x0199 }, { 0x1750, 0x748A }, { 0x1752, 0x0333 }, { 0x1754, 0x7D52 }, { 0x1756, 0x0333 },
	{ 0x1758, 0xFCFC }, { 0x175A, 0x4000 }, { 0x175C, 0xF488 }, { 0x175E, 0x0002 }, { 0x1760, 0xF48A },
	{ 0x1762, 0x0002 }, { 0x1764, 0xF480 }, { 0x1766, 0x0004 }, { 0x1768, 0xF482 }, { 0x176A, 0x0004 },
	{ 0x176C, 0xF46E }, { 0x176E, 0x001A }, { 0x1770, 0xF470 }, { 0x1772, 0x000A }, { 0x1774, 0xF600 },
	{ 0x1778, 0xF602 }, { 0x177C, 0xF60E }, { 0x177E, 0x0008 }, { 0x1780, 0xF628 }, { 0x1782, 0x0800 },
	{ 0x1784, 0xF626 }, { 0x1786, 0x0800 }, { 0x18A2, 0x0000 }, { 0x18A6, 0x0000 }, { 0x18AA, 0x0133 },
	{ 0x18AE, 0x0133 }, { 0x18B2, 0x0333 }, { 0x18B6, 0x0333 }, { 0x1990, 0x7430 }, { 0x1992, 0x0266 },
	{ 0x1994, 0x7CF8 }, { 0x1996, 0x0266 }, { 0x1998, 0x7426 }, { 0x199A, 0x0399 }, { 0x199C, 0x7CEE },
	{ 0x199E, 0x0399 }, { 0x19A0, 0x7458 }, { 0x19A2, 0x0199 }, { 0x19A4, 0x7D20 }, { 0x19A6, 0x0199 },
	{ 0x19A8, 0x748A }, { 0x19AA, 0x0333 }, { 0x19AC, 0x7D52 }, { 0x19AE, 0x0333 }, { 0x19B0, 0xFCFC },
	{ 0x19B2, 0x4000 }, { 0x19B4, 0xF488 }, { 0x19B6, 0x0004 }, { 0x19B8, 0xF48A }, { 0x19BA, 0x0004 },
	{ 0x19BC, 0xF480 }, { 0x19BE, 0x000C }, { 0x19C0, 0xF482 }, { 0x19C2, 0x000C }, { 0x19C4, 0xF46E },
	{ 0x19C6, 0x001A }, { 0x19C8, 0xF470 }, { 0x19CA, 0x000A }, { 0x19CC, 0xF600 }, { 0x19D0, 0xF602 },
	{ 0x19D4, 0xF60E }, { 0x19D6, 0x0008 }, { 0x19D8, 0xF628 }, { 0x19DA, 0x0F80 }, { 0x19DC, 0xF626 },
	{ 0x19DE, 0x0F80 }, { 0xFCFC, 0x2001 }, { 0x2C00, 0x0102 }, { 0x2C02, 0x0001 }, { 0x2C04, 0x0078 },
	{ 0x2C06, 0x1100 }, { 0x2C08, 0x0F00 }, { 0x2C0A, 0x0100 }, { 0x2C0C, 0x0100 }, { 0x2C0E, 0x0060 },
	{ 0x2C10, 0x0060 }, { 0x2C12, 0x0A0A }, { 0x2C14, 0x0A0A }, { 0x2C16, 0xFFFF }, { 0xFCFC, 0x2000 },
	{ 0x1736, 0x0200 }, { 0x5E44, 0x050E }, { 0x1CC2, 0x0005 }, { 0x1E84, 0x0008 }, { 0x1EBA, 0x0003 },
	{ 0x1F5C, 0x0002 }, { 0x14DE, 0x0100 }, { 0xFCFC, 0x2001 }, { 0x2C1A, 0xB701 }, { 0xFCFC, 0x2000 },
	{ 0x8B64, 0x0900 }, { 0x8BBC, 0x0900 }, { 0xFCFC, 0x4000 }, { 0x6000, 0x0085 },
};
/* sensor_2l3_setfile_B_2016x1512_30fps_mode2: 330 16-bit writes */
static const uint16_t mode_2016x1512[][2] = {
	{ 0xFCFC, 0x4000 }, { 0x6000, 0x0005 }, { 0x6214, 0x79F1 }, { 0x6218, 0x7150 }, { 0xFCFC, 0x2000 },
	{ 0x1074, 0x1B64 }, { 0x1076, 0x3658 }, { 0xFCFC, 0x4000 }, { 0x303E, 0x0400 }, { 0xFCFC, 0x2000 },
	{ 0x10A2, 0x0004 }, { 0xFCFC, 0x4000 }, { 0x30F4, 0x0040 }, { 0x30F8, 0x0000 }, { 0xFCFC, 0x2000 },
	{ 0x6700, 0x0001 }, { 0x66D0, 0x0101 }, { 0x66D0, 0x0101 }, { 0x66E0, 0x8000 }, { 0x66D2, 0x0100 },
	{ 0x66E4, 0x0100 }, { 0x66F0, 0x0102 }, { 0xFCFC, 0x4000 }, { 0x0342, 0x0338 }, { 0x0340, 0x9F00 },
	{ 0xFCFC, 0x2000 }, { 0x447C, 0x019C }, { 0xFCFC, 0x4000 }, { 0x0344, 0x0018 }, { 0x0346, 0x0000 },
	{ 0x0348, 0x1FD7 }, { 0x034A, 0x0BDF }, { 0x034C, 0x07E0 }, { 0x034E, 0x05E8 }, { 0x0BC8, 0x0000 },
	{ 0x0BCA, 0x0000 }, { 0xFCFC, 0x2000 }, { 0x8A6E, 0x0004 }, { 0xFCFC, 0x4000 }, { 0x30DE, 0x0000 },
	{ 0x0BC2, 0x0004 }, { 0x0BC6, 0x0004 }, { 0x0408, 0x0001 }, { 0x040A, 0x0005 }, { 0xFCFC, 0x2000 },
	{ 0x8A70, 0x0008 }, { 0xFCFC, 0x4000 }, { 0x0900, 0x0242 }, { 0x0382, 0x0003 }, { 0x0386, 0x0003 },
	{ 0x307E, 0x0701 }, { 0x3080, 0x0002 }, { 0x30A2, 0x002B }, { 0xFCFC, 0x2000 }, { 0x0F64, 0x0000 },
	{ 0xFCFC, 0x4000 }, { 0x3038, 0x0001 }, { 0x3066, 0x0400 }, { 0xFCFC, 0x2000 }, { 0x10E2, 0x0001 },
	{ 0x5D9C, 0x0001 }, { 0x1070, 0x0000 }, { 0x1050, 0x1D97 }, { 0x1052, 0x1D97 }, { 0x1054, 0x1AC0 },
	{ 0x1056, 0x1AC0 }, { 0xFCFC, 0x4000 }, { 0x021E, 0x0000 }, { 0xFCFC, 0x2000 }, { 0x9966, 0x0043 },
	{ 0x9948, 0x41C0 }, { 0x994A, 0x0000 }, { 0x9952, 0x0201 }, { 0x9950, 0x0101 }, { 0x9950, 0x0101 },
	{ 0x9922, 0x0000 }, { 0x9080, 0x0000 }, { 0x9082, 0x0003 }, { 0x907C, 0x0000 }, { 0x907E, 0x0003 },
	{ 0x97F8, 0x0000 }, { 0x97FA, 0x0003 }, { 0x97F4, 0x0000 }, { 0x97F6, 0x0003 }, { 0x9088, 0x0000 },
	{ 0x908A, 0x0800 }, { 0x9090, 0x0000 }, { 0x9092, 0x0600 }, { 0x90C4, 0x0000 }, { 0x90C6, 0x0007 },
	{ 0x90C0, 0x0000 }, { 0x90C2, 0x0007 }, { 0x9808, 0x0000 }, { 0x980A, 0x0007 }, { 0x9804, 0x0000 },
	{ 0x9806, 0x0007 }, { 0x90CC, 0x0000 }, { 0x90CE, 0x00B0 }, { 0x90D4, 0x0000 }, { 0x90D6, 0x00B0 },
	{ 0x91F8, 0x0000 }, { 0x91FA, 0x00B0 }, { 0x91FC, 0x0000 }, { 0x91FE, 0x00B0 }, { 0x9814, 0x00B0 },
	{ 0x9816, 0x00B0 }, { 0x90E4, 0x0000 }, { 0x90E6, 0x0003 }, { 0x90DC, 0x0000 }, { 0x90DE, 0x0003 },
	{ 0x920C, 0x0000 }, { 0x920E, 0x0003 }, { 0x9210, 0x0000 }, { 0x9212, 0x0003 }, { 0x9204, 0x0000 },
	{ 0x9206, 0x0003 }, { 0x9208, 0x0000 }, { 0x920A, 0x0003 }, { 0x9820, 0x0000 }, { 0x9822, 0x0003 },
	{ 0x9818, 0x0000 }, { 0x981A, 0x0003 }, { 0x90E8, 0x0000 }, { 0x90EA, 0x051D }, { 0x9218, 0x0000 },
	{ 0x921A, 0x051D }, { 0x9544, 0x0000 }, { 0x9546, 0x051D }, { 0x97E4, 0x0000 }, { 0x97E6, 0x051D },
	{ 0x9824, 0x0000 }, { 0x9826, 0x051D }, { 0xFCFC, 0x4000 }, { 0x030E, 0x0004 }, { 0x0310, 0x00D0 },
	{ 0xFCFC, 0x2000 }, { 0x8EB0, 0x08C0 }, { 0x8EB2, 0x1843 }, { 0x8EA8, 0x0000 }, { 0x8EAA, 0x0000 },
	{ 0x8B14, 0x0000 }, { 0x8B16, 0x0000 }, { 0x8B18, 0x0000 }, { 0x8B1A, 0x0000 }, { 0xFCFC, 0x4000 },
	{ 0xF628, 0x0800 }, { 0xF626, 0x0800 }, { 0xFCFC, 0x2000 }, { 0x60E2, 0x07FF }, { 0x60E4, 0x03FF },
	{ 0x60E6, 0x03FF }, { 0x60EC, 0x03FF }, { 0x62AE, 0x049E }, { 0x5F26, 0x0100 }, { 0x5F26, 0x0100 },
	{ 0x5EC2, 0x0400 }, { 0x5EAC, 0x0059 }, { 0x5EE2, 0x0038 }, { 0x5EE4, 0x0038 }, { 0x5EE6, 0x0039 },
	{ 0x5EE8, 0x0039 }, { 0x5DFA, 0x0904 }, { 0x5E02, 0x0809 }, { 0x5E0C, 0x0904 }, { 0xFCFC, 0x4000 },
	{ 0xF47C, 0x0005 }, { 0xF47E, 0x0005 }, { 0xFCFC, 0x2000 }, { 0x5E30, 0x0201 }, { 0x5E38, 0x0702 },
	{ 0x5E54, 0x0401 }, { 0x5E5C, 0x0404 }, { 0x5E42, 0x1005 }, { 0x5E4A, 0x0710 }, { 0x5DD2, 0x005B },
	{ 0x5DDE, 0x005B }, { 0x1D90, 0x003E }, { 0x1E56, 0x0002 }, { 0x1E68, 0x0005 }, { 0x1EC2, 0x005A },
	{ 0x1ED4, 0x0055 }, { 0x2A14, 0x0021 }, { 0x2A26, 0x002D }, { 0x1E32, 0x0000 }, { 0x281C, 0x0101 },
	{ 0x282E, 0x00FF }, { 0x2840, 0x00F7 }, { 0x2852, 0x00F5 }, { 0x2864, 0x00ED }, { 0x2876, 0x00EB },
	{ 0x2888, 0x00E3 }, { 0x289A, 0x00E1 }, { 0x28AC, 0x00D9 }, { 0x28BE, 0x00D7 }, { 0x28D0, 0x00CF },
	{ 0x28E2, 0x00CD }, { 0x6730, 0x0101 }, { 0x6736, 0x0003 }, { 0x1C50, 0x0120 }, { 0x6748, 0x1FFF },
	{ 0x5D8E, 0x0006 }, { 0x222C, 0x0000 }, { 0xFCFC, 0x4000 }, { 0xF488, 0x0004 }, { 0xF48A, 0x0004 },
	{ 0xF480, 0x0004 }, { 0xF482, 0x0004 }, { 0xFCFC, 0x2000 }, { 0x70B4, 0xFFFA }, { 0x70B6, 0x0000 },
	{ 0x70B8, 0x0000 }, { 0x70BA, 0x0000 }, { 0x70BC, 0xFFFA }, { 0x70BE, 0x0000 }, { 0x70C0, 0x0000 },
	{ 0x70C2, 0x0000 }, { 0x7406, 0x0000 }, { 0x7CCE, 0x0000 }, { 0x7462, 0x0000 }, { 0x7D2A, 0x0000 },
	{ 0x7494, 0x0000 }, { 0x7D5C, 0x0000 }, { 0xFCFC, 0x4000 }, { 0xF46E, 0x001A }, { 0xF470, 0x0027 },
	{ 0xF600, 0x0001 }, { 0xF602, 0x0001 }, { 0xF60E, 0x0005 }, { 0xFCFC, 0x2000 }, { 0x0F80, 0x0000 },
	{ 0x0F8E, 0xF46E }, { 0x0F92, 0x001B }, { 0x0F94, 0xF470 }, { 0x6AF2, 0x0000 }, { 0x2234, 0x000F },
	{ 0x2246, 0x0030 }, { 0x1C4C, 0x0C00 }, { 0x1C2E, 0x0001 }, { 0x5EA8, 0xFFFF }, { 0x10A6, 0xFFFF },
	{ 0xFCFC, 0x4000 }, { 0xF52A, 0x0016 }, { 0xF49A, 0x0000 }, { 0xF492, 0x0000 }, { 0xFCFC, 0x2000 },
	{ 0x1D12, 0x0015 }, { 0x5F1A, 0xA962 }, { 0x5F1E, 0x4050 }, { 0x4482, 0x0206 }, { 0x2222, 0x000C },
	{ 0x3782, 0xFFFB }, { 0xFCFC, 0x4000 }, { 0x0A50, 0x0001 }, { 0x0A58, 0x0010 }, { 0x0A5A, 0x0010 },
	{ 0x0A5C, 0x0000 }, { 0x0A5E, 0x0000 }, { 0x0A56, 0x0201 }, { 0x0A56, 0x0201 }, { 0x30C6, 0x0000 },
	{ 0x0A60, 0x0050 }, { 0xFCFC, 0x2000 }, { 0x9860, 0x0000 }, { 0x9862, 0x0000 }, { 0x9864, 0x0000 },
	{ 0x9866, 0x0000 }, { 0x9868, 0x0000 }, { 0x986A, 0x0000 }, { 0x986C, 0x0000 }, { 0x986E, 0x0000 },
	{ 0x9870, 0x0000 }, { 0x9872, 0x0000 }, { 0x9874, 0x0000 }, { 0x9876, 0x0000 }, { 0x9878, 0x0000 },
	{ 0x987A, 0x0000 }, { 0x987C, 0x0000 }, { 0x987E, 0x0000 }, { 0x998E, 0x0008 }, { 0x9990, 0x03F0 },
	{ 0x99C2, 0x0008 }, { 0x99C4, 0x03F0 }, { 0x9A18, 0x0004 }, { 0x9A1A, 0x0004 }, { 0x6738, 0x0000 },
	{ 0x6794, 0x0028 }, { 0x6796, 0x03C0 }, { 0x6798, 0x00C8 }, { 0x678C, 0x0000 }, { 0x678E, 0x04E1 },
	{ 0x10E2, 0x0001 }, { 0xFCFC, 0x4000 }, { 0x30DA, 0x0100 }, { 0x0B30, 0x0100 }, { 0xFCFC, 0x2000 },
	{ 0x10EC, 0x0001 }, { 0xFCFC, 0x4000 }, { 0x3010, 0x0001 }, { 0xF464, 0x0006 }, { 0xF466, 0x0008 },
	{ 0xF49C, 0x0001 }, { 0xF496, 0x0034 }, { 0x30E4, 0x0000 }, { 0x30E6, 0x0000 }, { 0x30E8, 0x0000 },
	{ 0x30EA, 0x0000 }, { 0xFCFC, 0x2000 }, { 0x8D82, 0x0001 }, { 0x8D90, 0x0014 }, { 0x9FA4, 0x0000 },
	{ 0x9E94, 0x0000 }, { 0xFCFC, 0x2001 }, { 0x2C1C, 0x0000 }, { 0x2C1E, 0x0000 }, { 0x2C20, 0x0000 },
	{ 0x2C22, 0x0000 }, { 0x2C24, 0x0000 }, { 0xFCFC, 0x2000 }, { 0x991A, 0x0000 }, { 0x10E4, 0x0001 },
	{ 0x994C, 0x0000 }, { 0x1C2A, 0x0118 }, { 0x10B4, 0x0000 }, { 0x10B6, 0x0000 }, { 0xFCFC, 0x4000 },
	{ 0x6000, 0x0085 }, { 0x6028, 0x2000 }, { 0x602A, 0xA950 }, { 0x6F12, 0x0001 }, { 0x6F12, 0x6300 },
};

static bool
sensor_put16(uint16_t reg, uint16_t value)
{
	uint8_t buf[4] = { (uint8_t)(reg >> 8), (uint8_t)reg, (uint8_t)(value >> 8), (uint8_t)value };
	uint32_t error = hakios_hsi2c_message(&sensor_bus, sensor_addr, false, buf, 4, true);

	if (error != 0) {
		say("exynos_camera: rear: the sensor did not take %#06x = %#x (%x)\n", reg, value, error);
		return false;
	}
	return true;
}

static bool
table_put(const uint16_t (*table)[2], uint32_t entries)
{
	for (uint32_t i = 0; i < entries; i++) {
		if (!sensor_put16(table[i][0], table[i][1])) {
			return false;
		}
	}
	return true;
}

/* the reset, the Global table and the mode's (2016 x 1512 at 30 frames a second, mode 2); false: a write failed */
bool
hakios_s9rear_mode(void)
{
	uint64_t start = mach_absolute_time(), ns;

	if (!cam_rear || !powered) {
		return false;
	}
	say("exynos_camera: rear: 5: the sensor's tables (setfile B): the reset (0x6010 = 1, 5 ms), %s, Global (%u 16-bit writes), "
	    "2016x1512_30fps_mode2 (%u), then 0x021E = 0 and 0x0702 = 0\n", HAVE_TNP ? "the firmware patch (this tree has "
	    "hakios.s5k2l3_tnp.h)" : "NO firmware patch (not carried: the sensor's ROM as it is)",
	    (unsigned)(sizeof(global_table) / sizeof(global_table[0])), (unsigned)(sizeof(mode_2016x1512) / sizeof(mode_2016x1512[0])));
	delay_for_interval(3, NSEC_PER_MSEC);
	if (!table_put(reset_table, sizeof(reset_table) / sizeof(reset_table[0]))) {
		return false;
	}
	delay_for_interval(5, NSEC_PER_MSEC);
	if (!table_put(reset_table2, sizeof(reset_table2) / sizeof(reset_table2[0]))) {
		return false;
	}
#if HAVE_TNP
	/* the words one at a time: the sensor's write pointer (0x602A) goes on by itself, as in its own tables */
	for (uint32_t i = 0; i < sizeof(tnp_words) / sizeof(tnp_words[0]); i++) {
		if (!sensor_put16(0x6F12, tnp_words[i])) {
			return false;
		}
	}
#endif
	delay_for_interval(3, NSEC_PER_MSEC);
	if (!table_put(global_table, sizeof(global_table) / sizeof(global_table[0]))) {
		return false;
	}
	delay_for_interval(3, NSEC_PER_MSEC);
	if (!table_put(mode_2016x1512, sizeof(mode_2016x1512) / sizeof(mode_2016x1512[0]))) {
		return false;
	}
	if (!sensor_put16(0x021E, 0x0000) || !sensor_put16(0x0702, 0x0000)) {
		return false;
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - start, &ns);
	say("exynos_camera: rear: 5: the tables are in (%llu ms)\n", ns / NSEC_PER_MSEC);
	return true;
}

/*
 * ---- the lens's bus: the lens's motor, the OIS controller and the aperture's driver ----
 *
 * hsi2c@10870000, "I2C_CAM1" of PERIC1 (hsi2c1), 400 kHz; pins gpc0-2 (SCL) and gpc0-3 (SDA), function 2 with their
 * pull-ups while the camera is up ("hsi2c1-bus"), inputs without a pull when it is down; in CMU_PERIC1:
 * GATE_CLK_PERIC1_I2C_CAM1 0x2024, its reset synchroniser's clock 0x2008, IPCLK 0x2068, PCLK 0x206C,
 * QCH_CON_I2C_CAM1_QCH 0x300C, DIV_CLK_PERIC1_I2C_CAM1 0x1804. Three chips: the lens's motor at 0x0C (an AK737x:
 * hakios.ak737x.c, which is given this bus, cam_af=N), the OIS controller at 0x24 and the aperture's driver at 0x4C.
 * All of it is used from the camera's thread only (one driver per bus).
 *
 * THE OIS (cam_rear_ois=N). A Rumba S4 controller with its own flash (firmware, gyro and hall calibration) and gyro.
 * 16-bit register addresses, one byte each: 0x0000 the servo (1 on, 0 off), 0x0001 the status (0x01 idle, 0x13 too
 * after a mode change; anything else: busy or a fault), 0x0002 the mode (0 still, 1 video, 5 centring: the lens held in
 * the middle), 0x0004-0x0005 the error word, 0x0006 a second status, 0x0030 = 3 the gyro's sleep (written after the
 * servo is switched off), 0x0238 and 0x023A the gyro gain's fade up and down in ms (16 bits, low byte first: 1000 each
 * before the first mode), 0x00F8-0x00FB and 0x007C-0x007F the firmware's version text, 0x03F0 the clock it expects on
 * its pin (32 bits, low byte first: 26 MHz, 0x018CBA80, is this board's), 0x021A and 0x021C the hall centre, 0x0248 and
 * 0x024A the gyro's two axes as the offset test reads them (16 bits, low byte first). Its supplies are switched with
 * the sensor's from cam_rear_ois=1 on, in Android's order; its reset is the sensor's.
 *   1  a look: the status waited for (300 ms at the most), the version, error, mode, clock and centre read. Nothing is
 *      written to it.
 *   2  that, and the servo: the two fades, the mode (cam_rear_ois_mode: 0 still, 1 video, 5 centring), 0x0000 = 1, as
 *      fimc_is_set_ois_mode_s4 does; only if the look found it idle, without an error and on a 26 MHz clock. Before the
 *      supplies go: 0x0000 = 0, idle waited for, 0x0030 = 3 (fimc_is_ois_gyro_sleep_s4).
 * NEVER WRITTEN, whoever asks: 0x0003 (it stores the registers in the flash), 0x000D and 0x000E (reset and flash
 * commands), 0x000F-0x0011, 0x0012-0x0014 (the self tests and the gyro calibration, which store their results), 0x03F0-
 * 0x03F5 (the clock, which Android rewrites and stores when it differs: here a controller on another clock is reported
 * and left alone), and anything of the firmware update. ois_put() takes 0x0000, 0x0002, 0x0030 and the four fade bytes,
 * with the values above, and nothing else. kern.hakios.ois.mode (0, 1, 5; 255: servo off) changes the mode in a
 * stream; kern.hakios.ois.dump=1 prints the state, the gyro and the centre once, =N (2 and up) every Nth frame, =-1 once
 * with the registers 0x0000-0x025F as hex (read only): two of those, compared, show which bytes are alive.
 *
 * THE APERTURE (cam_rear_ap=N). An AK7372 used as the driver of a two-stop iris, F1.5 and F2.4, which stays where it was
 * put without power. 8-bit registers: 0x00 and 0x01 a 10-bit position ([9:2], then [1:0] in bits 7-6), 0x02 the mode
 * (0 active), 0xA6 the drive (0x7B: open loop, 0: back), 0xAE the chip's write protection. Android's driver
 * (fimc-is-aperture-ak7372.c), as it runs at every opening of the camera:
 *   init   0xAE = 0x3B ("setting mode"), the position 511, the mode 0
 *   move   0xA6 = 0x7B, the position 0 (F1.5) or 1023 (F2.4), 10 ms; 0xA6 = 0, the position 511, 15 ms
 *   close  if the iris is not at F1.5: (the OIS to centring, 10 ms,) 0xA6 = 0x7B, the position 0, 10 ms; then the supplies
 *          go: F1.5 is where the iris rests
 * 0xAE = 0x3B releases the chip's write protection for the run: without it the chip acknowledges 0xA6 and keeps
 * nothing of it, and the iris does not move (the phone, 12 November: the same brightness before and after). It stores
 * nothing by itself. What would store is a command written to register 0x03, which makes the chip copy its settings
 * (0x10-0x25) into its own permanent memory: Android's aperture driver never writes it, and neither does this file,
 * ever, nor 0x10-0x25. ap_put() is the only way to the chip and takes 0xAE = 0x3B, 0xA6 = 0x7B or 0, 0x00, 0x01 and
 * 0x02 = 0; anything else is refused and said. (The lens's motor at 0x0C is another chip of the same family with its
 * own driver, hakios.ak737x.c: its 0xAE is never written.)
 *   1  a look: registers 0x00-0x03 and 0xA6 read. Nothing written: the iris stays where it rests.
 *   15 or 24: the look, and the iris put at F1.5 or F2.4 at each power-up of the camera.
 * kern.hakios.aperture.go=15 or 24 (with cam_rear_ap not 0) moves it in a stream. An iris this driver has moved is put
 * back at F1.5, Android's close, before the supplies go, and the log says so.
 */
static struct hakios_hsi2c lens_bus = {
	.name = "cam-rear-lens",
	.regs_pa = 0x10870000UL,                        /* hsi2c@10870000: I2C_CAM1 of PERIC1 (hsi2c1) */
	.usi_mode_pa = 0,
	.cmu_pa = EXYNOS_CMU_PERIC1_PA, .cmu_size = 0x4000,
	.gates = { 0x2024, 0x2008, 0x2068, 0x206C }, .ngates = 4,
	.qch = 0x300C, .div = 0x1804, .mux = 0x0120,
	.pins_pa = EXYNOS_GPIO_PERIC1_PA + 0x60,        /* gpc0: SCL pin 2, SDA pin 3, function 2 */
	.pins_mask = 0xff00u, .pins_want = 0x2200u,
	.ipclk_mhz = 400, .bus_hz = 400000,
};
static bool lens_mapped, lens_up;

#define OIS                     0x24
#define OIS_CTRL                0x0000
#define OIS_STATUS              0x0001
#define OIS_MODE                0x0002
#define OIS_GYRO_SLEEP          0x0030
#define OIS_FADE_UP             0x0238
#define OIS_FADE_DOWN           0x023A
#define OIS_CLOCK               0x03F0
#define OIS_CLOCK_26MHZ         0x018CBA80u
#define OIS_IDLE(s)             ((s) == 0x01 || (s) == 0x13)
#define AP                      0x4C

static bool ois_answers, ois_ok, ois_on, ois_said, ap_answers, ap_said, ap_ready;
static int ois_status = -1, ois_mode_now = -1, ois_mode_ask = -1, ois_dump, ois_error = -1;
static int ap_at, ap_go;                /* 15 or 24 once put there by this driver (0: where it rests) */
SYSCTL_DECL(_kern_hakios);
SYSCTL_NODE(_kern_hakios, OID_AUTO, ois, CTLFLAG_RW | CTLFLAG_LOCKED, 0, "the Galaxy S9's rear camera's stabiliser (hakios.exynos_camera_rear.c, cam_rear_ois=N)");
SYSCTL_INT(_kern_hakios_ois, OID_AUTO, status, CTLFLAG_RD | CTLFLAG_KERN | CTLFLAG_LOCKED, &ois_status, 0, "the controller's status register (0x0001) as last read: 1 idle, 2 the servo running; -1: not read");
SYSCTL_INT(_kern_hakios_ois, OID_AUTO, error, CTLFLAG_RD | CTLFLAG_KERN | CTLFLAG_LOCKED, &ois_error, 0, "its error word (0x0004-0x0005) as last read; -1: not read");
SYSCTL_INT(_kern_hakios_ois, OID_AUTO, mode_now, CTLFLAG_RD | CTLFLAG_KERN | CTLFLAG_LOCKED, &ois_mode_now, 0, "the mode the servo was last switched on in (0 still, 1 video, 5 centring); -1: off");
SYSCTL_INT(_kern_hakios_ois, OID_AUTO, mode, CTLFLAG_RW | CTLFLAG_KERN | CTLFLAG_LOCKED, &ois_mode_ask, 0, "set in a run with cam_rear_ois=2: 0 still, 1 video, 5 centring, 255 the servo off; taken at the stream's next frame");
SYSCTL_INT(_kern_hakios_ois, OID_AUTO, dump, CTLFLAG_RW | CTLFLAG_KERN | CTLFLAG_LOCKED, &ois_dump, 0, "set: 1 the state, gyro and centre once; N (2 and up) every Nth frame of the stream; -1 once with registers 0x0000-0x025F as hex");
SYSCTL_NODE(_kern_hakios, OID_AUTO, aperture, CTLFLAG_RW | CTLFLAG_LOCKED, 0, "the Galaxy S9's rear camera's iris (hakios.exynos_camera_rear.c, cam_rear_ap=N)");
SYSCTL_INT(_kern_hakios_aperture, OID_AUTO, at, CTLFLAG_RD | CTLFLAG_KERN | CTLFLAG_LOCKED, &ap_at, 0, "15 (F1.5) or 24 (F2.4) once this driver put it there; 0: where it rested");
SYSCTL_INT(_kern_hakios_aperture, OID_AUTO, go, CTLFLAG_RW | CTLFLAG_KERN | CTLFLAG_LOCKED, &ap_go, 0, "set to 15 or 24 in a run with cam_rear_ap: the iris to F1.5 or F2.4 at the stream's next frame");

struct hakios_hsi2c *
hakios_s9rear_lens_bus(void)
{
	return &lens_bus;
}

static uint32_t
ois_get(uint16_t reg, uint8_t *buf, uint32_t len)
{
	uint8_t address[2] = { (uint8_t)(reg >> 8), (uint8_t)reg };
	uint32_t error = hakios_hsi2c_message(&lens_bus, OIS, false, address, 2, false);

	return error ? error : hakios_hsi2c_message(&lens_bus, OIS, true, buf, len, true);
}

static uint32_t
ois_get16(uint16_t reg)
{
	uint8_t b[2] = { 0, 0 };

	return ois_get(reg, b, 2) ? 0xffffffffu : (uint32_t)(b[0] | (b[1] << 8));
}

/* The only way anything reaches the OIS controller: the servo, the mode, the gyro's sleep, the fades. */
static bool
ois_put(uint16_t reg, const uint8_t *value, uint32_t len)
{
	uint8_t buf[4] = { (uint8_t)(reg >> 8), (uint8_t)reg, value[0], len > 1 ? value[1] : 0 };
	bool allowed = false;

	switch (reg) {
	case OIS_CTRL:
		allowed = len == 1 && value[0] <= 1;
		break;
	case OIS_MODE:
		allowed = len == 1 && (value[0] == 0x00 || value[0] == 0x01 || value[0] == 0x05);
		break;
	case OIS_GYRO_SLEEP:
		allowed = len == 1 && value[0] == 0x03;
		break;
	case OIS_FADE_UP:
	case OIS_FADE_DOWN:
		allowed = len == 2;
		break;
	default:
		break;
	}
	if (!allowed || !ois_answers || cam_rear_ois < 2) {
		say("exynos_camera: rear: OIS %#06x = %#x refused: not a register and value this driver writes\n", reg, value[0]);
		return false;
	}
	if (hakios_hsi2c_message(&lens_bus, OIS, false, buf, 2 + len, true) != 0) {
		say("exynos_camera: rear: the OIS did not take %#06x = %#x (%#x)\n", reg, value[0], lens_bus.last_error);
		return false;
	}
	return true;
}

static bool
ois_put8(uint16_t reg, uint8_t value)
{
	return ois_put(reg, &value, 1);
}

static uint8_t
ois_wait_idle(uint32_t ms)
{
	uint8_t status = 0xff;

	for (uint32_t waited = 0; waited <= ms; waited += 5) {
		if (ois_get(OIS_STATUS, &status, 1) == 0 && OIS_IDLE(status)) {
			break;
		}
		spin_us(5000);
	}
	ois_status = status;
	return status;
}

static void
ois_state(const char *when)
{
	uint8_t status = 0xff, mode = 0xff, ctrl = 0xff, e[2] = { 0xff, 0xff }, s6 = 0xff;
	uint32_t gx, gy;

	(void)ois_get(OIS_CTRL, &ctrl, 1);
	(void)ois_get(OIS_STATUS, &status, 1);
	(void)ois_get(OIS_MODE, &mode, 1);
	(void)ois_get(0x0004, e, 2);
	(void)ois_get(0x0006, &s6, 1);
	gx = ois_get16(0x0248);
	gy = ois_get16(0x024A);
	ois_status = status;
	ois_error = e[0] | (e[1] << 8);
	say("exynos_camera: rear: OIS %s: servo (0x0000) %#x, status (0x0001) %#x, mode (0x0002) %#x, error (0x0004) %#x, 0x0006 "
	    "%#x; gyro (0x0248, 0x024A) %d %d; centre (0x021A, 0x021C) %d %d\n", when, ctrl, status, mode, ois_error, s6,
	    (int)(int16_t)gx, (int)(int16_t)gy, (int)(int16_t)ois_get16(0x021A), (int)(int16_t)ois_get16(0x021C));
}

/* registers 0x0000-0x025F and 0x03F0-0x03FF as hex, sixteen a line (reads only) */
static void
ois_hex(void)
{
	for (uint32_t reg = 0; reg < 0x0400; reg += 16) {
		uint8_t b[16];
		char hex[16 * 3 + 1];

		if (reg >= 0x0260 && reg < 0x03F0) {
			continue;
		}
		if (ois_get((uint16_t)reg, b, 8) != 0 || ois_get((uint16_t)(reg + 8), b + 8, 8) != 0) {
			say("exynos_camera: rear: OIS %#06x: not read (%#x)\n", reg, lens_bus.last_error);
			continue;
		}
		for (uint32_t k = 0; k < 16; k++) {
			snprintf(hex + 3 * k, 4, "%02x ", b[k]);
		}
		say("exynos_camera: rear: OIS %#06x: %s\n", reg, hex);
	}
}

/* the servo on in a mode, as fimc_is_set_ois_mode_s4 does; only a controller the look found well */
static void
ois_servo(uint32_t mode)
{
	static const uint8_t fade[2] = { 1000 & 0xff, 1000 >> 8 };
	static bool faded;

	if (!ois_ok || cam_rear_ois < 2) {
		return;
	}
	if (mode == 255) {
		if (ois_on) {
			(void)ois_put8(OIS_CTRL, 0);
			(void)ois_wait_idle(100);
			ois_on = false;
			ois_mode_now = -1;
			ois_state("after the servo was switched off");
		}
		return;
	}
	if (mode != 0 && mode != 1 && mode != 5) {
		return;
	}
	if (!faded) {
		(void)ois_wait_idle(100);
		faded = ois_put(OIS_FADE_UP, fade, 2) && ois_put(OIS_FADE_DOWN, fade, 2);
	}
	if (!ois_put8(OIS_MODE, (uint8_t)mode) || !ois_put8(OIS_CTRL, 1)) {
		return;
	}
	ois_on = true;
	ois_mode_now = (int)mode;
	spin_us(20000);
	say("exynos_camera: rear: OIS: the servo switched on in mode %u (%s): 0x0002 = %#x, 0x0000 = 1\n", mode,
	    mode == 0 ? "still" : mode == 1 ? "video" : "centring", mode);
	ois_state("20 ms after");
}

static void
ois_up(void)
{
	uint8_t v[8] = { 0 }, status;
	uint32_t clock;

	ois_on = false;
	ois_mode_now = -1;
	status = ois_wait_idle(300);
	if (status == 0xff && lens_bus.last_error != 0) {
		if (!ois_said) {
			say("exynos_camera: rear: OIS: nothing answers at %#x (%#x)\n", OIS, lens_bus.last_error);
		}
		ois_said = true;
		ois_answers = ois_ok = false;
		return;
	}
	ois_answers = true;
	(void)ois_get(0x00F8, v, 4);            /* 0x00F8, F9, (FA), FB */
	(void)ois_get(0x007C, v + 4, 4);
	clock = ois_get16(OIS_CLOCK) | (ois_get16(OIS_CLOCK + 2) << 16);
	ois_ok = OIS_IDLE(status) && clock == OIS_CLOCK_26MHZ;
	if (!ois_said || !ois_ok) {
		say("exynos_camera: rear: OIS: THE CONTROLLER ANSWERS at %#x: status %#x (%s), firmware version \"%c%c%c%c%c%c%c\" "
		    "(0x00FB, F9, F8, 0x007C-7F), 0x00FC-FD %#x, the clock it expects %u Hz (%s)\n", OIS, status,
		    OIS_IDLE(status) ? "idle" : "NOT idle: left alone", v[3], v[1], v[0], v[4], v[5], v[6], v[7], ois_get16(0x00FC),
		    clock, clock == OIS_CLOCK_26MHZ ? "26 MHz, this board's" : "NOT this board's 26 MHz: left alone, nothing written");
		ois_state("as found");
	}
	ois_said = true;
	if (cam_rear_ois >= 2 && ois_ok) {
		ois_servo(cam_rear_ois_mode);
	}
}

static uint32_t
ap_get(uint8_t reg, uint8_t *value)
{
	return hakios_hsi2c_read_reg(&lens_bus, AP, reg, value, 1);
}

/*
 * The only way anything reaches the aperture's chip: 0xAE = 0x3B (the write protection off for the run), the drive, the
 * position, the mode 0. Never 0x03 (the store command), never 0x10-0x25 (the stored settings), nothing else.
 */
static bool
ap_put(uint8_t reg, uint8_t value)
{
	uint8_t buf[2] = { reg, value };
	bool allowed = reg == 0x00 || (reg == 0x01 && (value & 0x3f) == 0) || (reg == 0x02 && value == 0x00) ||
	    (reg == 0xA6 && (value == 0x7B || value == 0x00)) || (reg == 0xAE && value == 0x3B);

	if (!allowed || !ap_answers || cam_rear_ap == 0) {
		say("exynos_camera: rear: aperture %#x = %#x refused: not a register and value this driver writes\n", reg, value);
		return false;
	}
	if (hakios_hsi2c_message(&lens_bus, AP, false, buf, 2, true) != 0) {
		say("exynos_camera: rear: the aperture's chip did not take %#x = %#x (%#x)\n", reg, value, lens_bus.last_error);
		return false;
	}
	return true;
}

static bool
ap_position(uint32_t pos)
{
	return ap_put(0x00, (uint8_t)(pos >> 2)) && ap_put(0x01, (uint8_t)((pos << 6) & 0xC0));
}

static void
ap_look(const char *when)
{
	uint8_t r[5] = { 0xff, 0xff, 0xff, 0xff, 0xff };

	if (ap_get(0x00, &r[0]) != 0) {
		say("exynos_camera: rear: aperture: nothing answers at %#x (%#x)\n", AP, lens_bus.last_error);
		ap_answers = false;
		return;
	}
	ap_answers = true;
	(void)ap_get(0x01, &r[1]);
	(void)ap_get(0x02, &r[2]);
	(void)ap_get(0x03, &r[3]);
	(void)ap_get(0xA6, &r[4]);
	say("exynos_camera: rear: aperture %s: THE CHIP ANSWERS at %#x: position registers %#x %#x (position %u of 1023), mode "
	    "%#x, product (0x03) %#x, drive (0xA6) %#x\n", when, AP, r[0], r[1], ((uint32_t)r[0] << 2) | (r[1] >> 6), r[2], r[3],
	    r[4]);
}

/* the iris to a stop: Android's move (back: its close, the first half only, before the supplies go) */
static void
ap_move(int stop, bool back)
{
	uint8_t drive = 0xff;

	if (!ap_answers || cam_rear_ap == 0 || (stop != 15 && stop != 24)) {
		return;
	}
	if (!ap_ready) {
		/* (sensor_ak7372_aperture_init: the write protection off, the position 511, the mode active) */
		if (!ap_put(0xAE, 0x3B) || !ap_position(511) || !ap_put(0x02, 0x00)) {
			return;
		}
		ap_ready = true;
	}
	if (!ap_put(0xA6, 0x7B)) {
		return;
	}
	(void)ap_get(0xA6, &drive);     /* (what the chip kept of it: without the unlock, nothing) */
	if (!ap_position(stop == 15 ? 0 : 1023)) {
		return;
	}
	spin_us(10000);
	if (!back) {
		if (!ap_put(0xA6, 0x00) || !ap_position(511)) {
			return;
		}
		spin_us(15000);
	}
	ap_at = stop;
	say("exynos_camera: rear: aperture: driven to F%s (0xA6 = 0x7B, which read back %#x; position %u, 10 ms%s)\n",
	    stop == 15 ? "1.5" : "2.4", drive, stop == 15 ? 0u : 1023u, back ? "; Android's close: THE IRIS IS LEFT AT F1.5" :
	    ", 0xA6 = 0, position 511, 15 ms");
	if (!back) {
		ap_look("after the move");
	}
}

/* after the sensor has answered: the bus, then the OIS and the aperture as the boot-args say */
void
hakios_s9rear_lens_up(void)
{
	if (!cam_rear || !powered) {
		return;
	}
	if (!lens_mapped) {
		say("exynos_camera: rear: the lens's bus: hsi2c@10870000 (I2C_CAM1 of PERIC1), pins gpc0-2 and gpc0-3\n");
		if (!hakios_hsi2c_map(&lens_bus, say)) {
			return;
		}
		lens_mapped = true;
	}
	hakios_hsi2c_power(&lens_bus);
	pin_pull(peric1 + GPC0, 2, 3);          /* (the device tree's hsi2c1-bus: the pins' pull-ups) */
	pin_pull(peric1 + GPC0, 3, 3);
	hakios_hsi2c_reset(&lens_bus);
	lens_up = true;
	ap_ready = false;
	if (cam_rear_ois >= 1) {
		ois_up();
	}
	if (cam_rear_ap != 0) {
		if (!ap_said) {
			ap_look("as found");
			ap_said = true;
		}
		if (cam_rear_ap == 15 || cam_rear_ap == 24) {
			ap_move((int)cam_rear_ap, false);
		}
	}
}

/* before the supplies go (also on the run's way out: nothing here sleeps) */
void
hakios_s9rear_lens_down(void)
{
	if (!lens_up) {
		return;
	}
	if (ap_ready && ap_answers) {
		/* sensor_ak7372_aperture_deinit: the OIS to centring (if its servo is on), 10 ms, then the iris to F1.5 */
		if (ois_on) {
			(void)ois_put8(OIS_MODE, 0x05);
			spin_us(10000);
		}
		ap_move(15, true);
	}
	if (ois_on) {
		(void)ois_put8(OIS_CTRL, 0);
		(void)ois_wait_idle(100);
		(void)ois_put8(OIS_GYRO_SLEEP, 0x03);
		spin_us(1000);
		ois_on = false;
		ois_mode_now = -1;
		say("exynos_camera: rear: OIS: the servo switched off (status %#x), the gyro put to sleep\n", ois_status);
	}
	pin_function(peric1 + GPC0, 2, 0);
	pin_function(peric1 + GPC0, 3, 0);
	pin_pull(peric1 + GPC0, 2, 0);
	pin_pull(peric1 + GPC0, 3, 0);
	lens_up = false;
}

/* each frame of a stream: what the sysctls asked for */
void
hakios_s9rear_poll(uint64_t frame)
{
	int go, mode, dump = ois_dump;

	if (!lens_up) {
		return;
	}
	go = __atomic_exchange_n(&ap_go, 0, __ATOMIC_ACQ_REL);
	if (go != 0 && cam_rear_ap != 0) {
		ap_move(go, false);
	}
	mode = __atomic_exchange_n(&ois_mode_ask, -1, __ATOMIC_ACQ_REL);
	if (mode >= 0) {
		ois_servo((uint32_t)mode);
	}
	if (dump != 0 && ois_answers) {
		if (dump == 1 || dump == -1) {
			ois_dump = 0;
		}
		if (dump == 1 || dump == -1 || (dump >= 2 && frame % (uint64_t)dump == 0)) {
			uint8_t b[16] = { 0 };

			ois_state("now");
			(void)ois_get(0x0070, b, 8);
			(void)ois_get(0x0080, b + 8, 8);
			say("exynos_camera: rear: OIS now: 0x0070-77: %02x %02x %02x %02x %02x %02x %02x %02x (0x0070, 16 bits, changed "
			    "between two looks with the phone still); 0x0080-87: %02x %02x %02x %02x %02x %02x %02x %02x\n", b[0], b[1],
			    b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
		}
		if (dump == -1) {
			ois_hex();
		}
	}
}

#endif /* HAKIOS_DEVICE_IS_STAR2LTE */
