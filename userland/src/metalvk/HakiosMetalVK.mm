/*
 * HakiosMetal, Metal on Vulkan (userland/build.sh METAL=vk; docs/metal-plan.md, "Metal on Vulkan"): the plug-in Metal loads when
 * the kernel names it (boot-arg metal=N), whose device is metal2vulkan's runtime on a Vulkan driver of Scion's.
 *
 * The bundle is made of four parts:
 *   this file      the bundle's executable: the class Metal takes (HakiosMetalDevice, MetalPluginClassName) and who is shown a device.
 *                  It links nothing but the system's frameworks, so a process that only asks whether there is a device pays for no more.
 *   HakiosMetalVK.dylib, in the bundle (HakiosMetalVKOpen.mm and NativeBridgeMetalFrontend.mm)
 *                  the Metal API: AdrenoMetal's frontend (MIT, NativeMetal contributors), every Metal object of its own handed to the
 *                  bridge; and the opening of the Vulkan device (which driver, the shader cache, PanVK's switches)
 *   libmetal2vulkan.dylib (/redwood/lib)
 *                  the bridge (AdrenoMetal's, MIT) over metal2vulkan's runtime and translator (GPL-2.0-or-later OR LGPL-3.0-or-later),
 *                  built by tools/metal/mk-metal2vulkan.sh; linked by HakiosMetalVK.dylib, never copied in, so that it can be replaced
 *   the Vulkan driver, PanVK on the Galaxy S9 (HakiosMetal.icd beside the bundle names it)
 *
 * What Metal does, as with the other build of this plug-in (src/HakiosMetal.m): it reads MetalPluginName and MetalPluginClassName off
 * IOAcceleratorES (the kernel's metal=N, MiniIOMobileFramebuffer.cpp), opens /System/Library/Extensions/HakiosMetal.bundle, takes the
 * class and sends it -initWithAcceleratorPort:, then MTLAddDevice() sends the object -initLimits and -initFeatureQueries and keeps it as
 * the process's device; MTLCreateSystemDefaultDevice() answers it from then on.
 *
 * The device is opened when it is first used, not when it is made. Many processes ask for the system's default device without ever
 * making anything with it, and opening one costs a process the 18 MB runtime, the Vulkan driver, a GPU context and its memory. So
 * -initWithAcceleratorPort: gives an object of this class, a _MTLDevice that answers what Metal's own registration sends (the four
 * -init... methods, which fill nothing: nothing was measured yet) and the device's fixed facts (-registryID, unified memory, not low
 * power, not removable, not headless). Every other message of MTLDevice and MTLDeviceSPI, and every method of _MTLDevice's own
 * but those its -init and the object's life use, goes to Objective-C's forwarding (_objc_msgForward for the methods _MTLDevice
 * has, the runtime's own path for the rest), and the first of them loads
 * HakiosMetalVK.dylib, opens the Vulkan device and puts the frontend's methods on this class (native_bridge_metal_adopt). The object
 * Metal and the program hold is then the device itself, and the message that opened it is sent again. When the device cannot be
 * opened, every such message is answered with nil or zero, and the log says why once.
 *
 * The window server (backboardd) is the exception: it opens the device in -initWithAcceleratorPort:, since CoreAnimation uses it within
 * a second anyway, and answers nil there when it cannot be opened. Metal adds no device for a nil answer (its discovery tests the
 * object before MTLAddDevice), so MTLCreateSystemDefaultDevice() is nil and CoreAnimation takes its OpenGL ES path, as on an image
 * without Metal. Opened at first use instead, a device that fails is found only after CoreAnimation has chosen Metal, and every
 * answer of nil after that ends the window server.
 *
 * Who is shown the device (src/hakios_metal_shown.c): metal=1 metal_test only, metal=2 applications and app extensions, metal=3 all but
 * backboardd, metal=4 all; backboardd at metal=2 or 3 when the driver is PanVK (camtl=0 or "-backboardd" keeps it on OpenGL ES); and
 * HakiosMetal.clients (beside the bundle, or /private/var/db). A process that is not shown it gets no class, and Metal no device.
 *
 * What it says goes to the console, to /private/var/tmp/HakiosMetal.log (up to 4 MiB) and to NSLog, one line each, with the process.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#import <objc/runtime.h>
#import <objc/objc-exception.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "../hakios_metal_shown.h"
#include "../p1274_user.h"
#include "p12751_layout_abi.h"

#if __has_feature(objc_arc)
#error "HakiosMetalVK.mm keeps its device by hand: build it without ARC"
#endif

#define LOG_FILE        "/private/var/tmp/HakiosMetal.log"
#define LOG_FILE_LIMIT  (4 << 20)
#define OPENER          "HakiosMetalVK.dylib"   /* in the bundle, beside this executable */

