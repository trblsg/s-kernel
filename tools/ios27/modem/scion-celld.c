// SPDX-License-Identifier: MIT-0
/*
 * scion-celld: the Galaxy S9's cellular data context (scion-kernel drivers/modem/hakios.shannon.c, cp_if=1: pdp_ip0)
 * made a network service iOS knows, so that names resolve and iOS routes over cellular when nothing better is up.
 *
 * It reads two of the kernel's sysctls every few seconds:
 *   kern.hakios.cp.info  one line: stage, registration, technology, bars, dBm, operator code, SIM, data (1: pdp_ip0 up)
 *   kern.hakios.cp.addr  root only: "ADDRESS DNS1 DNS2" of the active context, empty without one
 * and, while the context is up, publishes into configd's dynamic store (what CommCenter does on an iPhone):
 *   State:/Network/Service/<ID>        PrimaryRank = Last  (Wi-Fi, or anything else with a default route, wins)
 *   State:/Network/Service/<ID>/IPv4   Addresses, InterfaceName pdp_ip0, Router = the address (point to point: every
 *                                      route goes straight out of the interface)
 *   State:/Network/Service/<ID>/DNS    ServerAddresses = the context's DNS servers
 * IPMonitor then makes the default route, the resolver configuration and the network state from them as for any
 * service. When the context goes away the keys are removed. Nothing else is touched; no Apple file is changed.
 *
 * Without the modem (cp= absent: stage 0) or when its run has ended (state 100 or more) it exits 0 and launchd leaves
 * it. A line per change goes to /private/var/log/scion-celld.log (the addresses are never written there).
 *
 *   scion-celld          the daemon
 *   scion-celld -d       one look: the kernel's line and configd's global IPv4 and DNS state, then exit
 */
#include <CoreFoundation/CoreFoundation.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

/* SystemConfiguration's dynamic store (public C API; declared here because the iPhoneOS SDK does not ship its header) */
typedef const struct __SCDynamicStore *SCDynamicStoreRef;
extern SCDynamicStoreRef SCDynamicStoreCreate(CFAllocatorRef, CFStringRef, void *, void *);
extern Boolean SCDynamicStoreSetMultiple(SCDynamicStoreRef, CFDictionaryRef, CFArrayRef, CFArrayRef);
extern CFPropertyListRef SCDynamicStoreCopyValue(SCDynamicStoreRef, CFStringRef);
extern int SCError(void);
extern const char *SCErrorString(int);

#define SERVICE_ID      "5C10D000-CE11-4A7A-9000-5C1000000001"
#define LOG             "/private/var/log/scion-celld.log"

static void
say(const char *fmt, ...)
{
	char line[512];
	time_t t = time(NULL);
	struct tm tm;
	struct stat st;
	va_list ap;
	FILE *f;

	gmtime_r(&t, &tm);
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	if (stat(LOG, &st) == 0 && st.st_size > 256 * 1024) {
		(void)truncate(LOG, 0);
	}
	if ((f = fopen(LOG, "a")) != NULL) {
		fprintf(f, "%04d-%02d-%02dT%02d:%02d:%02dZ %s\n", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
		    tm.tm_min, tm.tm_sec, line);
		fclose(f);
	}
	fprintf(stderr, "scion-celld: %s\n", line);
}

static bool
read_line(const char *name, char *out, size_t n)
{
	size_t len = n;
	const char *fake = getenv("SCION_CELLD_FAKE");

	out[0] = 0;
	if (fake != NULL) {
		/* a model run off the phone: the file's first line stands for kern.hakios.cp.info, its second for .addr */
		FILE *f = fopen(fake, "r");
		char l1[256] = "", l2[256] = "";

		if (f == NULL) {
			return false;
		}
		if (fgets(l1, sizeof(l1), f) != NULL) {
			(void)fgets(l2, sizeof(l2), f);
		}
		fclose(f);
		strlcpy(out, strstr(name, ".addr") != NULL ? l2 : l1, n);
		out[strcspn(out, "\n")] = 0;
		return true;
	}
	if (sysctlbyname(name, out, &len, NULL, 0) != 0) {
		out[0] = 0;
		return false;
	}
	out[n - 1] = 0;
	return true;
}

/* the value of KEY=... in the kernel's info line, as an integer (DEF if absent) */
static long
field(const char *info, const char *key, long def)
{
	char pat[24];
	const char *p;

	snprintf(pat, sizeof(pat), "%s=", key);
	for (p = info; (p = strstr(p, pat)) != NULL; p++) {
		if (p == info || p[-1] == ' ') {
			return strtol(p + strlen(pat), NULL, 10);
		}
	}
	return def;
}

static bool
is_addr(const char *s)
{
	unsigned a, b, c, d;
	char x;

	return sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &x) == 4 && a > 0 && a < 224 && a != 127 && b < 256 && c < 256 &&
	       d < 256;
}

static CFStringRef
str(const char *s)
{
	return CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8);
}

static CFStringRef
key(const char *entity)
{
	return entity == NULL ? str("State:/Network/Service/" SERVICE_ID) :
	       CFStringCreateWithFormat(NULL, NULL, CFSTR("State:/Network/Service/" SERVICE_ID "/%s"), entity);
}

