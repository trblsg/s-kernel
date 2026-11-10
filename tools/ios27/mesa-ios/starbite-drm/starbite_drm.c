/*
 * Project Starbite: the libdrm of xf86drm.h (see there) for Mesa's Panfrost
 * on iOS. Panfrost's DRM requests go to the kernel's StarbiteMali user client
 * as IOConnectCallStructMethod calls:
 *
 *   DRM_COMMAND_BASE + n (Panfrost's SUBMIT .. MADVISE)  -> selector n
 *   DRM_IOCTL_GEM_CLOSE                                   -> selector 0x100
 *
 * with the request's structure going in and coming back as it is. The
 * kernel maps a buffer into this process itself when asked for its "mmap
 * offset": the offset Mesa gets back is the address (Mesa's pan_kmod_bo_mmap
 * is patched to use it as such, tools/ios27/mesa-ios/panfrost-ios.patch).
 *
 * Panfrost knows its device by a file descriptor. It gets one that means
 * nothing (/dev/null): there is one GPU and one connection to it in a
 * process.
 *
 * After HakiOS tools/mesa/hakios-drm/hakios.drm.c (MIT, Copyright the HakiOS
 * authors), reworked for Panfrost and StarbiteMali.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>
#include <sys/ioccom.h>
#include <time.h>
#include <unistd.h>

#include <IOKit/IOKitLib.h>

#include "xf86drm.h"

#define SBM_SERVICE     "StarbiteMali"
#define SBM_GEM_CLOSE   0x100
#define SBM_SURFACE     0x103
#define SBM_IMPORT_HOST 0x104
#define SBM_ASYNC       0x105
#define SBM_WAIT_IDLE   0x106

static io_connect_t connection;
static int connection_fd = -1;
static uint32_t next_syncobj;
/*
 * Asynchronous submits. The kernel (boot-arg sweetspeed&64) starts a chain and returns; a
 * fence (syncobj) wait is then a wait for the GPU to be done with everything submitted so far (the
 * kernel runs chains one at a time, in order), WAIT_BO waits for the chains that named the buffer.
 * STARBITE_DRM_SYNC=1: synchronous submits as before.
 */
static bool async_on;

/*
 * The connection to StarbiteMali, opened at the first request whoever makes it: Panfrost GL comes
 * through drmOpenWithType(), PanVK opens the device node of drmGetDevices2() itself.
 */
static bool
starbite_connect(void)
{
	if (connection != IO_OBJECT_NULL) {
		return true;
	}
	io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(SBM_SERVICE));
	if (service == IO_OBJECT_NULL) {
		return false;
	}
	kern_return_t kr = IOServiceOpen(service, mach_task_self(), 0, &connection);
	IOObjectRelease(service);
	if (kr != KERN_SUCCESS) {
		return false;
	}
	if (getenv("STARBITE_DRM_SYNC") == NULL) {
		uint32_t flag = 1;
		size_t returned = sizeof(flag);
		if (IOConnectCallStructMethod(connection, SBM_ASYNC, &flag, sizeof(flag), &flag, &returned) == KERN_SUCCESS &&
		    flag != 0) {
			async_on = true;
		}
	}
	return true;
}

int
drmOpenWithType(const char *name, const char *busid, int type)
{
	(void)busid;
	(void)type;
	if (name == NULL || strcmp(name, "panfrost") != 0) {
		return -1;
	}
	if (connection_fd >= 0) {
		return dup(connection_fd);
	}
	if (!starbite_connect()) {
		return -1;
	}
	connection_fd = open("/dev/null", O_RDWR | O_CLOEXEC);
	return dup(connection_fd);
}

/* STARBITE_DRM_TRACE=1: the first 300 calls and every failed one on stderr. */
static int trace = -1;

static void
trace_call(uint32_t selector, const void *data, size_t size, kern_return_t kr)
{
	const uint32_t *w = data;
	const uint64_t *q = data;

	if (selector == 0 && size >= 48) {             /* SUBMIT: jc, bo count, requirements */
		fprintf(stderr, "starbite-drm: SUBMIT jc %#llx bos %u req %#x -> %#x\n",
		    (unsigned long long)q[0], w[8], w[9], kr);
	} else if (selector == 2 && size >= 16) {      /* CREATE_BO: size, flags -> handle, va */
		fprintf(stderr, "starbite-drm: CREATE_BO size %#x flags %#x -> handle %u va %#llx (%#x)\n",
		    w[0], w[1], w[2], (unsigned long long)q[2], kr);
	} else {
		fprintf(stderr, "starbite-drm: sel %#x size %zu first %#x -> %#x\n", selector, size,
		    size >= 4 ? w[0] : 0, kr);
	}
}


