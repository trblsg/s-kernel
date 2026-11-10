/*
 * hal_probe: load a CoreAudio HAL plug-in (an AudioServerPlugIn bundle) the way the HAL does -- CFPlugIn, the factory for
 * kAudioServerPlugInTypeUUID, QueryInterface for the driver interface, Initialize with a host -- list its devices, and
 * drive the output of the one named by UID ("Speaker") for SECONDS with a quiet 440 Hz tone at -30 dBFS (16-bit stereo,
 * 48 kHz): the plug-in tested without the audio server. Scion, APSL.
 *   hal_probe [BUNDLE] [UID] [SECONDS] [AMPLITUDE]   (defaults: HakiosAudio.driver in the HAL directory, Speaker, 3, 1036)
 * Run as the root's /private/var/herolte/autorun (once per boot), its output in /private/var/herolte/autorun.out.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreFoundation/CFPlugInCOM.h>
#include <AudioToolbox/AudioToolbox.h>
#include <mach/mach_time.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef UInt32 AudioObjectID;
typedef struct { UInt32 mSelector, mScope, mElement; } AudioObjectPropertyAddress;
typedef struct AudioServerPlugInHostInterface AudioServerPlugInHostInterface;
typedef const AudioServerPlugInHostInterface *AudioServerPlugInHostRef;
struct AudioServerPlugInHostInterface {
	OSStatus (*PropertiesChanged)(AudioServerPlugInHostRef, AudioObjectID, UInt32, const AudioObjectPropertyAddress *);
	OSStatus (*CopyFromStorage)(AudioServerPlugInHostRef, CFStringRef, CFPropertyListRef *);
	OSStatus (*WriteToStorage)(AudioServerPlugInHostRef, CFStringRef, CFPropertyListRef);
	OSStatus (*DeleteFromStorage)(AudioServerPlugInHostRef, CFStringRef);
	OSStatus (*RequestDeviceConfigurationChange)(AudioServerPlugInHostRef, AudioObjectID, UInt64, void *);
};
typedef struct Drv Drv;
typedef Drv **DrvRef;
struct Drv {
	void *_reserved;
	HRESULT (*QueryInterface)(void *, REFIID, LPVOID *);
	ULONG (*AddRef)(void *);
	ULONG (*Release)(void *);
	OSStatus (*Initialize)(DrvRef, AudioServerPlugInHostRef);
	void *CreateDevice, *DestroyDevice, *AddDeviceClient, *RemoveDeviceClient, *PerformConfig, *AbortConfig;
	Boolean (*HasProperty)(DrvRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *);
	void *IsPropertySettable;
	OSStatus (*GetPropertyDataSize)(DrvRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, UInt32, const void *, UInt32 *);
	OSStatus (*GetPropertyData)(DrvRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, UInt32, const void *, UInt32,
	    UInt32 *, void *);
	void *SetPropertyData;
	OSStatus (*StartIO)(DrvRef, AudioObjectID, UInt32);
	OSStatus (*StopIO)(DrvRef, AudioObjectID, UInt32);
	OSStatus (*GetZeroTimeStamp)(DrvRef, AudioObjectID, UInt32, Float64 *, UInt64 *, UInt64 *);
	OSStatus (*WillDoIOOperation)(DrvRef, AudioObjectID, UInt32, UInt32, Boolean *, Boolean *);
	OSStatus (*BeginIOOperation)(DrvRef, AudioObjectID, UInt32, UInt32, UInt32, const void *);
	OSStatus (*DoIOOperation)(DrvRef, AudioObjectID, AudioObjectID, UInt32, UInt32, UInt32, const void *, void *, void *);
	OSStatus (*EndIOOperation)(DrvRef, AudioObjectID, UInt32, UInt32, UInt32, const void *);
};
#define TYPE_UUID CFUUIDGetConstantUUIDWithBytes(NULL, 0x44, 0x3A, 0xBA, 0xB8, 0xE7, 0xB3, 0x49, 0x1A, 0xB9, 0x85, 0xBE, 0xB9, 0x18, 0x70, 0x30, 0xDB)
#define DRV_UUID CFUUIDGetConstantUUIDWithBytes(NULL, 0xEE, 0xA5, 0x77, 0x3D, 0xCC, 0x43, 0x49, 0xF1, 0x8E, 0x00, 0x8F, 0x96, 0xE7, 0xD2, 0x3B, 0x17)

static OSStatus h_changed(AudioServerPlugInHostRef h, AudioObjectID o, UInt32 n, const AudioObjectPropertyAddress *a)
{ (void)h; printf("host: PropertiesChanged object %u, %u addresses (first '%.4s')\n", o, n, n ? (char *)&a[0].mSelector : "-"); return 0; }
static OSStatus h_copy(AudioServerPlugInHostRef h, CFStringRef k, CFPropertyListRef *d) { (void)h; (void)k; *d = NULL; return 0; }
static OSStatus h_write(AudioServerPlugInHostRef h, CFStringRef k, CFPropertyListRef d) { (void)h; (void)k; (void)d; return 0; }
static OSStatus h_delete(AudioServerPlugInHostRef h, CFStringRef k) { (void)h; (void)k; return 0; }
static OSStatus h_req(AudioServerPlugInHostRef h, AudioObjectID o, UInt64 a, void *i) { (void)h; (void)o; (void)a; (void)i; return 0; }
static const AudioServerPlugInHostInterface host = { h_changed, h_copy, h_write, h_delete, h_req };

static void
fourcc(UInt32 v, char out[5]) { out[0] = (char)(v >> 24); out[1] = (char)(v >> 16); out[2] = (char)(v >> 8); out[3] = (char)v; out[4] = 0; }

/* the whole iOS path: an AudioQueue (AudioToolbox) plays SECONDS of the tone through the audio server, which routes it */
static double aq_phase; static int aq_amp; static UInt32 aq_left;
static void
aq_fill(void *ud, AudioQueueRef q, AudioQueueBufferRef b)
{
	(void)ud;
	int16_t *s = b->mAudioData; UInt32 frames = b->mAudioDataBytesCapacity / 4;
	for (UInt32 f = 0; f < frames; f++) { int16_t v = aq_left ? (int16_t)(aq_amp * sin(aq_phase)) : 0; s[2 * f] = s[2 * f + 1] = v; aq_phase += 2 * M_PI * 440 / 48000; }
	b->mAudioDataByteSize = frames * 4;
	aq_left = aq_left > frames ? aq_left - frames : 0;
	AudioQueueEnqueueBuffer(q, b, 0, NULL);
}
static void
aq_play(int secs, int amp)
{
	AudioStreamBasicDescription f = { 48000, kAudioFormatLinearPCM, kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked, 4, 1, 4, 2, 16, 0 };
	AudioQueueRef q = NULL; OSStatus st;
	aq_amp = amp; aq_left = (UInt32)secs * 48000;
	st = AudioQueueNewOutput(&f, aq_fill, NULL, NULL, NULL, 0, &q);
	printf("hal_probe: AudioQueueNewOutput %d\n", (int)st);
	if (st != 0) { return; }
	for (int i = 0; i < 3; i++) { AudioQueueBufferRef b; if (AudioQueueAllocateBuffer(q, 4800 * 4, &b) == 0) { aq_fill(NULL, q, b); } }
	st = AudioQueueStart(q, NULL);
	printf("hal_probe: AudioQueueStart %d: %d s of the tone through the audio server\n", (int)st, secs);
	CFRunLoopRunInMode(kCFRunLoopDefaultMode, secs + 1, false);
	AudioQueueStop(q, true); AudioQueueDispose(q, true);
	printf("hal_probe: AudioQueue done\n");
}