/* ---- what the plug-in says ------------------------------------------------------------------------------------------------------ */

static int consoleFd = -2, fileFd = -2;

static void
sayLine(const char *text)
{
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	char line[1024];
	struct stat st;
	int n;

	n = snprintf(line, sizeof(line), "HakiosMetal[%s %d]: %s\n", getprogname(), getpid(), text);
	if (n < 0) {
		return;
	}
	if (n >= (int)sizeof(line)) {
		n = sizeof(line) - 1;
		line[n - 1] = '\n';
	}
	pthread_mutex_lock(&lock);
	if (consoleFd == -2) {
		consoleFd = open("/dev/console", O_WRONLY | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
	}
	if (fileFd == -2) {
		fileFd = open(LOG_FILE, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0666);
		/* the first process to write it may be root's: every process the device is shown to may add to it */
		if (fileFd >= 0 && fstat(fileFd, &st) == 0 && st.st_uid == geteuid() && (st.st_mode & 0777) != 0666) {
			fchmod(fileFd, 0666);
		}
	}
	if (consoleFd >= 0) {
		(void)write(consoleFd, line, (size_t)n);
	}
	if (fileFd >= 0 && fstat(fileFd, &st) == 0 && st.st_size < LOG_FILE_LIMIT) {
		(void)write(fileFd, line, (size_t)n);
	}
	pthread_mutex_unlock(&lock);
	line[n - 1] = '\0';
	NSLog(@"%s", line);
}

static void
say(const char *format, ...) __attribute__((format(printf, 1, 2)));

static void
say(const char *format, ...)
{
	char text[768];
	va_list ap;

	va_start(ap, format);
	vsnprintf(text, sizeof(text), format, ap);
	va_end(ap);
	sayLine(text);
}

/* ---- the device, before and after it is opened ---------------------------------------------------------------------------------- */

enum { LAZY, OPENING, OPEN, FAILED };

static pthread_mutex_t gLock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
static volatile int gState = LAZY;
static bool p1275_layout_installed;
static void p12751_install_alignment_observer(void);
static unsigned p1275_deferred;
static Class gClass, gBase;             /* HakiosMetalDevice and _MTLDevice */
static id gDevice;                      /* the process's one device object, kept */
static uint64_t gRegistryID;
static SEL *gForwarded;                 /* the selectors of _MTLDevice's that the class forwards until the device is open */
static unsigned gForwardedCount;
static char gWhy[512];                  /* why the device could not be opened, or what was opened */

static uint64_t
nowMs(void)
{
	return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000;
}

/* the methods still forwarded go back to _MTLDevice's, where the frontend put none of its own: as a subclass that never forwarded */
static void
stopForwarding(void)
{
	for (unsigned i = 0; i < gForwardedCount; i++) {
		Method method = class_getInstanceMethod(gClass, gForwarded[i]);

		if (method != NULL && method_getImplementation(method) == (IMP)_objc_msgForward) {
			method_setImplementation(method, class_getMethodImplementation(gBase, gForwarded[i]));
		}
	}
}

/*
 * Who sent the message being forwarded: the first frames outside this bundle, the Objective-C runtime and Foundation's
 * forwarding, as image+offset, into OUT.
 */
static void
callers(char *out, size_t size)
{
	void *frames[24];
	int count = backtrace(frames, 24), told = 0;
	Dl_info self, info;

	out[0] = '\0';
	dladdr((const void *)&callers, &self);
	for (int i = 1; i < count && told < 3; i++) {
		const char *image, *name;

		if (dladdr(frames[i], &info) == 0 || info.dli_fname == NULL || info.dli_fbase == self.dli_fbase) {
			continue;
		}
		image = strrchr(info.dli_fname, '/') ? strrchr(info.dli_fname, '/') + 1 : info.dli_fname;
		if (strcmp(image, "libobjc.A.dylib") == 0 || strcmp(image, "CoreFoundation") == 0) {
			continue;
		}
		name = info.dli_sname != NULL ? info.dli_sname : "?";
		size_t used = strlen(out);
		snprintf(out + used, size - used, "%s%s+%#lx (%s)", told ? ", " : "", image,
		    (unsigned long)((uintptr_t)frames[i] - (uintptr_t)info.dli_fbase), name);
		told++;
	}
}

/*
 * Exceptions thrown in a process whose device is open, said with who threw them (the first 32): a Metal client that gets nil where
 * Apple's driver would have given an object usually throws (NanoUniverse: "object cannot be nil"), and an app extension's own
 * output goes nowhere. Thrown, not uncaught: one that is caught is said too, which is what makes the first of a series visible.
 */
static objc_exception_preprocessor gPreviousPreprocessor;

static id
sayException(id exception)
{
	static volatile int told;
	char from[512];

	if (told < 32 && [exception isKindOfClass:[NSException class]]) {
		told++;
		callers(from, sizeof(from));
		say("an exception is thrown: %s: %s; from %s", [[(NSException *)exception name] UTF8String] ?: "?",
		    [[(NSException *)exception reason] UTF8String] ?: "(no reason)", from);
	}
	return gPreviousPreprocessor != NULL ? gPreviousPreprocessor(exception) : exception;
}

/* load the opener and open the Vulkan device, once; WHY is the message that asked for it */
static void
openDevice(SEL why)
{
	typedef bool (*Opener)(id, uint64_t, void (*)(const char *), char *, size_t);
	char path[1024], *slash;
	Opener open;
	uint64_t began;
	Dl_info info;
	void *opener;

	char from[512];

	struct p1274_stage waitStage=p1274_stage_begin();
	pthread_mutex_lock(&gLock);
	if(waitStage.at && mach_absolute_time()-waitStage.at>0) {
		/* One event for the opener, not every forwarding waiter. */
		if(gState==LAZY && gDevice!=nil)p1274_stage_end(waitStage,1,1);
	}
	if (gState != LAZY || gDevice == nil) {
		pthread_mutex_unlock(&gLock);
		return;
	}
	gState = OPENING;
	unsigned p1275m=p1275_mode(),selectorCode=0;
	uint64_t p1275Began=0;
	if(p1275m && p1274_user_observing()){
		const char *selectorName=sel_getName(why);
		selectorCode=!strcmp(selectorName,"newTextureLayoutWithDescriptor:isHeapOrBufferBacked:")?1:
		    !strcmp(selectorName,"iosurfaceTextureAlignmentBytes")?2:
		    !strcmp(selectorName,"newCommandQueue")?3:
		    !strcmp(selectorName,"newTextureWithDescriptor:")?4:5;
		p1275Began=mach_absolute_time();
		(void)p1274_emit(6,105,selectorCode,p1275_deferred,p1275m,p1275Began,0,0);
	}
	if(p1275_deferred){
        if(p12751_user_boot())say("P12751 deferred_layout=%u later_open=%s",p1275_deferred,sel_getName(why));
        if(p1274_user_observing())(void)p1274_emit(6,101,p1275_deferred,0,0,0,0,0);
    }
	callers(from, sizeof(from));
	say("opening the device for -%s, sent from %s", sel_getName(why), from);
	began = nowMs();
	if (dladdr((const void *)&openDevice, &info) == 0 || info.dli_fname == NULL ||
	    strlcpy(path, info.dli_fname, sizeof(path)) >= sizeof(path) || (slash = strrchr(path, '/')) == NULL) {
		snprintf(gWhy, sizeof(gWhy), "this bundle's own path is not known");
	} else {
		strlcpy(slash + 1, OPENER, sizeof(path) - (size_t)(slash + 1 - path));
		struct p1274_stage loadStage=p1274_stage_begin();
		opener=dlopen(path, RTLD_NOW | RTLD_LOCAL);
		p1274_stage_end(loadStage,2,opener!=NULL);
		if (opener == NULL) {
			snprintf(gWhy, sizeof(gWhy), "%s", dlerror() ?: path);
		} else {
			struct p1274_stage symbolStage=p1274_stage_begin();
			open=(Opener)dlsym(opener, "hakios_metal_vk_open");
			p1274_stage_end(symbolStage,3,open!=NULL);
			if(open==NULL){
				snprintf(gWhy,sizeof(gWhy),"%s has no hakios_metal_vk_open",path);
			}else{
				struct p1274_stage runtimeStage=p1274_stage_begin();
				bool opened=open(gDevice,gRegistryID,sayLine,gWhy,sizeof(gWhy));
				p1274_stage_end(runtimeStage,4,opened);
				if(opened){
			stopForwarding();
			if(p12751_user_boot())p12751_install_alignment_observer();
			gState = OPEN;
			if(p1275Began)(void)p1274_emit(6,106,selectorCode,1,mach_absolute_time()-p1275Began,p1275Began,0,0);
			gPreviousPreprocessor = objc_setExceptionPreprocessor(sayException);
			say("the device is opened, at -%s, in %llu ms: %s", sel_getName(why), nowMs() - began, gWhy);
			pthread_mutex_unlock(&gLock);
			return;
				}
			}
		}
	}
	gState = FAILED;
	if(p1275Began)(void)p1274_emit(6,106,selectorCode,0,mach_absolute_time()-p1275Began,p1275Began,0,0);
	say("the device could not be opened (asked -%s, after %llu ms): %s; it answers nil or 0 from now on", sel_getName(why),
	    nowMs() - began, gWhy);
	pthread_mutex_unlock(&gLock);
}

/* Only the exact object-returning, descriptor + BOOL selector with a
 * matching inherited ABI is eligible. Unknown bootargs/probes fail closed.
 * We keep the original forwarding/open path for every other selector. */
static bool p1275_layout_disabled(void){
    const char *env=getenv("HAKIOS_METAL_LAYOUT");
    const char *plan=getenv("HAKIOS_METAL_LAYOUT_PLAN");
    if((env && !strcmp(env,"1")) || (plan && !strcmp(plan,"1")))return false;
    static const bool disabled=[]{
        char args[4096]={};size_t n=sizeof(args)-1;
        if(sysctlbyname("kern.bootargs",args,&n,NULL,0)!=0 || n>=sizeof(args))return false;
        args[n]=0;
        /* Conservative: any mention of experimental layout/probe excludes it. */
        return !strstr(args,"metal_layout=") && !strstr(args,"p_layout_probe=");
    }();
    return disabled;
}
static bool p12751_early_allowed(void){
    if(p12751_user_boot()){int mode=p12751_boot_mode();return mode>=0 && (mode&2);}
    return (p1275_cached_flags()&1u)!=0;
}
static id p1275_layout_shim(id self,SEL sel,MTLTextureDescriptor *d,BOOL backed){
    pthread_mutex_lock(&gLock);
    if(gState==LAZY && gDevice==self && p12751_early_allowed() && p1275_layout_disabled() && !hakios_metal_is_window_server()){
        unsigned n=++p1275_deferred;
        if(n==1 || n%32==0){
            if(p12751_user_boot())say("P12751 layout early_nil=%u backed=%u",n,(unsigned)backed);
            (void)p1274_emit(6,100,n,(uint64_t)backed,0,0,0,0);
        }
        pthread_mutex_unlock(&gLock);return nil;
    }
    pthread_mutex_unlock(&gLock);
    openDevice(sel);
    /* The frontend replaces this method on successful adoption. Never
     * recurse through a still-installed shim on a failed/reentrant open. */
    IMP imp=class_getMethodImplementation(object_getClass(self),sel);
    if(gState==OPEN && imp && imp!=(IMP)p1275_layout_shim && imp!=(IMP)_objc_msgForward)
        return ((id(*)(id,SEL,MTLTextureDescriptor *,BOOL))imp)(self,sel,d,backed);
    return nil;
}
static const char *p12751_protocol_encoding(Protocol *proto,SEL sel,unsigned depth){
    if(!proto || depth>12)return NULL;
    for(int required=0;required<2;required++){
        struct objc_method_description d=protocol_getMethodDescription(proto,sel,required,YES);
        if(d.name && d.types)return d.types;
    }
    unsigned n=0;Protocol *__unsafe_unretained *parents=protocol_copyProtocolList(proto,&n);
    const char *result=NULL;
    for(unsigned i=0;i<n && !result;i++)result=p12751_protocol_encoding(parents[i],sel,depth+1);
    free(parents);return result;
}
static bool p12751_layout_abi(const char *encoding){
    if(!encoding)return false;
    @try{
        NSMethodSignature *sig=[NSMethodSignature signatureWithObjCTypes:encoding];
        if(!sig || sig.numberOfArguments!=4 || sig.methodReturnType[0]!='@' || sig.methodReturnType[1]=='?' ||
            strcmp([sig getArgumentTypeAtIndex:0],"@") || strcmp([sig getArgumentTypeAtIndex:1],":"))return false;
        const char *arg=[sig getArgumentTypeAtIndex:2],*flag=[sig getArgumentTypeAtIndex:3];
        return arg[0]=='@' && arg[1]!='?' && !strcmp(flag,@encode(BOOL));
    }@catch(NSException *e){(void)e;return false;}
}
static void p1275_install_layout_shim(void){
    SEL sel=sel_registerName("newTextureLayoutWithDescriptor:isHeapOrBufferBacked:");
    Method m=class_getInstanceMethod(gBase,sel);
    const char *base=m?method_getTypeEncoding(m):NULL;
    const char *spi=p12751_protocol_encoding(objc_getProtocol("MTLDeviceSPI"),sel,0);
    const char *publicABI=p12751_protocol_encoding(@protocol(MTLDevice),sel,0);
    const char *compiled=p12751_protocol_encoding(@protocol(S9P12751LayoutABI),sel,0);
    char os[32]={0};size_t osn=sizeof(os)-1;
    bool knownOS=sysctlbyname("kern.osversion",os,&osn,NULL,0)==0 && osn<sizeof(os) && !strcmp(os,"20F66");
    const char *encoding=base?:spi?:publicABI;
    const char *source=base?"_MTLDevice":spi?"MTLDeviceSPI":publicABI?"MTLDevice":"missing";
    // No guessed encoding: fallback to the exact compiled frontend
    // declaration only on the pinned system build and only if no system
    // method/protocol metadata exists. Any present mismatch fails closed.
    if(!encoding && p12751_user_boot() && knownOS){encoding=compiled;source="compiled-20F66";}
    Method forwarded=class_getInstanceMethod(gClass,sel);
    const char *reason="installed";
    if(p12751_user_boot() && !p12751_early_allowed())reason="boot_disabled";
    else if(!encoding)reason="no_metadata";
    else if(!p12751_layout_abi(encoding) || !p12751_layout_abi(compiled) || (base&&!p12751_layout_abi(base)) ||
       (spi&&!p12751_layout_abi(spi)) || (publicABI&&!p12751_layout_abi(publicABI)))reason="abi_mismatch";
    else if(forwarded && method_getImplementation(forwarded)!=(IMP)_objc_msgForward)reason="real_method";
    else if(forwarded){class_replaceMethod(gClass,sel,(IMP)p1275_layout_shim,encoding);p1275_layout_installed=true;}
    else if(class_addMethod(gClass,sel,(IMP)p1275_layout_shim,encoding))p1275_layout_installed=true;
    else reason="add_failed";
    if(p12751_user_boot())say("P12751 layout source=%s base=%s spi=%s public=%s compiled=%s os=%s bool=%s ABI=%u reason=%s",
       source,base?: "-",spi?: "-",publicABI?: "-",compiled?: "-",os,@encode(BOOL),p1275_layout_installed?1:0,reason);
}
/* Natural-call observation only. Never manufacture an alignment answer. */
static IMP p12751_alignment_original;
static unsigned p12751_alignment_calls;
static NSUInteger p12751_alignment_observer(id self,SEL sel){
    NSUInteger value=((NSUInteger(*)(id,SEL))p12751_alignment_original)(self,sel);
    unsigned n=__atomic_add_fetch(&p12751_alignment_calls,1,__ATOMIC_RELAXED);
    if(n==1 || n%64==0){
        say("P12751 alignment calls=%u value=%llu",n,(unsigned long long)value);
        (void)p1274_emit(6,107,value,n,0,0,0,0);
    }
    return value;
}
static void p12751_install_alignment_observer(void){
    SEL sel=sel_registerName("iosurfaceTextureAlignmentBytes");
    Method m=class_getInstanceMethod(gClass,sel);
    if(!m){say("P12751 alignment ABI missing");return;}
    const char *encoding=method_getTypeEncoding(m);
    IMP imp=method_getImplementation(m);
    char type[16]={0};method_getReturnType(m,type,sizeof(type));
    if(!imp || imp==(IMP)_objc_msgForward || imp==(IMP)p12751_alignment_observer ||
       method_getNumberOfArguments(m)!=2 || strcmp(type,@encode(NSUInteger))){
        say("P12751 alignment ABI refused encoding=%s",encoding?:"-");return;
    }
    p12751_alignment_original=imp;
    class_replaceMethod(gClass,sel,(IMP)p12751_alignment_observer,encoding);
    say("P12751 alignment observation installed encoding=%s",encoding);
}

/* whether MTLDevice or MTLDeviceSPI (or a protocol either takes in) declares SELECTOR as an instance method */
static bool
deviceProtocolHas(SEL selector)
{
	static const char *const names[] = { "MTLDevice", "MTLDeviceSPI" };

	for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		Protocol *protocol = objc_getProtocol(names[i]);

		if (protocol != NULL && (protocol_getMethodDescription(protocol, selector, YES, YES).name != NULL ||
		    protocol_getMethodDescription(protocol, selector, NO, YES).name != NULL)) {
			return true;
		}
	}
	return false;
}

