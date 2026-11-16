/*
 * SPDX-License-Identifier: MIT-0
 *
 * The device's orientation for iOS 27 on the Galaxy S9, from its accelerometer. A library inserted into backboardd
 * (the kernel's boot-arg sweetinject=...;/usr/lib/o.dylib,backboardd).
 *
 * Why. The iOS 27 root runs as an iPhone 11, and on that model CoreMotion asks Apple's motion coprocessor for the
 * orientation (CLOrientationNotifier::openHidDriverInterface, chosen by CLPlatformInfo::queryMotionCaps() for the
 * model); its own detector that works from the accelerometer (CLOrientationDetector) is not started. The S9 has no such
 * coprocessor, so backboardd, which asks CoreMotion's CMDeviceOrientationManager and sends what it gets on to
 * SpringBoard and the applications, never hears of a turn. As userland/src/inject.c does on iOS 16.5, this library
 * replaces that manager's start, stop, blocking read and "available" in backboardd, and works the orientation out
 * itself.
 *
 * From what. The kernel's sensor-hub driver (drivers/input/ssp/hakios.ssp.c, boot-arg shub=6) hands the hub's samples
 * out on /dev/hakios-sensors (userland/src/hakios.sensors.h). Without that device this library does nothing at all:
 * no method is replaced. The accelerometer and the gyroscope themselves reach iOS as HID events from the kernel
 * (the fork's SweetHIDSensors.cpp, boot-arg shub_hid); this is only the orientation.
 *
 * How. Each accelerometer sample in g and iOS's sign (at rest -1 g on the axis that points up), low-passed; the axis
 * that holds more than 0.75 g names the orientation, and a new one counts after it has held for 300 ms:
 *   y down  1 portrait             y up  2 portrait upside down
 *   x down  3 landscape left       x up  4 landscape right      (UIDeviceOrientation's numbers)
 *   z down  5 face up              z up  6 face down
 * What it finds and what backboardd asks is written as lines of text to the device, which the kernel logs.
 *
 * Build: tools/ios27/sensors/build.sh (arm64, signed ad hoc). Install and remove: docs/ios27/SENSORS.md.
 */
#include <Block.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "../../../userland/src/hakios.sensors.h"

#define HOLD_US         300000
#define GRAVITY_MIN     0.75
#define COREMOTION      "/System/Library/Frameworks/CoreMotion.framework/CoreMotion"

static int device = -1;

static void
say(const char *format, ...)
{
	char line[200];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 1) {
		n = sizeof(line) - 1;
	}
	if (n > 0 && device >= 0) {
		(void)write(device, line, (size_t)n);
	}
}

static volatile int orientation;                /* UIDeviceOrientation: 0 unknown, 1-6 */
static id orientation_queue;                    /* the manager's handler and the queue it wants it run on */
static void (^orientation_handler)(id, id);
static pthread_mutex_t orientation_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned asked, told;

static const char *
name_of(int value)
{
	static const char *const names[] = { "unknown", "portrait", "portrait upside down", "landscape left",
		                             "landscape right", "face up", "face down" };

	return names[value >= 0 && value <= 6 ? value : 0];
}

static id
make_orientation(int value)
{
	Class cls = objc_getClass("CMDeviceOrientation");
	id object = cls ? ((id (*)(id, SEL))objc_msgSend)((id)cls, sel_registerName("alloc")) : nil;

	if (object == nil) {
		return nil;
	}
	return ((id (*)(id, SEL, int, double))objc_msgSend)(object, sel_registerName("initWithOrientation:andTimestamp:"), value,
	    CFAbsoluteTimeGetCurrent());
}

/* the new orientation to backboardd, on the queue it gave */
static void
orientation_changed(int value)
{
	id queue;
	void (^handler)(id, id);

	pthread_mutex_lock(&orientation_lock);
	orientation = value;
	queue = orientation_queue;
	handler = orientation_handler ? Block_copy(orientation_handler) : NULL;
	pthread_mutex_unlock(&orientation_lock);
	say("orientation %d (%s)%s", value, name_of(value), handler != NULL && queue != nil ? ": told to backboardd" :
	    ": nobody has asked yet");
	if (handler == NULL || queue == nil) {
		if (handler != NULL) {
			Block_release(handler);
		}
		return;
	}
	told++;
	((void (*)(id, SEL, void (^)(void)))objc_msgSend)(queue, sel_registerName("addOperationWithBlock:"), ^{
		id o = make_orientation(value);

		handler(o, nil);
		if (o != nil) {
			((void (*)(id, SEL))objc_msgSend)(o, sel_registerName("release"));
		}
		Block_release(handler);
	});
}

static void
my_start_orientation(__unused id self, __unused SEL selector, id queue, void (^handler)(id, id))
{
	int now;

	pthread_mutex_lock(&orientation_lock);
	if (orientation_handler != NULL) {
		Block_release(orientation_handler);
	}
	if (orientation_queue != nil) {
		((void (*)(id, SEL))objc_msgSend)(orientation_queue, sel_registerName("release"));
	}
	orientation_handler = handler ? Block_copy(handler) : NULL;
	orientation_queue = queue ? ((id (*)(id, SEL))objc_msgSend)(queue, sel_registerName("retain")) : nil;
	now = orientation;
	asked++;
	pthread_mutex_unlock(&orientation_lock);
	say("backboardd asks for the orientation (time %u; it is %d, %s)", asked, now, name_of(now));
	if (now != 0) {
		orientation_changed(now);
	}
}

