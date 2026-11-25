/*
 * mali_test: the Galaxy S9's GPU from a program, without Mesa -- what HakiosMali (drivers/gpu/mali/hakios.mali.cpp) does
 * for a client, one request at a time, so that a run of the phone says whether the kernel's half works before Mesa's
 * panfrost is asked to draw through it.
 *
 *   1  open HakiosMali (there only once the kernel's bring-up has reached its last step: mali=9)
 *   2  its parameters: which GPU, its cores and its MMU, as panfrost asks for them (GET_PARAM)
 *   3  a buffer (CREATE_BO): its handle and its address in the GPU's space; mapped here (the client memory of the
 *      type that is its handle)
 *   4  a job written into it -- the GPU's simplest, "write value": put 0xc0ffee01 at an address -- submitted (SUBMIT),
 *      waited for by its number (0x7d) and by its buffer (WAIT_BO), and the word looked at
 *   5  the buffer given back (GEM_CLOSE)
 *
 * The job is the one the kernel's step 8 runs from inside (hakios.mali.c, run_write_job()): a job header with the type
 * 2 (write value) and 64-bit pointers at +0x10, the target at +0x20, the kind of value (6: a 32-bit immediate) at
 * +0x28, the value at +0x30. Written through the buffer's write-combining mapping, it is in memory for the GPU.
 *
 *   mali_test          all of it, once
 *   mali_test N        the job N times, and how long a round trip takes
 */
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/sysctl.h>
#include <unistd.h>

enum {
	kDrmGemClose = 0x09, kSubmit = 0x40, kWaitBO = 0x41, kCreateBO = 0x42, kMmapBO = 0x43, kGetParam = 0x44,
	kGetBOOffset = 0x45, kWaitSeq = 0x7d,
};

struct gem_close { uint32_t handle, pad; };
struct submit { uint64_t jc, in_syncs; uint32_t in_sync_count, out_sync; uint64_t bo_handles; uint32_t bo_handle_count, requirements; };
struct wait_bo { uint32_t handle, pad; int64_t timeout_ns; };
struct create_bo { uint32_t size, flags, handle, pad; uint64_t offset; };
struct mmap_bo { uint32_t handle, flags; uint64_t offset; };
struct get_param { uint32_t param, pad; uint64_t value; };
struct wait_seq { uint64_t seq; int64_t timeout_ns; };

static io_connect_t gpu;

static kern_return_t
request(uint32_t selector, void *structure, size_t size)
{
	size_t out = size;

	return IOConnectCallStructMethod(gpu, selector, structure, size, structure, &out);
}

static double
ms_since(uint64_t then)
{
	static mach_timebase_info_data_t tb;

	if (tb.denom == 0) {
		mach_timebase_info(&tb);
	}
	return (double)(mach_absolute_time() - then) * tb.numer / tb.denom / 1e6;
}

/* A new IOKit connection reproduces the failing GET_PARAM path, not
 * a kernel-only call. No buffer allocation or job in params-only mode. */