static bool
publish(SCDynamicStoreRef store, const char *ip, const char *dns1, const char *dns2)
{
	CFMutableDictionaryRef all = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	CFMutableDictionaryRef svc = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	CFMutableDictionaryRef v4 = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	CFMutableDictionaryRef dns = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	CFMutableArrayRef servers = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
	CFStringRef a = str(ip), k1 = key(NULL), k2 = key("IPv4"), k3 = key("DNS"), t;
	CFArrayRef addrs = CFArrayCreate(NULL, (const void **)&a, 1, &kCFTypeArrayCallBacks);
	Boolean ok;

	CFDictionarySetValue(svc, CFSTR("PrimaryRank"), CFSTR("Last"));
	CFDictionarySetValue(svc, CFSTR("UserDefinedName"), CFSTR("Cellular (scion-celld)"));
	CFDictionarySetValue(v4, CFSTR("Addresses"), addrs);
	CFDictionarySetValue(v4, CFSTR("DestAddresses"), addrs);
	CFDictionarySetValue(v4, CFSTR("InterfaceName"), getenv("SCION_CELLD_IF") != NULL ? str(getenv("SCION_CELLD_IF")) :
	    CFSTR("pdp_ip0"));      /* (SCION_CELLD_IF: the model run's stand-in interface; leaks one string, once) */
	CFDictionarySetValue(v4, CFSTR("Router"), a);
	if (is_addr(dns1)) {
		t = str(dns1);
		CFArrayAppendValue(servers, t);
		CFRelease(t);
	}
	if (is_addr(dns2) && strcmp(dns1, dns2) != 0) {
		t = str(dns2);
		CFArrayAppendValue(servers, t);
		CFRelease(t);
	}
	CFDictionarySetValue(dns, CFSTR("ServerAddresses"), servers);
	CFDictionarySetValue(all, k1, svc);
	CFDictionarySetValue(all, k2, v4);
	if (CFArrayGetCount(servers) != 0) {
		CFDictionarySetValue(all, k3, dns);
	}
	ok = SCDynamicStoreSetMultiple(store, all, NULL, NULL);
	if (!ok) {
		say("configd refused the service: %s", SCErrorString(SCError()));
	}
	CFRelease(all); CFRelease(svc); CFRelease(v4); CFRelease(dns); CFRelease(servers); CFRelease(a); CFRelease(addrs);
	CFRelease(k1); CFRelease(k2); CFRelease(k3);
	return ok;
}

static void
withdraw(SCDynamicStoreRef store)
{
	CFStringRef keys[3] = { key(NULL), key("IPv4"), key("DNS") };
	CFArrayRef gone = CFArrayCreate(NULL, (const void **)keys, 3, &kCFTypeArrayCallBacks);

	if (!SCDynamicStoreSetMultiple(store, NULL, gone, NULL)) {
		say("configd refused the removal: %s", SCErrorString(SCError()));
	}
	CFRelease(gone);
	for (int i = 0; i < 3; i++) {
		CFRelease(keys[i]);
	}
}

static void
show(SCDynamicStoreRef store, const char *k)
{
	CFStringRef s = str(k);
	CFPropertyListRef v = SCDynamicStoreCopyValue(store, s);
	char buf[2048] = "(none)";

	if (v != NULL) {
		CFStringRef d = CFCopyDescription(v);

		CFStringGetCString(d, buf, sizeof(buf), kCFStringEncodingUTF8);
		CFRelease(d);
		CFRelease(v);
	}
	printf("%s = %s\n", k, buf);
	CFRelease(s);
}

int
main(int argc, char **argv)
{
	SCDynamicStoreRef store;
	char info[256], addr[96], last[96] = "";
	bool up = false;
	long gen = -1;

	store = SCDynamicStoreCreate(NULL, CFSTR("scion-celld"), NULL, NULL);
	if (argc > 1 && strcmp(argv[1], "-d") == 0) {
		read_line("kern.hakios.cp.info", info, sizeof(info));
		printf("kern.hakios.cp.info = %s\n", info[0] ? info : "(none)");
		read_line("kern.hakios.cp.addr", addr, sizeof(addr));
		printf("kern.hakios.cp.addr = %s\n", addr[0] ? "(set; not shown)" : "(empty)");
		if (store == NULL) {
			printf("configd: no session\n");
			return 1;
		}
		show(store, "State:/Network/Global/IPv4");
		show(store, "State:/Network/Global/DNS");
		show(store, "State:/Network/Service/" SERVICE_ID "/IPv4");
		return 0;
	}
	if (!read_line("kern.hakios.cp.info", info, sizeof(info)) || field(info, "stage", 0) == 0) {
		say("no modem in this boot (cp= absent): nothing to do");
		return 0;
	}
	if (store == NULL) {
		say("no configd session: %s", SCErrorString(SCError()));
		return 1;
	}
	say("started: %s", info);
	withdraw(store);
	for (;;) {
		long state, data;

		if (!read_line("kern.hakios.cp.info", info, sizeof(info))) {
			info[0] = 0;
		}
		state = field(info, "state", 0);
		data = field(info, "data", 0);
		if (field(info, "gen", -1) != gen) {
			gen = field(info, "gen", -1);
			if (gen % 20 == 0 || data != up) {
				say("%s", info);
			}
		}
		read_line("kern.hakios.cp.addr", addr, sizeof(addr));
		if (data == 1 && addr[0] != 0) {
			if (!up || strcmp(addr, last) != 0) {
				char ip[32] = "", d1[32] = "", d2[32] = "";

				sscanf(addr, "%31s %31s %31s", ip, d1, d2);
				if (is_addr(ip) && publish(store, ip, d1, d2)) {
					up = true;
					strlcpy(last, addr, sizeof(last));
					say("pdp_ip0 published to configd as service " SERVICE_ID " (rank Last, %s DNS)",
					    is_addr(d1) ? (is_addr(d2) ? "two" : "one") : "no");
				}
			}
		} else if (up) {
			withdraw(store);
			up = false;
			last[0] = 0;
			say("the data context is gone: service withdrawn (%s)", info);
		}
		if (state >= 100) {
			say("the modem's run is over (%s): exiting", info);
			if (up) {
				withdraw(store);
			}
			return 0;
		}
		sleep(up ? 5 : 2);
	}
}
