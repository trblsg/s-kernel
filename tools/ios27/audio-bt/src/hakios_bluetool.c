/*
 * hakios.bluetool.dylib: BlueTool on the Galaxy S9 under iOS 27 (Scion; from scion-kernel userland/src/inject_bluetool.c,
 * the iOS 16.5 version, APSL as that file). BlueTool runs the boot and init scripts bluetoothd asks for; it looks for
 * /etc/bluetool/<HW model>.<script>.script only where MobileGestalt's ReleaseType is "Vendor" or "Internal", else it
 * picks one of Apple's built-in firmware sequences, none of which fits the S9's BCM4361 (the kernel's bt=7 has powered,
 * patched and raised it already). So, in BlueTool and nowhere else, ReleaseType answers "Vendor" (through both
 * MGCopyAnswer and MGGetStringAnswer, which iOS 27's BlueTool imports); every other answer is MobileGestalt's own. The
 * iOS 27 root is writable, so the scripts live at /etc/bluetool/ itself (no redirection as on 16.5).
 */
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <unistd.h>

extern CFTypeRef MGCopyAnswer(CFStringRef key, CFDictionaryRef options);
extern CFStringRef MGGetStringAnswer(CFStringRef key);

__attribute__((constructor)) static void
hello(void)
{
	fprintf(stderr, "hakios.bluetool: in BlueTool (pid %d): ReleaseType Vendor\n", getpid());
}

static CFTypeRef
my_MGCopyAnswer(CFStringRef key, CFDictionaryRef options)
{
	if (key != NULL && CFEqual(key, CFSTR("ReleaseType"))) {
		return CFRetain(CFSTR("Vendor"));
	}
	return MGCopyAnswer(key, options);
}

static CFStringRef
my_MGGetStringAnswer(CFStringRef key)
{
	if (key != NULL && CFEqual(key, CFSTR("ReleaseType"))) {
		return CFSTR("Vendor");
	}
	return MGGetStringAnswer(key);
}

__attribute__((used)) static const struct { const void *replacement, *replacee; } interposers[]
    __attribute__((section("__DATA,__interpose"))) = {
	{ (const void *)my_MGCopyAnswer, (const void *)MGCopyAnswer },
	{ (const void *)my_MGGetStringAnswer, (const void *)MGGetStringAnswer },
};