/*
 * Who holds the GPU's memory (the iOS 26 port ran out of RAM with 1.3 GB of buffers wired):
 * every buffer this process creates or imports is kept with the call stack that made it, and
 * every 15 s the call sites holding the most live memory are appended to /var/local/mtl2.log as
 * "SBDRM <proc> live N MiB in M buffers; site: K MiB in J (made T) <- lib+off ...".
 * STARBITE_DRM_LEAKS=0: off.
 */
#define LEAK_FRAMES 10
#define LEAK_SITES  512
#define LEAK_HANDLES 65536
struct leak_site {
	void *pcs[LEAK_FRAMES];
	uint64_t live_bytes, live, made;
};
static struct leak_site leak_sites[LEAK_SITES];
static unsigned leak_nsites;
static struct { uint64_t bytes; uint16_t site; bool used; } leak_handles[LEAK_HANDLES];
static pthread_mutex_t leak_lock = PTHREAD_MUTEX_INITIALIZER;
static int leaks_on = -1;
static time_t leak_last;

static void
leak_dump_locked(void)
{
	FILE *f = fopen("/var/local/mtl2.log", "a");
	if (f == NULL) {
		return;
	}
	uint64_t all = 0, count = 0;
	for (unsigned i = 0; i < leak_nsites; i++) {
		all += leak_sites[i].live_bytes;
		count += leak_sites[i].live;
	}
	fprintf(f, "SBDRM %s[%d] live %llu MiB in %llu buffers\n", getprogname(), getpid(),
	    (unsigned long long)(all >> 20), (unsigned long long)count);
	bool shown[LEAK_SITES] = { false };
	for (unsigned k = 0; k < 8; k++) {
		int best = -1;
		for (unsigned i = 0; i < leak_nsites; i++) {
			if (!shown[i] && leak_sites[i].live_bytes > 0 &&
			    (best < 0 || leak_sites[i].live_bytes > leak_sites[best].live_bytes)) {
				best = (int)i;
			}
		}
		if (best < 0) {
			break;
		}
		shown[best] = true;
		struct leak_site *s = &leak_sites[best];
		fprintf(f, "SBDRM %s  %llu KiB in %llu (made %llu) <-", getprogname(),
		    (unsigned long long)(s->live_bytes >> 10), (unsigned long long)s->live,
		    (unsigned long long)s->made);
		for (unsigned j = 0; j < LEAK_FRAMES && s->pcs[j] != NULL; j++) {
			Dl_info info;
			if (dladdr(s->pcs[j], &info) && info.dli_fname != NULL) {
				const char *base = strrchr(info.dli_fname, '/');
				fprintf(f, " %s+%#lx", base ? base + 1 : info.dli_fname,
				    (unsigned long)((uintptr_t)s->pcs[j] - (uintptr_t)info.dli_fbase));
			} else {
				fprintf(f, " %p", s->pcs[j]);
			}
		}
		fprintf(f, "\n");
	}
	fclose(f);
}

static void
leak_note(uint32_t handle, uint64_t bytes, bool made)
{
	if (leaks_on < 0) {
		const char *e = getenv("STARBITE_DRM_LEAKS");
		leaks_on = e == NULL || *e != '0';
	}
	if (!leaks_on || handle >= LEAK_HANDLES) {
		return;
	}
	void *pcs[LEAK_FRAMES + 3] = { NULL };
	if (made) {
		backtrace(pcs, LEAK_FRAMES + 3);
	}
	pthread_mutex_lock(&leak_lock);
	if (made) {
		unsigned i;
		for (i = 0; i < leak_nsites; i++) {
			if (memcmp(leak_sites[i].pcs, pcs + 3, sizeof(leak_sites[i].pcs)) == 0) {
				break;
			}
		}
		if (i == leak_nsites && leak_nsites < LEAK_SITES) {
			memcpy(leak_sites[i].pcs, pcs + 3, sizeof(leak_sites[i].pcs));
			leak_nsites++;
		}
		if (i < leak_nsites) {
			leak_sites[i].live_bytes += bytes;
			leak_sites[i].live++;
			leak_sites[i].made++;
			leak_handles[handle].bytes = bytes;
			leak_handles[handle].site = (uint16_t)i;
			leak_handles[handle].used = true;
		}
	} else if (leak_handles[handle].used) {
		struct leak_site *s = &leak_sites[leak_handles[handle].site];
		s->live_bytes -= leak_handles[handle].bytes;
		s->live--;
		leak_handles[handle].used = false;
	}
	time_t now = time(NULL);
	if (now - leak_last >= 15) {
		leak_last = now;
		leak_dump_locked();
	}
	pthread_mutex_unlock(&leak_lock);
}