/*
 * The methods of HakiosMetalDevice before its device is open. They are written on NSObject here (_MTLDevice is not in the SDK's
 * Metal.tbd, so it cannot be subclassed in source) and copied onto the class when it is made. The answers are the frontend's own
 * (NBDeviceMethods in NativeBridgeMetalFrontend.mm), which replace these when the device opens: the two must stay the same.
 */
@interface HakiosMetalLazyDevice : NSObject
@end

@implementation HakiosMetalLazyDevice
- (uint64_t)registryID {return gRegistryID;}
- (BOOL)hasUnifiedMemory {return YES;}
- (uint64_t)dedicatedMemorySize {return 0;}
- (uint64_t)sharedMemorySize {return [NSProcessInfo processInfo].physicalMemory;}
- (BOOL)isLowPower {return NO;}
- (BOOL)isRemovable {return NO;}
- (BOOL)isHeadless {return NO;}
- (BOOL)supportsFeatureSet:(MTLFeatureSet)set {(void)set; return NO;}
/* MTLAddDevice() sends the first two to every device it registers; they would fill the base class's tables from an Apple GPU's profile */
- (void)initLimits {}
- (void)initFeatureQueries {}
- (void)initGPUFamilySupport {}
- (void)initWorkarounds {}
/*
 * Not Metal's: whether this device was opened for the window server in -initWithAcceleratorPort:, which userland/src/inject.c asks in
 * backboardd before CoreAnimation takes the device (without camtl=, it takes out any device that does not answer YES).
 */
