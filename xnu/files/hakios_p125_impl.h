/* Included exactly once by the iOS 16.5 cpu.c overlay. */
#include <kern/hakios_p125.h>
#include <kern/task.h>
#include <mach/vm_param.h>
uint64_t hakios_p125_counts[P125_CPUS][P125_NCOUNT];
struct p125_row hakios_p125_rows[P125_CPUS][P125_SLOTS];
uint64_t hakios_p125_pages[P125_CPUS][P125_BANKS];
uint64_t hakios_p125_kind_total[P125_CPUS][P125_KINDS];
uint64_t hakios_p125_kind_drop[P125_CPUS][P125_KINDS];
uint64_t hakios_p125_thread_map_drops;
uint64_t hakios_p125_intervals[P125_CPUS][5];
uint64_t hakios_p125_lease, hakios_p125_start, hakios_p125_end;
uint32_t hakios_p125_stop_reason;
uint64_t hakios_p125_usb_down_at,hakios_p125_usb_up_at;
uint64_t hakios_p125_display_events;
unsigned
hakios_p125_active(void)
{
    uint64_t v=__atomic_load_n(&hakios_p125_lease,__ATOMIC_ACQUIRE);
    if(!(v&3u))return 0;
    if(mach_absolute_time()>=(v&~3ULL))return 0;
    if((v&3u)==3u){
        if(!__atomic_load_n(&hakios_p125_usb_down_at,__ATOMIC_ACQUIRE) ||
            __atomic_load_n(&hakios_p125_usb_up_at,__ATOMIC_ACQUIRE))return 0;
        return 2;
    }
    return (unsigned)(v&3u);
}
void hakios_p125_usb_event(unsigned connected)
{
    uint64_t v=__atomic_load_n(&hakios_p125_lease,__ATOMIC_ACQUIRE);
    if((v&3u)!=3u)return;
    uint64_t now=mach_absolute_time();
    if(now>=(v&~3ULL))return;
    uint64_t zero=0;
    if(!connected){
        /* First detected disconnect is the start. Repeated disconnects
         * never reset the capture. */
        if(__atomic_compare_exchange_n(&hakios_p125_usb_down_at,&zero,now,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))
            __atomic_store_n(&hakios_p125_start,now,__ATOMIC_RELEASE);
    }else if(__atomic_load_n(&hakios_p125_usb_down_at,__ATOMIC_ACQUIRE)){
        /* End before serial reconnect traffic; no rearm. */
        (void)__atomic_compare_exchange_n(&hakios_p125_usb_up_at,&zero,now,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
    }
}
void hakios_p125_display_event(unsigned on)
{
    if(on && (__atomic_load_n(&hakios_p125_lease,__ATOMIC_ACQUIRE)&3u))
        __atomic_fetch_add(&hakios_p125_display_events,1,__ATOMIC_RELAXED);
}
unsigned hakios_p125_cpu(void){return (unsigned)getCpuDatap()->cpu_number;}
uint64_t hakios_p125_code(uintptr_t p){return p?(uint64_t)VM_KERNEL_UNSLIDE(p):0;}
void hakios_p125_inc(unsigned cpu,unsigned idx,uint64_t n)
{
    if(cpu<P125_CPUS && idx<P125_NCOUNT)
        __atomic_fetch_add(&hakios_p125_counts[cpu][idx],n,__ATOMIC_RELAXED);
}
unsigned hakios_p125_short(uint64_t ticks)
{
    return ticks<__atomic_load_n(&hakios_p124_wfi_bounds[2],__ATOMIC_RELAXED);
}
void hakios_p125_interval(unsigned cpu,uint64_t ticks)
{
    if(cpu>=P125_CPUS)return;
    unsigned b=0;
    while(b<4 && ticks>=__atomic_load_n(&hakios_p124_wfi_bounds[b],__ATOMIC_RELAXED))b++;
    __atomic_fetch_add(&hakios_p125_intervals[cpu][b],1,__ATOMIC_RELAXED);
    if(b<3)hakios_p125_inc(cpu,P125_SHORT,1);
}
/* Thread creation holds tasks_threads_lock; entries immutable after publish.
 * Always-on bounded metadata, not a periodic probe or a thread reference. */
static struct {uint64_t tid,entry;} p125_thread_map[1024];
void hakios_p125_thread_register(uint64_t tid,uintptr_t entry)
{
    if(!tid || !entry)return;
    unsigned h=(unsigned)(tid*2654435761u)&1023u;
    for(unsigned i=0;i<16;i++){
        unsigned slot=(h+i)&1023u;
        uint64_t id=__atomic_load_n(&p125_thread_map[slot].tid,__ATOMIC_ACQUIRE);
        if(id==tid)return;
        if(!id){
            p125_thread_map[slot].entry=hakios_p125_code(entry);
            __atomic_store_n(&p125_thread_map[slot].tid,tid,__ATOMIC_RELEASE);
            return;
        }
    }
    __atomic_fetch_add(&hakios_p125_thread_map_drops,1,__ATOMIC_RELAXED);
}
static uint64_t p125_thread_entry(uint64_t tid)
{
    unsigned h=(unsigned)(tid*2654435761u)&1023u;
    for(unsigned i=0;i<16;i++){
        unsigned slot=(h+i)&1023u;
        uint64_t id=__atomic_load_n(&p125_thread_map[slot].tid,__ATOMIC_ACQUIRE);
        if(id==tid)return p125_thread_map[slot].entry;
        if(!id)break;
    }
    return 0;
}
static struct p125_row *
p125_find(unsigned cpu,unsigned kind,uint64_t func,uint64_t owner,uint64_t aux)
{
    if(cpu>=P125_CPUS || !kind || kind>=P125_KINDS)return 0;
    __atomic_fetch_add(&hakios_p125_kind_total[cpu][kind],1,__ATOMIC_RELAXED);
    unsigned bank=(kind==P125_K_QUEUE || kind==P125_K_OTHER_QUEUE)?0:
        (kind==P125_K_ENTER || kind==P125_K_CANCEL || kind==P125_K_ASSIGN)?1:
        (kind==P125_K_WAIT_ARM || kind==P125_K_WAIT_EXPIRE || kind==P125_K_WAIT_STALE)?2:3;
    uint64_t h=func^(owner*0x9e3779b97f4a7c15ULL)^(aux*0xbf58476d1ce4e5b9ULL)^kind;
    h^=h>>30;h*=0xbf58476d1ce4e5b9ULL;h^=h>>27;h*=0x94d049bb133111ebULL;h^=h>>31;
    for(unsigned i=0;i<16;i++){
        unsigned slot=bank*P125_BANK_SLOTS+((unsigned)(h+i)&(P125_BANK_SLOTS-1));
        struct p125_row *r=&hakios_p125_rows[cpu][slot];
        uint32_t s=__atomic_load_n(&r->state,__ATOMIC_ACQUIRE);
        if(s==2 && r->kind==kind && r->func==func && r->owner==owner && r->aux==aux)return r;
        if(s==1)break; /* in-flight publication: no spin or duplicate key */
        if(s==0){
            uint32_t expect=0;
            if(!__atomic_compare_exchange_n(&r->state,&expect,1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))break;
            r->kind=kind;r->func=func;r->owner=owner;r->aux=aux;
            __atomic_store_n(&r->state,2,__ATOMIC_RELEASE);
            __atomic_fetch_or(&hakios_p125_pages[cpu][bank],1ULL<<((slot%P125_BANK_SLOTS)/P125_PAGE_ROWS),__ATOMIC_RELEASE);
            return r;
        }
    }
    __atomic_fetch_add(&hakios_p125_kind_drop[cpu][kind],1,__ATOMIC_RELAXED);
    hakios_p125_inc(cpu,P125_DROP,1);
    return 0;
}
void
hakios_p125_record(unsigned cpu,unsigned kind,uint64_t func,uint64_t owner,
    uint64_t aux,uint64_t delta,uint64_t requested_leeway,uint64_t effective_leeway,uint32_t flags)
{
    struct p125_row *r=p125_find(cpu,kind,func,owner,aux);
    if(!r)return;
    __atomic_fetch_add(&r->count,1,__ATOMIC_RELAXED);
    /* UINT64_MAX is a sentinel for non-registration/unknown slack. */
    if(delta!=UINT64_MAX){
        uint64_t b=__atomic_load_n(&hakios_p124_wfi_bounds[2],__ATOMIC_RELAXED);
        if(delta<b)__atomic_fetch_add(&r->short_n,1,__ATOMIC_RELAXED);
        if(!requested_leeway)__atomic_fetch_add(&r->reqzero,1,__ATOMIC_RELAXED);
        if(!effective_leeway)__atomic_fetch_add(&r->effzero,1,__ATOMIC_RELAXED);
    }
    __atomic_fetch_or(&r->flags_or,(uint64_t)flags,__ATOMIC_RELAXED);
}
uint32_t
hakios_p125_sample(unsigned cpu,unsigned kind,uint64_t func,uint64_t owner,uint64_t aux)
{
    struct p125_row *r=p125_find(cpu,kind,func,owner,aux);
    if(!r)return 0;
    uint64_t n=__atomic_fetch_add(&r->count,1,__ATOMIC_RELAXED);
    if((n&31u)!=0)return 0;
    unsigned slot=(unsigned)(r-&hakios_p125_rows[cpu][0]);
    return 1+cpu*P125_SLOTS+slot;
}
void hakios_p125_wall(uint32_t token,uint64_t elapsed)
{
    if(!token || token>P125_CPUS*P125_SLOTS)return;
    token--;
    struct p125_row *r=&hakios_p125_rows[token/P125_SLOTS][token%P125_SLOTS];
    __atomic_fetch_add(&r->wall_ticks,elapsed,__ATOMIC_RELAXED);
    __atomic_fetch_add(&r->sample_n,1,__ATOMIC_RELAXED);
}
uint64_t hakios_p125_task_id(void *p){return p?((task_t)p)->task_uniqueid:0;}
/* Valid thread reference under existing wait lock, or current_thread().
 * PID from audit token, no proc lookup, no new locks in IRQ. */
void hakios_p125_owner(void *p,uint64_t *owner,uint64_t *aux,uint64_t *site)
{
    thread_t th=(thread_t)p;
    *owner=0;*aux=0;*site=0;
    if(!th)return;
    task_t task=get_threadtask(th);
    if(task==kernel_task){
        *owner=th->thread_id;
        *site=p125_thread_entry(th->thread_id);
        if(!*site)*site=hakios_p125_code((uintptr_t)th->continuation);
    }else if(task){
        *owner=task->task_uniqueid;
        *aux=(uint64_t)(uint32_t)task_pid(task);
    }
}