static int
call(uint32_t selector, void *data, size_t size)
{
	size_t returned = size;
	if (!starbite_connect()) {
		return ENODEV;
	}
	kern_return_t kr = IOConnectCallStructMethod(connection, selector, data, size, data, &returned);

	if (trace < 0) {
		const char *t = getenv("STARBITE_DRM_TRACE");
		trace = t != NULL && *t == '1';
	}
	static unsigned traced;
	if (trace && (traced++ < 300 || kr != KERN_SUCCESS)) {
		trace_call(selector, data, size, kr);
	}

	if (kr == KERN_SUCCESS) {
		const uint32_t *w = data;
		const uint64_t *q = data;
		if (selector == 2 && size >= 16) {                     /* CREATE_BO: size, flags -> handle */
			leak_note(w[2], w[0], true);
		} else if (selector == SBM_IMPORT_HOST && size >= 20) { /* addr, size -> handle */
			leak_note(w[4], q[1], true);
		} else if (selector == SBM_GEM_CLOSE && size >= 4) {
			leak_note(w[0], 0, false);
		}
	}

	switch (kr) {
	case KERN_SUCCESS:
		return 0;
	case kIOReturnNoMemory:
	case kIOReturnNoSpace:
		return ENOMEM;
	case kIOReturnBadArgument:
		return EINVAL;
	case kIOReturnTimeout:
		return ETIMEDOUT;
	case kIOReturnBusy:
		return EBUSY;
	case kIOReturnNotFound:
		return ENOENT;
	case kIOReturnUnsupported:
		return ENOSYS;
	default:
		return EIO;
	}
}

/*
 * Requests counted by selector (0-15; 16 = GEM_CLOSE, 17 = others), and every 10 s one line on the kernel's console through
 * StarbiteMali's LOG selector: "STARLTE GPU APP: drm[pid] 10 s: submit N wait N madvise N(local) ...". Only with
 * STARBITE_DRM_STATS=1 in the environment (a clock read and two atomics a request otherwise saved).
 * MADVISE is answered here: StarbiteMali never purges a buffer (its MADVISE only checks the handle and says
 * "retained"), and Panfrost's BO cache asks twice per cached buffer reuse. STARBITE_DRM_KMADVISE=1 asks the kernel again.
 */