- (BOOL)hakiosOpenedForWindowServer {return gState == OPEN && hakios_metal_is_window_server();}

/* every forwarded message: the device is opened here (or was already), and the message is sent again by forwardInvocation: */
- (id)forwardingTargetForSelector:(SEL)selector
{
	openDevice(selector);
	return nil;
}

- (void)forwardInvocation:(NSInvocation *)invocation
{
	static NSMutableSet *told;
	SEL selector = invocation.selector;
	NSUInteger length;

	if (gState == OPEN) {
		if (class_getMethodImplementation(object_getClass(self), selector) != (IMP)_objc_msgForward) {
			[invocation invokeWithTarget:self];
		} else {
			[self doesNotRecognizeSelector:selector];
		}
		return;
	}
	if ((length = invocation.methodSignature.methodReturnLength) != 0) {
		void *zero = calloc(1, length);

		[invocation setReturnValue:zero];
		free(zero);
	}
	@synchronized([HakiosMetalLazyDevice class]) {
		NSString *name = NSStringFromSelector(selector);

		if (told == nil) {
			told = [NSMutableSet new];
		}
		if (told.count < 64 && ![told containsObject:name]) {
			[told addObject:name];
			say("-%s answered with nil or 0: the device is not open (%s)", sel_getName(selector),
			    gState == OPENING ? "asked while it was being opened" : gWhy);
		}
	}
}