static void
my_stop_orientation(__unused id self, __unused SEL selector)
{
	pthread_mutex_lock(&orientation_lock);
	if (orientation_handler != NULL) {
		Block_release(orientation_handler);
		orientation_handler = NULL;
	}
	if (orientation_queue != nil) {
		((void (*)(id, SEL))objc_msgSend)(orientation_queue, sel_registerName("release"));
		orientation_queue = nil;
	}
	pthread_mutex_unlock(&orientation_lock);
	say("backboardd stops the orientation's updates");
}

static id
my_orientation_blocking(__unused id self, __unused SEL selector)
{
	id o = make_orientation(orientation);

	return o ? ((id (*)(id, SEL))objc_msgSend)(o, sel_registerName("autorelease")) : nil;
}

static BOOL
my_orientation_available(__unused id self, __unused SEL selector)
{
	return YES;
}

/* CMDeviceOrientationManager's methods replaced. False when the class is not there. */
static bool
orientation_fixes(void)
{
	static const struct { const char *name; IMP imp; } methods[] = {
		{ "startDeviceOrientationUpdatesToQueue:withHandler:", (IMP)my_start_orientation },
		{ "stopDeviceOrientationUpdates", (IMP)my_stop_orientation },
		{ "deviceOrientationBlocking", (IMP)my_orientation_blocking },
		{ "isDeviceOrientationAvailable", (IMP)my_orientation_available },
	};
	Class cls = objc_getClass("CMDeviceOrientationManager");
	unsigned replaced = 0;

	if (cls == NULL && dlopen(COREMOTION, RTLD_NOW) != NULL) {
		cls = objc_getClass("CMDeviceOrientationManager");
	}
	if (cls == NULL) {
		say("no CMDeviceOrientationManager in %s: the orientation is not taken over", getprogname());
		return false;
	}
	for (unsigned i = 0; i < sizeof(methods) / sizeof(methods[0]); i++) {
		Method method = class_getInstanceMethod(cls, sel_registerName(methods[i].name));

		if (method != NULL) {
			method_setImplementation(method, methods[i].imp);
			replaced++;
		}
	}
	say("%u of CMDeviceOrientationManager's 4 methods replaced in %s: the orientation comes from the accelerometer", replaced,
	    getprogname());
	return replaced != 0;
}

/* the orientation from a sample (g, iOS's sign), filtered; a change is said after it held for HOLD_US */
static void
orientation_from(double x, double y, double z, uint64_t now_us)
{
	static double fx, fy, fz;
	static int candidate;
	static uint64_t since_us;
	static bool primed;
	double ax, ay, az;
	int o = 0;

	if (!primed) {
		fx = x, fy = y, fz = z;
		primed = true;
	}
	fx += 0.2 * (x - fx);
	fy += 0.2 * (y - fy);
	fz += 0.2 * (z - fz);
	ax = fx < 0 ? -fx : fx;
	ay = fy < 0 ? -fy : fy;
	az = fz < 0 ? -fz : fz;
	if (ay >= ax && ay >= az && ay > GRAVITY_MIN) {
		o = fy < 0 ? 1 : 2;
	} else if (ax >= ay && ax >= az && ax > GRAVITY_MIN) {
		o = fx < 0 ? 3 : 4;
	} else if (az > GRAVITY_MIN) {
		o = fz < 0 ? 5 : 6;
	}
	if (o == 0) {
		return;
	}
	if (o != candidate) {
		candidate = o;
		since_us = now_us;
		return;
	}
	if (o != orientation && now_us - since_us >= HOLD_US) {
		orientation_changed(o);
	}
}

static void *
sensors_thread(__unused void *arg)
{
	struct hakios_sensor_sample s[8];
	mach_timebase_info_data_t tb;
	uint64_t samples = 0;
	ssize_t n;

	pthread_setname_np("hakios orientation");
	mach_timebase_info(&tb);
	while ((n = read(device, s, sizeof(s))) >= (ssize_t)sizeof(s[0])) {
		for (unsigned i = 0; i < (size_t)n / sizeof(s[0]); i++) {
			double k, x, y, z;

			if (s[i].type != HAKIOS_SENSOR_ACCEL) {
				continue;
			}
			k = (s[i].scale > 0 ? s[i].scale : 8) / 32768.0;
			x = -s[i].v[0] * k, y = -s[i].v[1] * k, z = -s[i].v[2] * k;  /* Android's sign to iOS's */
			if (samples++ == 0) {
				say("the first accelerometer sample: %d %d %d counts = %.2f %.2f %.2f g in iOS's sign", s[i].v[0], s[i].v[1],
				    s[i].v[2], x, y, z);
			}
			orientation_from(x, y, z, s[i].time * tb.numer / tb.denom / 1000);
		}
	}
	say("the device's read ended (%zd): no more orientation changes", n);
	return NULL;
}

__attribute__((constructor))
static void
start(void)
{
	pthread_t thread;

	/* only when the kernel has the sensor hub running (shub=6): otherwise nothing of this happens */
	device = open(HAKIOS_SENSORS_DEVICE, O_RDWR);
	if (device < 0) {
		return;
	}
	if (!orientation_fixes() || pthread_create(&thread, NULL, sensors_thread, NULL) != 0) {
		close(device);
		device = -1;
		return;
	}
	pthread_detach(thread);
}