static uint64_t sel_count[18], sel_last_ns, mmap_local;
uint64_t sbdrm_flush_reason[10];                         /* pan_job.c: batches submitted, by why */
#define MMAP_CACHE 8193u                                /* StarbiteMali's handles are 1..8192 */
static uint64_t mmap_cache[MMAP_CACHE];
static void
count_and_report(uint32_t selector)
{
	static int stats = -1;
	if (stats < 0) {
		stats = getenv("STARBITE_DRM_STATS") != NULL;
	}
	if (!stats) {
		return;
	}
	__atomic_fetch_add(&sel_count[selector < 16 ? selector : selector == SBM_GEM_CLOSE ? 16 : 17], 1, __ATOMIC_RELAXED);
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	uint64_t now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
	uint64_t last = __atomic_load_n(&sel_last_ns, __ATOMIC_RELAXED);
	if (last == 0) {
		__atomic_store_n(&sel_last_ns, now, __ATOMIC_RELAXED);
		return;
	}
	if (now - last < 10000000000ull || !__atomic_compare_exchange_n(&sel_last_ns, &last, now, false, __ATOMIC_RELAXED,
	    __ATOMIC_RELAXED)) {
		return;
	}
	uint64_t c[18];
	for (int i = 0; i < 18; i++) {
		c[i] = __atomic_exchange_n(&sel_count[i], 0, __ATOMIC_RELAXED);
	}
	char text[200];
	int n = snprintf(text, sizeof(text), "drm[%d] %llu s: submit %llu wait %llu create %llu mmap %llu param %llu "
	    "offset %llu madvise %llu(local) close %llu other %llu; mmap local %llu", getpid(), (unsigned long long)((now - last) / 1000000000ull),
	    c[0], c[1], c[2], c[3], c[4], c[5], c[8], c[16], c[6] + c[7] + c[9] + c[10] + c[11] + c[12] + c[13] + c[14] + c[15] + c[17],
	    (unsigned long long)__atomic_exchange_n(&mmap_local, 0, __ATOMIC_RELAXED));
	if (n > 0 && n < (int)sizeof(text)) {
		size_t returned = 0;
		(void)IOConnectCallStructMethod(connection, 0x102, text, (size_t)n + 1, NULL, &returned);
	}
	uint64_t f[10];
	for (int i = 0; i < 10; i++) {
		f[i] = __atomic_exchange_n(&sbdrm_flush_reason[i], 0, __ATOMIC_RELAXED);
	}
	n = snprintf(text, sizeof(text), "drm[%d] batches by why: lru %llu fresh %llu dep-writer %llu dep-readers %llu "
	    "flush-all %llu flush-writer %llu flush-users %llu; non-empty %llu; renamed %llu", getpid(), f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8]);
	if (n > 0 && n < (int)sizeof(text)) {
		size_t returned = 0;
		(void)IOConnectCallStructMethod(connection, 0x102, text, (size_t)n + 1, NULL, &returned);
	}
}

int
drmIoctl(int fd, unsigned long request, void *arg)
{
	uint32_t nr = (uint32_t)(request & 0xff);
	uint32_t selector;

	(void)fd;
	static int kmadvise = -1;
	if (kmadvise < 0) {
		kmadvise = getenv("STARBITE_DRM_KMADVISE") != NULL;
	}
	if (nr >= DRM_COMMAND_BASE && nr - DRM_COMMAND_BASE == 8 && IOCPARM_LEN(request) >= 12 && !kmadvise) {
		((uint32_t *)arg)[2] = 1;                       /* MADVISE: retained */
		count_and_report(8);
		return 0;
	}
	/*
	 * MMAP_BO's answer is the kernel's mapping of the buffer, made once and kept until GEM_CLOSE
	 * (StarbiteMali mmapBO); Panfrost asks again every time it takes a buffer out of its cache. Remember it per handle.
	 */
	if (nr >= DRM_COMMAND_BASE && nr - DRM_COMMAND_BASE == 3 && IOCPARM_LEN(request) >= 16) {
		uint32_t h = ((uint32_t *)arg)[0];
		uint64_t at = h < MMAP_CACHE ? __atomic_load_n(&mmap_cache[h], __ATOMIC_RELAXED) : 0;
		if (at != 0) {
			((uint64_t *)arg)[1] = at;
			__atomic_fetch_add(&mmap_local, 1, __ATOMIC_RELAXED);
			count_and_report(3);
			return 0;
		}
	}
	if (request == DRM_IOCTL_GEM_CLOSE) {
		uint32_t h = ((uint32_t *)arg)[0];
		if (h < MMAP_CACHE) {
			__atomic_store_n(&mmap_cache[h], 0, __ATOMIC_RELAXED);
		}
	}
	if (nr >= DRM_COMMAND_BASE) {
		selector = nr - DRM_COMMAND_BASE;
	} else if (request == DRM_IOCTL_GEM_CLOSE) {
		selector = SBM_GEM_CLOSE;
	} else {
		if (getenv("STARBITE_DRM_TRACE") != NULL) {
			fprintf(stderr, "starbite-drm: ioctl %#lx not supported\n", request);
		}
		errno = ENOSYS;
		return -1;
	}
	count_and_report(selector);
	int error = call(selector, arg, IOCPARM_LEN(request));
	if (error != 0) {
		errno = error;
		return -1;
	}
	if (selector == 3 && IOCPARM_LEN(request) >= 16 && ((uint32_t *)arg)[0] < MMAP_CACHE) {
		__atomic_store_n(&mmap_cache[((uint32_t *)arg)[0]], ((uint64_t *)arg)[1], __ATOMIC_RELAXED);
	}
	return 0;
}