/* a method of the protocols that only the frontend has is not on the class yet: asking about one opens the device */
- (BOOL)respondsToSelector:(SEL)selector
{
	Class cls = object_getClass(self);

	if (gState == LAZY && !class_respondsToSelector(cls, selector) && deviceProtocolHas(selector)) {
		openDevice(selector);
	}
	return class_respondsToSelector(cls, selector);
}
@end

/*
 * -initWithAcceleratorPort:, as Metal sends it ([[cls alloc] initWithAcceleratorPort:]): _MTLDevice's own -init, nothing opened. The
 * object is the process's one device; a second call answers the first object (the second allocation is kept, not released: -[_MTLDevice
 * dealloc] on an object whose -init never ran is not something to bet on).
 */
static id
initWithAcceleratorPort(id self, SEL selector, unsigned port)
{
	struct objc_super base = { self, gBase };
	id made;

	(void)selector;
	if (port == 0) {
		return nil;
	}
	pthread_mutex_lock(&gLock);
	if (gDevice != nil) {
		/* the window server's device that could not be opened stays no device */
		made = gState == FAILED && hakios_metal_is_window_server() ? nil : [gDevice retain];
		pthread_mutex_unlock(&gLock);
		return made;
	}
	made = ((id (*)(struct objc_super *, SEL))objc_msgSendSuper)(&base, @selector(init));
	if (made != nil) {
		gDevice = [made retain];
		IORegistryEntryGetRegistryEntryID(port, &gRegistryID);
	}
	if (made != nil && hakios_metal_is_window_server()) {
		/* opened now, and no device if it cannot be (the head of this file); the object stays in gDevice, never released */
		say("a device for the accelerator port %u; opened now, for the window server", port);
		openDevice(selector);
		if (gState != OPEN) {
			say("no device for the window server: CoreAnimation draws with OpenGL ES");
			[made release];
			made = nil;
		}
		pthread_mutex_unlock(&gLock);
		return made;
	}
	pthread_mutex_unlock(&gLock);
	if (made != nil) {
		say("a device for the accelerator port %u; it is opened at the first message that needs the GPU", port);
	}
	return made;
}

