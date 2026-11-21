/* S9 P5 Display/Wake/GPU Lab: included after hakios.mali_lab.h.
 *
 * Read-only diagnostic. The owner is a HakiosMaliUserClient number, NOT
 * a PID or a CoreAnimation call stack. No GPU-domain MMIO, voltage,
 * clock, thermal, fence or display write is performed by this file.
 * Snapshot uses the EXISTING 5s G3D Lab kernel thread. The per-owner
 * counters are updated while the GPU queue mutex is already held.
 */
#ifndef HAKIOS_P5_LAB_H
#define HAKIOS_P5_LAB_H
#define P5_MAX_CLIENTS 32
#define P5_OWNER_PRINT 12
struct p5_client_slot {
    uint32_t owner, tracked, queued, done, fragment, vertex, failed, dropped;
    uint64_t queued_ticks, run_ticks, max_queued_ticks, max_run_ticks;
};
static struct p5_client_slot p5_clients[P5_MAX_CLIENTS];
static uint32_t p5_overflow, p5_last_overflow, p5_last_jobs;
static uint32_t p5_last_frag, p5_last_verts, p5_last_dropped, p5_last_errors;
static uint32_t p5_last_peak_1s, p5_last_owners, p5_last_queue_max;
static uint64_t p5_last_wait_ns, p5_last_run_ns, p5_last_at_ns;
static uint32_t p5_queue_peak, p5_sec_jobs, p5_sec_peak;
static uint64_t p5_sec_started;

/* Existing mali_queue_lock must already be held. */
static struct p5_client_slot *
p5_slot_locked(uint32_t owner)
{
    for (unsigned i = 0; i < P5_MAX_CLIENTS; i++)
        if (p5_clients[i].owner == owner && p5_clients[i].tracked)
            return &p5_clients[i];
    for (unsigned i = 0; i < P5_MAX_CLIENTS; i++)
        if (!p5_clients[i].tracked) {
            p5_clients[i].owner = owner;
            p5_clients[i].tracked = 1;
            return &p5_clients[i];
        }
    p5_overflow++; /* overflow is explicit rather than misattributed */
    return NULL;
}

static void
p5_queue_added_locked(uint32_t owner, uint32_t queue_depth)
{
    struct p5_client_slot *s;
    if (!__atomic_load_n(&p5_mode, __ATOMIC_RELAXED))
        return;
    s = p5_slot_locked(owner);
    if (s) s->queued++;
    if (queue_depth > p5_queue_peak) p5_queue_peak = queue_depth;
}

static void
p5_queue_completed_locked(uint32_t owner, bool fragment, bool dropped,
    uint32_t status, uint64_t waiting, uint64_t running, uint64_t now)
{
    struct p5_client_slot *s;
    uint64_t span;
    if (!__atomic_load_n(&p5_mode, __ATOMIC_RELAXED))
        return;
    s = p5_slot_locked(owner);
    if (s) {
        s->done++;
        s->fragment += fragment ? 1u : 0u;
        s->vertex += fragment ? 0u : 1u;
        s->dropped += dropped ? 1u : 0u;
        s->failed += status ? 1u : 0u;
        s->queued_ticks += waiting;
        s->run_ticks += running;
        if (waiting > s->max_queued_ticks) s->max_queued_ticks = waiting;
        if (running > s->max_run_ticks) s->max_run_ticks = running;
    }
    if (!p5_sec_started) p5_sec_started=now;
    nanoseconds_to_absolutetime(NSEC_PER_SEC, &span);
    if (now >= p5_sec_started && now - p5_sec_started >= span) {
        if (p5_sec_jobs > p5_sec_peak) p5_sec_peak = p5_sec_jobs;
        p5_sec_started=now;
        p5_sec_jobs=0;
    }
    p5_sec_jobs++;
}

/* Queued job timings are NOT GPU execution time: a worker may sleep,
 * wait for IRQ, perform power validation, and spend CPU time on submit. */