int
drmCloseBufferHandle(int fd, uint32_t handle)
{
	struct drm_gem_close close = { .handle = handle };
	return drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &close);
}

/*
 * The kernel speaks Panfrost 1.3, but Mesa then asks for the system
 * timestamp parameters, which it does not have: 1.1 (heap buffers,
 * allocated in full by the kernel) is all Mesa needs.
 */
drmVersionPtr
drmGetVersion(int fd)
{
	drmVersionPtr version = calloc(1, sizeof(*version));

	(void)fd;
	if (version != NULL) {
		version->version_major = 1;
		version->version_minor = 1;
		version->name = strdup("panfrost");
		version->name_len = (int)strlen(version->name);
		version->date = strdup("");
		version->desc = strdup("Mali-G72 on Starbite XNU");
		version->desc_len = (int)strlen(version->desc);
	}
	return version;
}

void
drmFreeVersion(drmVersionPtr version)
{
	if (version != NULL) {
		free(version->name);
		free(version->date);
		free(version->desc);
		free(version);
	}
}

int
drmGetCap(int fd, uint64_t capability, uint64_t *value)
{
	(void)fd;
	(void)capability;
	*value = 0;
	return 0;
}

int
drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd)
{
	(void)fd;
	(void)handle;
	(void)flags;
	(void)prime_fd;
	errno = ENOSYS;
	return -1;
}

/* Describe only: no buffer is made (size and layout for Mesa's import). */
int
starbite_drm_surface(int fd, struct starbite_drm_surface *surface)
{
	(void)fd;
	surface->flags = 1;
	int error = call(SBM_SURFACE, surface, sizeof(*surface));
	if (error != 0) {
		errno = error;
		return -1;
	}
	return 0;
}

int
starbite_drm_import_host(int fd, void *addr, uint64_t size, uint32_t *handle)
{
	struct {
		uint64_t addr, size;
		uint32_t handle, pad;
		uint64_t va;
	} request = { .addr = (uint64_t)(uintptr_t)addr, .size = size };

	(void)fd;
	int error = call(SBM_IMPORT_HOST, &request, sizeof(request));
	if (error != 0) {
		errno = error;
		return -1;
	}
	*handle = request.handle;
	return 0;
}

int
drmPrimeFDToHandle(int fd, int surface, uint32_t *handle)
{
	struct starbite_drm_surface request = { .surface = (uint32_t)surface };

	(void)fd;
	int error = call(SBM_SURFACE, &request, sizeof(request));
	if (error != 0) {
		errno = error;
		return -1;
	}
	*handle = request.handle;
	return 0;
}

int
drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle)
{
	(void)fd;
	(void)flags;
	*handle = __atomic_add_fetch(&next_syncobj, 1, __ATOMIC_RELAXED);
	return 0;
}

int
drmSyncobjDestroy(int fd, uint32_t handle)
{
	(void)fd;
	(void)handle;
	return 0;
}

int
drmSyncobjReset(int fd, const uint32_t *handles, uint32_t count)
{
	(void)fd;
	(void)handles;
	(void)count;
	return 0;
}

int
drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t count)
{
	(void)fd;
	(void)handles;
	(void)count;
	return 0;
}

int
drmSyncobjWait(int fd, uint32_t *handles, unsigned num_handles, int64_t timeout_nsec,
    unsigned flags, uint32_t *first_signaled)
{
	(void)fd;
	(void)handles;
	(void)num_handles;
	(void)flags;
	if (first_signaled != NULL) {
		*first_signaled = 0;
	}
	if (async_on) {
		/* timeout_nsec is an absolute CLOCK_MONOTONIC time: 0 (or past) = just look */
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		int64_t now = (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
		int64_t rel = timeout_nsec < 0 ? INT64_MAX : timeout_nsec > now ? timeout_nsec - now : 0;
		int error = call(SBM_WAIT_IDLE, &rel, sizeof(rel));
		if (error == ETIMEDOUT) {
			errno = ETIME;
			return -ETIME;
		}
	}
	return 0;
}

int
drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points, unsigned num_handles,
    int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled)
{
	(void)points;
	return drmSyncobjWait(fd, handles, num_handles, timeout_nsec, flags, first_signaled);
}