/* forward SELECTOR until the device is open, if _MTLDevice has a method of that name (any other reaches forwarding by itself) */
static void
forwardOne(SEL selector)
{
	Method inherited = class_getInstanceMethod(gBase, selector);

	if (inherited == NULL || class_getInstanceMethod(gClass, selector) != inherited) {
		return;         /* none to forward, or the class has a method of its own for it (an answer, or forwarded already) */
	}
	if (class_addMethod(gClass, selector, (IMP)_objc_msgForward, method_getTypeEncoding(inherited))) {
		SEL *grown = (SEL *)realloc(gForwarded, (gForwardedCount + 1) * sizeof(SEL));

		if (grown != NULL) {
			gForwarded = grown;
			gForwarded[gForwardedCount++] = selector;
		}
	}
}

/*
 * _MTLDevice's own methods that are not in the protocols: Metal's internals (pipeline variants, function keys, binary archives,
 * IO handles, the compiler) call them on a device, and the frontend replaces some of them. Before the device is open they would run
 * _MTLDevice's own code on an object no driver filled; so they are forwarded as well, and the device opens at the first. Kept as
 * they are: what _MTLDevice's own -init and Metal's registration use, and the object's life.
 */
static void
forwardBaseMethods(void)
{
	static const char *const kept[] = {
		"dealloc", ".cxx_construct", ".cxx_destruct", "init", "initWithAcceleratorPort:", "initProgressTracking",
		"initSerializationPaths", "allowLibrariesFromOtherPlatforms", "globalTraceObjectID", "formattedDescription:",
		"serialQueue", "concurrentQueue", "_incrementCommandQueueCount", "_decrementCommandQueueCount", "_purgeDevice",
	};
	unsigned count;
	Method *methods = class_copyMethodList(gBase, &count);

	for (unsigned i = 0; i < count; i++) {
		const char *name = sel_getName(method_getName(methods[i]));
		bool keep = false;

		for (unsigned k = 0; k < sizeof(kept) / sizeof(kept[0]) && !keep; k++) {
			keep = strcmp(name, kept[k]) == 0;
		}
		if (!keep) {
			forwardOne(method_getName(methods[i]));
		}
	}
	free(methods);
}