static void
p5_monitor_snapshot(uint64_t now, uint64_t up_ns, uint32_t status,
    uint32_t pll, uint32_t mux, bool awake)
{
    struct p5_client_slot bins[P5_MAX_CLIENTS], temp;
    uint32_t mode=__atomic_load_n(&p5_mode,__ATOMIC_ACQUIRE);
    uint32_t n=0,submitted=0,completed=0,fragment=0,vertex=0,errors=0,dropped=0;
    uint32_t peak,queue_peak,overflows,phase;
    uint64_t wait_ticks=0,run_ticks=0,wait_ns=0,run_ns=0;
    uint64_t frame_ns=0,job_ns=0;
    if (!mode) return;
    phase=__atomic_load_n(&p5_phase,__ATOMIC_RELAXED);
    /* Take a bounded snapshot under the same queue mutex as submissions.
     * Do NOT hold queue lock across any serial output or PMU work. */
    lck_mtx_lock(&mali_queue_lock);
    memcpy(bins,p5_clients,sizeof(bins));
    bzero(p5_clients,sizeof(p5_clients));
    peak=p5_sec_peak>p5_sec_jobs?p5_sec_peak:p5_sec_jobs;
    queue_peak=p5_queue_peak;
    overflows=p5_overflow;
    p5_sec_peak=p5_sec_jobs=p5_queue_peak=p5_overflow=0;
    p5_sec_started=now;
    lck_mtx_unlock(&mali_queue_lock);
    for(unsigned i=0;i<P5_MAX_CLIENTS;i++){
        if(!bins[i].tracked)continue;
        n++;
        submitted+=bins[i].queued;
        completed+=bins[i].done;
        fragment+=bins[i].fragment;
        vertex+=bins[i].vertex;
        dropped+=bins[i].dropped;
        errors+=bins[i].failed;
        wait_ticks+=bins[i].queued_ticks;
        run_ticks+=bins[i].run_ticks;
    }
    absolutetime_to_nanoseconds(wait_ticks,&wait_ns);
    absolutetime_to_nanoseconds(run_ticks,&run_ns);
    p5_last_at_ns=up_ns;
    p5_last_jobs=completed;p5_last_frag=fragment;p5_last_verts=vertex;
    p5_last_dropped=dropped;p5_last_errors=errors;p5_last_peak_1s=peak;
    p5_last_overflow=overflows;
    p5_last_owners=n;p5_last_queue_max=queue_peak;
    p5_last_wait_ns=wait_ns;p5_last_run_ns=run_ns;
    uint64_t frame_last=__atomic_load_n(&gov_frame_last,__ATOMIC_RELAXED);
    uint64_t job_last=__atomic_load_n(&g3d_last_busy,__ATOMIC_RELAXED);
    if(frame_last && now >= frame_last)
        absolutetime_to_nanoseconds(now-frame_last,&frame_ns);
    if(job_last && now >= job_last)
        absolutetime_to_nanoseconds(now-job_last,&job_ns);
    say("p5-gpu: up=%llums phase=%u mode=%u domain=%u pmu_status=%#x awake=%u pll_en=%u pll_stable=%u mux=%u clock_hint=%uMHz frame_age_ms=%llu job_age_ms=%llu submitted=%u completed=%u fragment=%u vertex=%u errors=%u dropped=%u owners=%u overflow=%u queue_peak=%u peak_jobs_1s=%u worker_wait_us_avg=%llu worker_run_us_avg=%llu deep=unmeasured voltage=unmeasured\n",
        up_ns/NSEC_PER_MSEC,phase,mode,((status&G3D_ON)==G3D_ON)?1u:0u,
        status,awake?1u:0u,(pll&PLL_ENABLE)?1u:0u,(pll&PLL_STABLE)?1u:0u,
        (mux&MUX_SEL)?1u:0u,((status&G3D_ON)==G3D_ON&&gov_level<GOV_LEVELS)?
        g3d_rates[gov_level].mhz:0u,frame_ns/NSEC_PER_MSEC,job_ns/NSEC_PER_MSEC,
        submitted,completed,fragment,vertex,errors,dropped,n,overflows,queue_peak,
        peak,completed?wait_ns/1000/completed:0,completed?run_ns/1000/completed:0);
    /* Sort by completed job count, making per-client visibility useful
     * during the 2,000-job display wake burst without per-job UART writes. */
    for(unsigned i=0;i<P5_MAX_CLIENTS;i++)
        for(unsigned j=i+1;j<P5_MAX_CLIENTS;j++)
            if(bins[j].done>bins[i].done){
                temp=bins[i];bins[i]=bins[j];bins[j]=temp;
            }
    for(unsigned i=0;i<P5_OWNER_PRINT && i<P5_MAX_CLIENTS && bins[i].tracked;i++){
        uint64_t qns=0,rns=0,maxq=0,maxr=0;
        absolutetime_to_nanoseconds(bins[i].queued_ticks,&qns);
        absolutetime_to_nanoseconds(bins[i].run_ticks,&rns);
        absolutetime_to_nanoseconds(bins[i].max_queued_ticks,&maxq);
        absolutetime_to_nanoseconds(bins[i].max_run_ticks,&maxr);
        say("p5-owner: up=%llums phase=%u owner=%u submitted=%u jobs=%u fragment=%u vertex=%u dropped=%u failed=%u wait_us_avg=%llu wait_us_max=%llu run_us_avg=%llu run_us_max=%llu identity=client-not-pid\n",
            up_ns/NSEC_PER_MSEC,phase,bins[i].owner,bins[i].queued,bins[i].done,
            bins[i].fragment,bins[i].vertex,bins[i].dropped,bins[i].failed,
            bins[i].done?qns/1000/bins[i].done:0,maxq/1000,
            bins[i].done?rns/1000/bins[i].done:0,maxr/1000);
    }
}