#define CAP_COUNT 41u
struct probe_state {
    unsigned eligible,domain,pll,mux,wake,off,wake_fail,fault,safe,tz;
    int display;
};
static bool
probe_state(struct probe_state *s)
{
    char line[320];size_t n=sizeof(line);
    if(sysctlbyname("kern.hakios.p1272_probe_state",line,&n,NULL,0) || n==0 || n>sizeof(line))return false;
    line[sizeof(line)-1]=0;
    return sscanf(line,"version=1 eligible=%u display=%d domain=%u pll=%u mux=%u wake=%u off=%u wake_fail=%u fault=%u safe=%u tz=%u",
        &s->eligible,&s->display,&s->domain,&s->pll,&s->mux,&s->wake,&s->off,&s->wake_fail,&s->fault,&s->safe,&s->tz)==11;
}
static bool
probe_enabled(void)
{
    char args[1024];size_t n=sizeof(args);
    if(sysctlbyname("kern.bootargs",args,&n,NULL,0) || n==0 || n>sizeof(args))return false;
    args[sizeof(args)-1]=0;
    char *save=NULL;
    for(char *arg=strtok_r(args," ",&save);arg;arg=strtok_r(NULL," ",&save))
        if(!strcmp(arg,"p1272_probe=1"))return true;
    return false;
}
static bool
probe_requested(void)
{
    int v=0;size_t n=sizeof(v);
    return sysctlbyname("kern.hakios.p1272_probe_request",&v,&n,NULL,0)==0 && n==sizeof(v) && v==1;
}
static bool
probe_no_trials(void)
{
    char line[512];size_t n=sizeof(line);
    unsigned active,armed,mode;
    if(sysctlbyname("kern.hakios.p125_status",line,&n,NULL,0) || n==0 || n>sizeof(line))return false;
    line[sizeof(line)-1]=0;
    if(sscanf(line,"version=1 active=%u armed=%u",&active,&armed)!=2 || active || armed)return false;
    n=sizeof(line);
    if(sysctlbyname("kern.hakios.p127_status",line,&n,NULL,0) || n==0 || n>sizeof(line))return false;
    line[sizeof(line)-1]=0;
    if(sscanf(line,"version=1 mode=%u",&mode)!=1 || mode)return false;
    n=sizeof(line);
    if(sysctlbyname("kern.hakios.p126_status",line,&n,NULL,0) || n==0 || n>sizeof(line))return false;
    line[sizeof(line)-1]=0;
    return sscanf(line,"version=1 mode=%u",&mode)==1 && mode==0;
}
static bool
get_all_caps(uint64_t values[CAP_COUNT])
{
    for(uint32_t i=0;i<CAP_COUNT;i++){
        struct get_param p={.param=i,.value=UINT64_MAX};size_t out=sizeof(p);
        kern_return_t kr=IOConnectCallStructMethod(gpu,kGetParam,&p,sizeof(p),&p,&out);
        if(kr!=KERN_SUCCESS || out!=sizeof(p) || p.param!=i){
            printf("p1272-probe: GET_PARAM %u refused %#x bytes=%zu\n",i,kr,out);return false;
        }
        values[i]=p.value;
        printf("p1272-probe: param=%u value=%#llx\n",i,(unsigned long long)p.value);
    }
    struct get_param bad={.param=CAP_COUNT};
    if(request(kGetParam,&bad,sizeof(bad))==KERN_SUCCESS)return false;
    return true;
}
int
main(int argc, char **argv)
{
	static const struct { uint32_t param; const char *name; } params[] = {
		{ 0, "GPU_PROD_ID" }, { 1, "GPU_REVISION" }, { 2, "SHADER_PRESENT" }, { 3, "TILER_PRESENT" }, { 4, "L2_PRESENT" },
		{ 6, "AS_PRESENT" }, { 7, "JS_PRESENT" }, { 12, "MMU_FEATURES" }, { 17, "COHERENCY_FEATURES" },
		{ 38, "NR_CORE_GROUPS" }, { 39, "THREAD_TLS_ALLOC" },
	};
	bool params_only=argc==2 && !strcmp(argv[1],"--params-only");
    bool auto_probe=argc==2 && !strcmp(argv[1],"--p1272-auto");
    unsigned rounds=(params_only || auto_probe)?1:argc>1?(unsigned)strtoul(argv[1],NULL,0):1;
    struct probe_state before={0},after={0};
    uint64_t off_caps[CAP_COUNT];
    if(auto_probe){
        if(!probe_enabled())return 0;
        setvbuf(stdout,NULL,_IOLBF,0);
        printf("p1272-probe: waiting for host request and stable OFF; max 900s\n");
        bool ready=false;
        for(unsigned i=0;i<180;i++){
            if(probe_requested() && probe_no_trials() && probe_state(&before) && before.eligible){ready=true;break;}
            sleep(5);
        }
        if(!ready){printf("p1272-probe: INCONCLUSIVE no eligible requested OFF window\n");return 1;}
        printf("p1272-probe: OFF_BEGIN domain=%u pll=%u mux=%u wake=%u off=%u\n",
            before.domain,before.pll,before.mux,before.wake,before.off);
    }
	io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("HakiosMali"));
	struct create_bo bo = { .size = 16384 };
	mach_vm_address_t base = 0;
	mach_vm_size_t mapped = 0;
	unsigned good = 0;
	kern_return_t kr;

	if (argc > 1 && rounds == 0) {
		fprintf(stderr, "usage: mali_test [ROUNDS | --params-only | --p1272-auto]\n");
		return 2;
	}
	if (service == 0) {
		printf("mali_test: no HakiosMali: the kernel's GPU bring-up has not reached its last step (mali=9)\n");
		return params_only || auto_probe ? 1 : 0;
	}
	if ((kr = IOServiceOpen(service, mach_task_self(), 0, &gpu)) != KERN_SUCCESS) {
		printf("mali_test: 1: HakiosMali would not open: %#x\n", kr);
		return 1;
	}
	IOObjectRelease(service);
	printf("mali_test: 1: HakiosMali open\n");
    if(params_only || auto_probe){
        if(!get_all_caps(off_caps)){IOServiceClose(gpu);return 1;}
        if(params_only){printf("p1272-probe: PARAMS_ONLY PASS count=41\n");IOServiceClose(gpu);return 0;}
        if(!probe_requested() || !probe_no_trials() || !probe_state(&after) || !after.eligible ||
            after.wake!=before.wake || after.off!=before.off ||
            after.wake_fail!=before.wake_fail || after.fault){
            printf("p1272-probe: INCONCLUSIVE OFF changed during GET_PARAM; no job submitted\n");
            IOServiceClose(gpu);return 1;
        }
        printf("p1272-probe: OFF_GET_PARAM PASS count=41 domain=0 pll=0 mux=0 wake=%u off=%u\n",
            after.wake,after.off);
    }

	for (unsigned i = 0; i < sizeof(params) / sizeof(params[0]); i++) {
		struct get_param p = { .param = params[i].param };

		kr = request(kGetParam, &p, sizeof(p));
		printf("mali_test: 2: %-20s %s %#llx\n", params[i].name, kr ? "refused," : "=", kr ? (unsigned long long)kr :
		    (unsigned long long)p.value);
	}

	if(auto_probe && (!probe_requested() || !probe_no_trials() || !probe_state(&after) || !after.eligible ||
       after.wake!=before.wake || after.off!=before.off)){
        printf("p1272-probe: INCONCLUSIVE pre-job state changed\n");IOServiceClose(gpu);return 1;
    }
	if ((kr = request(kCreateBO, &bo, sizeof(bo))) != KERN_SUCCESS || bo.handle == 0) {
		printf("mali_test: 3: CREATE_BO refused: %#x\n", kr);
		return 1;
	}
	struct mmap_bo mm = { .handle = bo.handle };
	kr = request(kMmapBO, &mm, sizeof(mm));
	if (kr == KERN_SUCCESS) {
		kr = IOConnectMapMemory64(gpu, bo.handle, mach_task_self(), &base, &mapped, kIOMapAnywhere);
	}
	printf("mali_test: 3: buffer %u, %u bytes, at %#llx in the GPU's space (%s); mapped here at %#llx (%llu bytes): %#x\n",
	    bo.handle, bo.size, (unsigned long long)bo.offset,
	    bo.offset >= 0x2000000ull && bo.offset + bo.size <= 0x100000000ull ? "between 32 MB and 4 GB, as panfrost expects" :
	    "OUTSIDE 32 MB to 4 GB, where panfrost's blend shaders would break", (unsigned long long)base,
	    (unsigned long long)mapped, kr);
	if (kr != KERN_SUCCESS || mapped < 0x1000) {
		return 1;
	}

	uint64_t started = mach_absolute_time();
	for (unsigned round = 0; round < rounds; round++) {
		volatile uint32_t *job = (volatile uint32_t *)base, *word = (volatile uint32_t *)(base + 0x800);
		uint32_t value = 0xc0ffee01u + round;
		struct submit s = { .jc = bo.offset, .bo_handles = (uint64_t)(uintptr_t)&bo.handle, .bo_handle_count = 1 };
		struct wait_seq w;
		struct wait_bo wb = { .handle = bo.handle, .timeout_ns = 1000000000 };

		memset((void *)base, 0, 0x1000);
		job[4] = (2u << 1) | 1u | (1u << 16);           /* write value; 64-bit pointers; job index 1 */
		job[8] = (uint32_t)(bo.offset + 0x800);
		job[9] = (uint32_t)((bo.offset + 0x800) >> 32);
		job[10] = 6;                                    /* a 32-bit immediate */
		job[12] = value;
		__asm__ volatile ("dsb sy" ::: "memory");       /* out of the write-combining buffer before the request */

		if(auto_probe && (!probe_requested() || !probe_no_trials() || !probe_state(&after) || !after.eligible ||
            after.wake!=before.wake || after.off!=before.off)){
            printf("p1272-probe: INCONCLUSIVE state changed before submit\n");break;
        }
		if ((kr = request(kSubmit, &s, sizeof(s))) != KERN_SUCCESS || s.jc == 0) {
			printf("mali_test: 4: SUBMIT refused: %#x (job number %llu)\n", kr, (unsigned long long)s.jc);
			break;
		}
		w = (struct wait_seq){ .seq = s.jc, .timeout_ns = 1000000000 };
		kr = request(kWaitSeq, &w, sizeof(w));
		kern_return_t kb = request(kWaitBO, &wb, sizeof(wb));
		if (rounds == 1 || *word != value) {
			printf("mali_test: 4: job %llu: wait %#x, WAIT_BO %#x; the job's status word %#x; the word %#x%s\n",
			    (unsigned long long)s.jc, kr, kb, job[0], *word, *word == value ? " -- THE GPU WROTE IT" : " -- not written");
		}
		good += *word == value && kr==KERN_SUCCESS && kb==KERN_SUCCESS;
		if (*word != value || kr!=KERN_SUCCESS || kb!=KERN_SUCCESS) {
			break;
		}
	}
	if (rounds > 1) {
		printf("mali_test: 4: %u of %u jobs written, %.3f ms a job, request to answer\n", good, rounds,
		    good ? ms_since(started) / good : 0.0);
	}

	struct gem_close gc = { .handle = bo.handle };
	(void)IOConnectUnmapMemory64(gpu, bo.handle, mach_task_self(), base);
	kr = request(kDrmGemClose, &gc, sizeof(gc));
	printf("mali_test: 5: buffer given back: %#x\n", kr);
	if(auto_probe){
        uint64_t on_caps[CAP_COUNT];
        bool passed=good==1 && kr==KERN_SUCCESS && probe_state(&after) &&
            after.domain==15 && after.pll==1 && after.mux==1 && after.safe && after.tz &&
            !after.fault && after.wake==before.wake+1 && after.off==before.off &&
            after.wake_fail==before.wake_fail &&
            get_all_caps(on_caps) && !memcmp(off_caps,on_caps,sizeof(off_caps));
        printf("p1272-probe: %s wake=%u->%u domain=%u safe=%u tz=%u caps_equal=%u job_good=%u\n",
            passed?"COMPLETE PASS":"INCONCLUSIVE_OR_FAILED",before.wake,after.wake,after.domain,after.safe,after.tz,passed?1u:0u,good);
        IOServiceClose(gpu);return passed?0:1;
    }
	IOServiceClose(gpu);
	return good == rounds ? 0 : 1;
}