static void
forwardProtocol(Protocol *protocol)
{
	unsigned count, i;
	Protocol * __unsafe_unretained *taken;

	if (protocol == NULL || strcmp(protocol_getName(protocol), "NSObject") == 0) {
		return;
	}
	for (int required = 0; required < 2; required++) {
		struct objc_method_description *methods = protocol_copyMethodDescriptionList(protocol, required, YES, &count);

		for (i = 0; i < count; i++) {
			forwardOne(methods[i].name);
		}
		free(methods);
	}
	taken = protocol_copyProtocolList(protocol, &count);
	for (i = 0; i < count; i++) {
		forwardProtocol(taken[i]);
	}
	free(taken);
}

__attribute__((constructor)) static void
registerDevice(void)
{
	unsigned count;
	Method *methods;
	Protocol *spi;

	if (!hakios_metal_shown(sayLine)) {
		return;
	}
	if ((gBase = objc_getClass("_MTLDevice")) == Nil) {
		say("this system has no _MTLDevice: no device class is registered");
		return;
	}
	if (objc_getClass("HakiosMetalDevice") != Nil || (gClass = objc_allocateClassPair(gBase, "HakiosMetalDevice", 0)) == Nil) {
		say("could not make the class HakiosMetalDevice");
		return;
	}
	class_addMethod(gClass, @selector(initWithAcceleratorPort:), (IMP)initWithAcceleratorPort, "@20@0:8I16");
	methods = class_copyMethodList([HakiosMetalLazyDevice class], &count);
	for (unsigned i = 0; i < count; i++) {
		class_addMethod(gClass, method_getName(methods[i]), method_getImplementation(methods[i]), method_getTypeEncoding(methods[i]));
	}
	free(methods);
	/* what MTLAddDevice asserts of the class it is given (docs/metal-plan.md, step 1); the frontend's device declares the same */
	class_addProtocol(gClass, @protocol(MTLDevice));
	if ((spi = objc_getProtocol("MTLDeviceSPI")) != NULL) {
		class_addProtocol(gClass, spi);
	}
	forwardProtocol(@protocol(MTLDevice));
	forwardProtocol(spi);
	forwardBaseMethods();
	if(p1275_user_boot()){@autoreleasepool {p1275_install_layout_shim();}}
	objc_registerClassPair(gClass);
	if(p1275_user_boot())say("P1275 typed layout shim ABI=%u",p1275_layout_installed?1:0);
	say("the class HakiosMetalDevice is registered (Metal on Vulkan, opened at first use; %u of _MTLDevice's methods forwarded "
	    "until then)", gForwardedCount);
}