int
drmSyncobjTransfer(int fd, uint32_t dst_handle, uint64_t dst_point, uint32_t src_handle,
    uint64_t src_point, uint32_t flags)
{
	(void)fd;
	(void)dst_handle;
	(void)dst_point;
	(void)src_handle;
	(void)src_point;
	(void)flags;
	return 0;
}

/* A signalled fence as a file: /dev/null, which poll() always finds ready. */
int
drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd)
{
	(void)fd;
	(void)handle;
	*sync_file_fd = open("/dev/null", O_RDWR | O_CLOEXEC);
	return *sync_file_fd < 0 ? -1 : 0;
}

int
drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd)
{
	return drmSyncobjExportSyncFile(fd, handle, obj_fd);
}

int
drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd)
{
	(void)fd;
	(void)handle;
	(void)sync_file_fd;
	return 0;
}

int
drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle)
{
	(void)obj_fd;
	return drmSyncobjCreate(fd, 0, handle);
}

/* ---- devices (PanVK) ---------------------------------------------------------------------------- */

static char starbite_node[] = "/dev/null";
static char *starbite_nodes[DRM_NODE_MAX] = { starbite_node, starbite_node, starbite_node };
static char starbite_compatible_0[] = "arm,mali-bifrost";
static char *starbite_compatible[] = { starbite_compatible_0, NULL };
static drmPlatformBusInfo starbite_bus = { "/starbite/mali" };
static drmPlatformDeviceInfo starbite_info = { starbite_compatible };

static drmDevicePtr
starbite_device(void)
{
	drmDevicePtr d = calloc(1, sizeof(*d));
	if (d != NULL) {
		d->nodes = starbite_nodes;
		d->available_nodes = (1 << DRM_NODE_PRIMARY) | (1 << DRM_NODE_RENDER);
		d->bustype = DRM_BUS_PLATFORM;
		d->businfo.platform = &starbite_bus;
		d->deviceinfo.platform = &starbite_info;
	}
	return d;
}

int
drmGetDevices2(uint32_t flags, drmDevicePtr devices[], int max_devices)
{
	(void)flags;
	if (devices == NULL || max_devices <= 0) {
		return 1;
	}
	devices[0] = starbite_device();
	return devices[0] != NULL ? 1 : -ENOMEM;
}

void
drmFreeDevices(drmDevicePtr devices[], int count)
{
	for (int i = 0; devices != NULL && i < count; i++) {
		drmFreeDevice(&devices[i]);
	}
}

int
drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device)
{
	(void)fd;
	(void)flags;
	*device = starbite_device();
	return *device != NULL ? 0 : -ENOMEM;
}

void
drmFreeDevice(drmDevicePtr *device)
{
	if (device != NULL) {
		free(*device);
		*device = NULL;
	}
}

int
drmDevicesEqual(drmDevicePtr a, drmDevicePtr b)
{
	return a != NULL && b != NULL;
}

int
drmSyncobjTimelineSignal(int fd, const uint32_t *handles, uint64_t *points, uint32_t count)
{
	(void)fd;
	(void)handles;
	(void)points;
	(void)count;
	return 0;
}

int
drmSyncobjQuery2(int fd, uint32_t *handles, uint64_t *points, uint32_t count, uint32_t flags)
{
	(void)fd;
	(void)handles;
	(void)flags;
	/*
	 * "Every point is reached" has to be true when it is said. With asynchronous submits it is once the GPU is
	 * done with everything submitted so far, so wait for that first (the kernel's queue: a snapshot of the
	 * last chain queued now, not of chains queued later). Without the wait a caller that reads a queue's timeline
	 * value right after another thread's submit takes chains that have not run yet for finished and reuses their
	 * memory under the GPU (seen as an MMU fault at address 0 and a GPU reset once the kernel really queued chains).
	 */
	if (async_on) {
		int64_t rel = INT64_MAX;
		(void)call(SBM_WAIT_IDLE, &rel, sizeof(rel));
	}
	/* every point is reached: report the furthest any timeline could be */
	for (uint32_t i = 0; i < count; i++) {
		points[i] = UINT64_MAX >> 1;
	}
	return 0;
}

int
drmSyncobjQuery(int fd, uint32_t *handles, uint64_t *points, uint32_t count)
{
	return drmSyncobjQuery2(fd, handles, points, count, 0);
}