static int
p5_mode_sysctl(__unused struct sysctl_oid *oidp,__unused void *arg1,
    __unused int arg2,struct sysctl_req *req)
{
    int value=(int)__atomic_load_n(&p5_mode,__ATOMIC_RELAXED);
    int err=sysctl_io_number(req,value,sizeof(value),&value,NULL);
    if(err || req->newptr == USER_ADDR_NULL)return err;
    if(value<0 || value>2)return EINVAL;
    if(value && reached<STEPS)return ENODEV;
    __atomic_store_n(&p5_mode,(uint32_t)value,__ATOMIC_RELEASE);
    if(value)g3d_lab_monitor_start(); /* EXISTING independent OFF sampler */
    say("p5: mode=%d: 0=off 1=read-only 2=read-only+bounded-burst; no power/GL policy writes\n",value);
    return 0;
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p5_mode,
    CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,
    p5_mode_sysctl,"I","P5 read-only CPU/display/GPU observer, 0 off 1 passive 2 burst");

static int
p5_phase_sysctl(__unused struct sysctl_oid *oidp,__unused void *arg1,
    __unused int arg2,struct sysctl_req *req)
{
    int value=(int)__atomic_load_n(&p5_phase,__ATOMIC_RELAXED);
    int err=sysctl_io_number(req,value,sizeof(value),&value,NULL);
    uint64_t ns=0;
    if(err || req->newptr == USER_ADDR_NULL)return err;
    if(value<0 || value>99)return EINVAL;
    __atomic_store_n(&p5_phase,(uint32_t)value,__ATOMIC_RELEASE);
    absolutetime_to_nanoseconds(mach_absolute_time(),&ns);
    say("p5: PHASE=%u up=%llums marker-only\n",value,ns/NSEC_PER_MSEC);
    return 0;
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p5_phase,
    CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,
    p5_phase_sysctl,"I","P5 phase marker 0..99");

static int
p5_threads_sysctl(__unused struct sysctl_oid *oidp,__unused void *arg1,
    __unused int arg2,struct sysctl_req *req)
{
    int value=(int)__atomic_load_n(&p5_threads,__ATOMIC_RELAXED);
    int err=sysctl_io_number(req,value,sizeof(value),&value,NULL);
    if(err || req->newptr==USER_ADDR_NULL)return err;
    if(value<0 || value>2)return EINVAL;
    __atomic_store_n(&p5_threads,(uint32_t)value,__ATOMIC_RELEASE);
    say("p5: per-thread sampler=%u (0 off 1 backboardd 2 +SpringBoard/PosterBoard; existing poster_cost)\n",value);
    return 0;
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p5_threads,
    CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,
    p5_threads_sysctl,"I","P5 libproc CPU thread details via poster_cost when poster_probe=1");

static int
p5_stats_sysctl(__unused struct sysctl_oid *oidp,__unused void *arg1,
    __unused int arg2,struct sysctl_req *req)
{
    char line[290];
    snprintf(line,sizeof(line),
        "phase=%u mode=%u threads=%u sample_up_ms=%llu jobs=%u owners=%u frag=%u vertex=%u dropped=%u errors=%u overflow=%u peak_1s=%u queue_peak=%u queue_wait_ns=%llu worker_run_ns=%llu deep=unmeasured power=read-only",
        __atomic_load_n(&p5_phase,__ATOMIC_RELAXED),
        __atomic_load_n(&p5_mode,__ATOMIC_RELAXED),
        __atomic_load_n(&p5_threads,__ATOMIC_RELAXED),
        p5_last_at_ns/NSEC_PER_MSEC,p5_last_jobs,p5_last_owners,p5_last_frag,
        p5_last_verts,p5_last_dropped,p5_last_errors,p5_last_overflow,p5_last_peak_1s,
        p5_last_queue_max,p5_last_wait_ns,p5_last_run_ns);
    return SYSCTL_OUT(req,line,strlen(line)+1);
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p5_stats,
    CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,
    p5_stats_sysctl,"A","P5 last read-only GPU owner/PMU snapshot");
#endif