/* a system sound: the audio server plays it itself (SystemSoundServer), so no audio session of ours is needed */
static void
system_sound(SystemSoundID id)
{
	__block int done = 0;
	printf("hal_probe: AudioServicesPlaySystemSound(%u)\n", (unsigned)id);
	AudioServicesPlaySystemSoundWithCompletion(id, ^{ done = 1; });
	for (int i = 0; i < 40 && !done; i++) { CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, false); }
	printf("hal_probe: system sound %u %s\n", (unsigned)id, done ? "completed" : "did not complete in 4 s");
}

int
main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "/System/Library/Audio/Plug-Ins/HAL/HakiosAudio.driver";
	const char *want = argc > 2 ? argv[2] : "Speaker";
	int secs = argc > 3 ? atoi(argv[3]) : 3, amp = argc > 4 ? atoi(argv[4]) : 1036;   /* 1036 = -30 dBFS */
	setvbuf(stdout, NULL, _IOLBF, 0);
	printf("hal_probe: pid %d, %s, device %s, %d s at amplitude %d\n", getpid(), path, want, secs, amp);
	sleep(20);   /* after boot: let the system settle */
	CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), true);
	CFPlugInRef plugin = CFPlugInCreate(NULL, url);
	if (plugin == NULL) { printf("hal_probe: CFPlugInCreate failed\n"); return 1; }
	CFArrayRef factories = CFPlugInFindFactoriesForPlugInTypeInPlugIn(TYPE_UUID, plugin);
	if (factories == NULL || CFArrayGetCount(factories) == 0) { printf("hal_probe: no AudioServerPlugIn factory\n"); return 1; }
	IUnknownVTbl **unk = CFPlugInInstanceCreate(NULL, (CFUUIDRef)CFArrayGetValueAtIndex(factories, 0), TYPE_UUID);
	if (unk == NULL) { printf("hal_probe: the factory made nothing\n"); return 1; }
	DrvRef drv = NULL;
	HRESULT hr = (*unk)->QueryInterface(unk, CFUUIDGetUUIDBytes(DRV_UUID), (LPVOID *)&drv);
	printf("hal_probe: QueryInterface %d, driver %p\n", (int)hr, (void *)drv);
	if (drv == NULL) { return 1; }
	OSStatus st = (*drv)->Initialize(drv, &host);
	printf("hal_probe: Initialize %d\n", (int)st);
	AudioObjectPropertyAddress a = { 'dev#', 'glob', 0 };
	AudioObjectID devs[16], speaker = 0, out = 0;
	UInt32 size = 0;
	st = (*drv)->GetPropertyData(drv, 1, getpid(), &a, 0, NULL, sizeof(devs), &size, devs);
	printf("hal_probe: device list %d, %u devices\n", (int)st, size / 4);
	for (UInt32 i = 0; i < size / 4; i++) {
		CFStringRef uid = NULL; UInt32 s2 = 0; char u[64] = "?";
		AudioObjectPropertyAddress ua = { 'uid ', 'glob', 0 };
		if ((*drv)->GetPropertyData(drv, devs[i], getpid(), &ua, 0, NULL, sizeof(uid), &s2, &uid) == 0 && uid != NULL) {
			CFStringGetCString(uid, u, sizeof(u), kCFStringEncodingUTF8);
		}
		AudioObjectID streams[8]; UInt32 s3 = 0;
		AudioObjectPropertyAddress sa = { 'stm#', 'outp', 0 };
		(void)(*drv)->GetPropertyData(drv, devs[i], getpid(), &sa, 0, NULL, sizeof(streams), &s3, streams);
		printf("hal_probe: device %u UID %s, %u output stream(s)%s\n", devs[i], u, s3 / 4, s3 ? "" : "");
		if (strcmp(u, want) == 0 && s3 >= 4) { speaker = devs[i]; out = streams[0]; }
	}
	if (speaker == 0) { printf("hal_probe: no %s with an output\n", want); return 1; }
	st = (*drv)->StartIO(drv, speaker, 1);
	printf("hal_probe: StartIO %d on %u, stream %u\n", (int)st, speaker, out);
	mach_timebase_info_data_t tb; mach_timebase_info(&tb);
	int16_t buf[480 * 2]; double ph = 0; UInt32 cycles = 0;
	UInt64 t0 = mach_absolute_time(), per = (UInt64)(10000000.0 * tb.denom / tb.numer);   /* 10 ms */
	for (int c = 0; c < secs * 100; c++) {
		Float64 stime; UInt64 htime, seed;
		(*drv)->GetZeroTimeStamp(drv, speaker, 1, &stime, &htime, &seed);
		for (int f = 0; f < 480; f++) { int16_t v = (int16_t)(amp * sin(ph)); buf[2 * f] = buf[2 * f + 1] = v; ph += 2 * M_PI * 440 / 48000; }
		(*drv)->BeginIOOperation(drv, speaker, 1, 'rite', 480, NULL);
		st = (*drv)->DoIOOperation(drv, speaker, out, 1, 'rite', 480, NULL, buf, NULL);
		(*drv)->EndIOOperation(drv, speaker, 1, 'rite', 480, NULL);
		if (st != 0) { char f4[5]; fourcc((UInt32)st, f4); printf("hal_probe: DoIOOperation %d '%s'\n", (int)st, f4); break; }
		cycles++;
		mach_wait_until(t0 + per * (UInt64)(c + 1));
	}
	st = (*drv)->StopIO(drv, speaker, 1);
	printf("hal_probe: %u cycles of 480 frames written, StopIO %d\n", cycles, (int)st);
	/* the microphone: the device "Codec" (one 16-bit channel), ReadInput for 3 s, its level */
	AudioObjectID codec = 0, in = 0;
	for (UInt32 i = 0; i < size / 4; i++) {
		CFStringRef uid = NULL; UInt32 s2 = 0; char u[64] = "?";
		AudioObjectPropertyAddress ua = { 'uid ', 'glob', 0 };
		if ((*drv)->GetPropertyData(drv, devs[i], getpid(), &ua, 0, NULL, sizeof(uid), &s2, &uid) == 0 && uid != NULL) {
			CFStringGetCString(uid, u, sizeof(u), kCFStringEncodingUTF8);
		}
		AudioObjectID streams[8]; UInt32 s3 = 0;
		AudioObjectPropertyAddress sa = { 'stm#', 'inpt', 0 };
		(void)(*drv)->GetPropertyData(drv, devs[i], getpid(), &sa, 0, NULL, sizeof(streams), &s3, streams);
		if (strcmp(u, "Codec") == 0 && s3 >= 4) { codec = devs[i]; in = streams[0]; }
	}
	if (codec == 0) { printf("hal_probe: no Codec input\n"); aq_play(secs, amp); return 0; }
	st = (*drv)->StartIO(drv, codec, 2);
	printf("hal_probe: StartIO %d on the Codec %u, stream %u: 3 s of the microphone\n", (int)st, codec, in);
	int16_t mic[480]; double sum = 0; int peak = 0; UInt64 n = 0, zeros = 0;
	t0 = mach_absolute_time();
	for (int c = 0; c < 300; c++) {
		mach_wait_until(t0 + per * (UInt64)(c + 1));
		(*drv)->BeginIOOperation(drv, codec, 2, 'read', 480, NULL);
		st = (*drv)->DoIOOperation(drv, codec, in, 2, 'read', 480, NULL, mic, NULL);
		(*drv)->EndIOOperation(drv, codec, 2, 'read', 480, NULL);
		if (c < 50) { continue; }   /* the first 0.5 s: the codec and the DMA starting */
		for (int f = 0; f < 480; f++) { int v = mic[f]; sum += (double)v * v; n++; zeros += v == 0; v = v < 0 ? -v : v; peak = v > peak ? v : peak; }
		if (c % 50 == 49) { printf("hal_probe: mic %d.%d s: RMS %.1f, peak %d, %llu of %llu samples 0\n", (c + 1) / 100, (c + 1) % 100 / 10, n ? sqrt(sum / (double)n) : 0, peak, zeros, n); }
	}
	st = (*drv)->StopIO(drv, codec, 2);
	printf("hal_probe: microphone StopIO %d\n", (int)st);
	aq_play(secs, amp);
	system_sound(1104);
	printf("hal_probe: done\n");
	return 0;
}
