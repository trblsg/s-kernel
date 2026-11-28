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
uint32_t hakios_p12753_busy,hakios_p12753_observe;
uint32_t hakios_p12754_hw_observe,hakios_p12754_mfc_first=UINT32_MAX,hakios_p12754_mfc_state,hakios_p12754_cam_first=UINT32_MAX,hakios_p12754_cam_state;
uint32_t hakios_p12754_audio[11];
uint64_t hakios_p12754_audio_at;
uint64_t hakios_p12753_fg_link,hakios_p12753_usb_down,hakios_p12753_usb_up,hakios_p12753_display_on;
uint64_t hakios_p125_usb_down_at,hakios_p125_usb_up_at;
uint64_t hakios_p1273_offline_duration,hakios_p1273_offline_deadline;
uint32_t hakios_p1273_offline_fixed;
uint64_t hakios_p125_display_events;
uint64_t hakios_p126_lease;
uint32_t hakios_p126_reason;
uint64_t hakios_p126_hits[6];
uint64_t hakios_p126_diag[9];
uint32_t hakios_p126_audio_state;
uint64_t hakios_p127_lease;
uint32_t hakios_p127_reason,hakios_p127_offline_aggregate;
uint64_t hakios_p127_wfi_n[8][9],hakios_p127_wfi_ticks[8][9],hakios_p127_bounds[8];
uint64_t hakios_p127_usb[5],hakios_p127_abox[4],hakios_p127_psr[4],hakios_p127_floor[2];
uint64_t hakios_p127_irq[8][3],hakios_p127_wifi[5],hakios_p127_bt[6],hakios_p127_ticker[3];
uint32_t hakios_p127_audio_event;
struct p127_fg_row hakios_p127_fg_rows[P127_FG_SLOTS];
uint64_t hakios_p127_fg_seq;
uint64_t hakios_p1274_deadline,hakios_p1274_fg_seq;
uint64_t hakios_p1274_usb_down,hakios_p1274_usb_up,hakios_p1274_display_on_at;
uint64_t hakios_p1274_wfi_n[8],hakios_p1274_wfi_ticks[8];
uint32_t hakios_p1274_trace_late,hakios_p1274_writers,hakios_p12751_trace_replace;
struct p127_fg_row hakios_p1274_fg_rows[P1274_FG_SLOTS];
static bool p1274_live(uint64_t now) {
    uint64_t end=__atomic_load_n(&hakios_p1274_deadline,__ATOMIC_SEQ_CST);
    return end && now<end;
}
static bool p1274_writer_enter(uint64_t now) {
    __atomic_fetch_add(&hakios_p1274_writers,1,__ATOMIC_SEQ_CST);
    if(p1274_live(now))return true;
    __atomic_fetch_sub(&hakios_p1274_writers,1,__ATOMIC_SEQ_CST);return false;
}
static void p1274_writer_leave(void){__atomic_fetch_sub(&hakios_p1274_writers,1,__ATOMIC_SEQ_CST);}
static void p1274_fg_record(int32_t ua,int32_t avg_ua,uint32_t uv,unsigned external,unsigned charger,unsigned qh)
{
    uint64_t now=mach_absolute_time();
    if(!p1274_live(now) || !p1274_writer_enter(now))return;
    uint64_t seq=__atomic_load_n(&hakios_p1274_fg_seq,__ATOMIC_RELAXED)+1;
    if(seq>P1274_FG_SLOTS){p1274_writer_leave();return;} /* never overwrite, explicit coverage */
    struct p127_fg_row *r=&hakios_p1274_fg_rows[seq-1];
    r->ticks=now;r->ua=ua;r->avg_ua=avg_ua;r->uv=uv;
    r->external=external;r->charger=charger;r->qh=qh;
    __atomic_store_n(&r->seq,seq,__ATOMIC_RELEASE);
    __atomic_store_n(&hakios_p1274_fg_seq,seq,__ATOMIC_RELEASE);
    p1274_writer_leave();
}
/* The recount hook executes with interrupts disabled: one writer per CPU.
 * Freeze never waits on a writer. The reader claims frozen state and checks
 * inflight before touching plain ring storage. No pointer is retained. */
uint32_t hakios_p1273_trace_state,hakios_p1273_trace_reason;
uint64_t hakios_p1273_trace_generation,hakios_p1273_trace_start,hakios_p1273_trace_until;
uint64_t hakios_p1273_trace_trigger,hakios_p1273_trace_end,hakios_p1273_trace_span;
uint64_t hakios_p1273_trace_overhead[8],hakios_p1273_trace_clipped[8];
struct p1273_trace_bucket hakios_p1273_trace_rows[8][P1273_TRACE_BUCKETS];
static uint32_t p1273_trace_inflight[8];
static bool p1273_trace_quiet(void)
{
    for(unsigned i=0;i<8;i++)if(__atomic_load_n(&p1273_trace_inflight[i],__ATOMIC_SEQ_CST))return false;
    return true;
}
bool hakios_p1273_trace_arm(uint64_t start,uint64_t until,uint64_t bucket_span)
{
    if(!bucket_span || until<=start)return false;
    uint32_t old=__atomic_load_n(&hakios_p1273_trace_state,__ATOMIC_SEQ_CST);
    if(old!=0)return false; /* preserve one frozen capture per boot */
    if(!__atomic_compare_exchange_n(&hakios_p1273_trace_state,&old,1,false,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST))return false;
    if(!p1273_trace_quiet()){
        __atomic_store_n(&hakios_p1273_trace_state,old,__ATOMIC_SEQ_CST);return false;
    }
    __atomic_fetch_add(&hakios_p1273_trace_generation,1,__ATOMIC_RELAXED);
    __atomic_store_n(&hakios_p1273_trace_start,start,__ATOMIC_RELAXED);
    __atomic_store_n(&hakios_p1273_trace_until,until,__ATOMIC_RELAXED);
    __atomic_store_n(&hakios_p1273_trace_span,bucket_span,__ATOMIC_RELAXED);
    __atomic_store_n(&hakios_p1273_trace_trigger,0,__ATOMIC_RELAXED);
    __atomic_store_n(&hakios_p1273_trace_end,0,__ATOMIC_RELAXED);
    __atomic_store_n(&hakios_p1273_trace_reason,0,__ATOMIC_RELAXED);
    for(unsigned i=0;i<8;i++){
        __atomic_store_n(&hakios_p1273_trace_overhead[i],0,__ATOMIC_RELAXED);
        __atomic_store_n(&hakios_p1273_trace_clipped[i],0,__ATOMIC_RELAXED);
    }
    __atomic_store_n(&hakios_p1273_trace_state,2,__ATOMIC_SEQ_CST);
    return true;
}
void hakios_p1273_trace_stop(unsigned reason,uint64_t now)
{
    uint32_t old=2;
    if(!__atomic_compare_exchange_n(&hakios_p1273_trace_state,&old,3,false,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST))return;
    uint64_t until=__atomic_load_n(&hakios_p1273_trace_until,__ATOMIC_RELAXED);
    __atomic_store_n(&hakios_p1273_trace_end,now<until?now:until,__ATOMIC_RELAXED);
    __atomic_store_n(&hakios_p1273_trace_reason,reason,__ATOMIC_RELAXED);
    __atomic_store_n(&hakios_p1273_trace_state,4,__ATOMIC_SEQ_CST);
}
bool hakios_p1273_trace_read_begin(void)
{
    uint32_t old=4;
    if(!__atomic_compare_exchange_n(&hakios_p1273_trace_state,&old,5,false,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST))return false;
    if(!p1273_trace_quiet()){
        __atomic_store_n(&hakios_p1273_trace_state,4,__ATOMIC_SEQ_CST);return false;
    }
    return true;
}
void hakios_p1273_trace_read_end(void)
{
    __atomic_store_n(&hakios_p1273_trace_state,4,__ATOMIC_SEQ_CST);
}
void hakios_p1273_identity(uint64_t *tid,uint64_t *task_id,uint32_t *pid)
{
    thread_t th=current_thread();
    task_t task=get_threadtask(th);
    *tid=th->thread_id;
    *task_id=task?task->task_uniqueid:0;
    *pid=task && task!=kernel_task?(uint32_t)task_pid(task):0;
}
void hakios_p1273_recount(void *p,void *t,uint64_t end,uint64_t ticks,bool from_user,bool switched)
{
    if(!p || !t || !ticks || ticks>end)return;
    unsigned cpu=hakios_p125_cpu();
    if(cpu>=8)return;
    uint64_t gen=__atomic_load_n(&hakios_p1273_trace_generation,__ATOMIC_ACQUIRE);
    __atomic_fetch_add(&p1273_trace_inflight[cpu],1,__ATOMIC_SEQ_CST);
    if(__atomic_load_n(&hakios_p1273_trace_state,__ATOMIC_SEQ_CST)!=2 ||
       gen!=__atomic_load_n(&hakios_p1273_trace_generation,__ATOMIC_ACQUIRE))goto done;
    {
        uint64_t entered=mach_absolute_time();
        uint64_t start=__atomic_load_n(&hakios_p1273_trace_start,__ATOMIC_RELAXED);
        uint64_t until=__atomic_load_n(&hakios_p1273_trace_until,__ATOMIC_RELAXED);
        uint64_t span=__atomic_load_n(&hakios_p1273_trace_span,__ATOMIC_RELAXED);
        uint64_t begin=end-ticks,original_end=end;
        if(!span || end<=start || begin>=until)goto done;
        if(begin<start)begin=start;
        if(end>until)end=until;
        /* Clamp exceptionally long samples to the ring's current horizon.
         * Count omitted time explicitly; never extend an idle interval. */
        uint64_t last=(end-1-start)/span;
        uint64_t first=last>=P1273_TRACE_BUCKETS-1?last-(P1273_TRACE_BUCKETS-1):0;
        uint64_t low=start+first*span;
        if(begin<low){
            __atomic_fetch_add(&hakios_p1273_trace_clipped[cpu],low-begin,__ATOMIC_RELAXED);
            begin=low;
        }
        thread_t thread=(thread_t)p;
        task_t task=(task_t)t;
        uint64_t tid=thread->thread_id,task_id=task->task_uniqueid;
        uint32_t kind=(thread->options&TH_OPT_IDLE_THREAD)?0u:task==kernel_task?1u:2u;
        uint32_t pid=kind==2?(uint32_t)task_pid(task):0;
        while(begin<end){
            uint64_t bin=(begin-start)/span,edge=start+(bin+1)*span;
            uint64_t part=edge<end?edge:end,delta=part-begin;
            struct p1273_trace_bucket *b=&hakios_p1273_trace_rows[cpu][bin%P1273_TRACE_BUCKETS];
            if(b->generation!=gen || b->bin!=bin){
                bzero(b,sizeof(*b));b->generation=gen;b->bin=bin;
            }
            unsigned pos=P1273_TRACE_OWNERS;
            for(unsigned i=0;i<P1273_TRACE_OWNERS;i++){
                if(!(b->mask&(1u<<i))){
                    b->mask|=1u<<i;
                    b->owner[i].tid=tid;b->owner[i].task=task_id;
                    b->owner[i].pid=pid;b->owner[i].kind=kind;pos=i;break;
                }
                if(b->owner[i].tid==tid && b->owner[i].task==task_id &&
                   b->owner[i].kind==kind){pos=i;break;}
            }
            if(pos==P1273_TRACE_OWNERS && __atomic_load_n(&hakios_p12751_trace_replace,__ATOMIC_RELAXED)){
                /* Keep the heavy owners instead of permanently retaining
                 * the first 16 transient threads. Evicted time is explicitly
                 * unknown; never credit it to the replacement. Preserve idle.
                 * Bounded 16-entry scan only on overflow. */
                unsigned least=P1273_TRACE_OWNERS;uint64_t ticks=UINT64_MAX;
                for(unsigned i=0;i<P1273_TRACE_OWNERS;i++){
                    struct p1273_trace_owner *o=&b->owner[i];
                    uint64_t total=o->user_ticks+o->system_ticks;
                    if(o->kind!=0 && total<ticks){ticks=total;least=i;}
                }
                if(least<P1273_TRACE_OWNERS && (ticks<=span/32 || delta>=ticks)){
                    struct p1273_trace_owner *o=&b->owner[least];
                    b->dropped+=o->fragments;b->dropped_ticks+=ticks;
                    bzero(o,sizeof(*o));o->tid=tid;o->task=task_id;o->pid=pid;o->kind=kind;pos=least;
                }
            }
            if(pos==P1273_TRACE_OWNERS){b->dropped++;b->dropped_ticks+=delta;}
            else{
                struct p1273_trace_owner *o=&b->owner[pos];
                if(from_user)o->user_ticks+=delta;else o->system_ticks+=delta;
                o->fragments++;
                if(switched && part==original_end)o->switches++;
            }
            begin=part;
        }
        __atomic_fetch_add(&hakios_p1273_trace_overhead[cpu],mach_absolute_time()-entered,__ATOMIC_RELAXED);
    }
done:
    __atomic_fetch_sub(&p1273_trace_inflight[cpu],1,__ATOMIC_SEQ_CST);
}
void hakios_p127_fg_record(int32_t ua,int32_t avg_ua,uint32_t uv,unsigned external,unsigned charger,unsigned qh)
{
    p1274_fg_record(ua,avg_ua,uv,external,charger,qh);
    /* One atomic word: fresh gauge timestamp + valid/external bits. */
    __atomic_store_n(&hakios_p12753_fg_link,(mach_absolute_time()&~3ULL)|2u|(external?1u:0u),__ATOMIC_RELEASE);
    if((!hakios_p127_mode() && !__atomic_load_n(&hakios_p12753_observe,__ATOMIC_ACQUIRE)) || !hakios_p125_active())return;
    uint64_t seq=__atomic_load_n(&hakios_p127_fg_seq,__ATOMIC_RELAXED)+1;
    struct p127_fg_row *r=&hakios_p127_fg_rows[(seq-1)%P127_FG_SLOTS];
    __atomic_store_n(&r->seq,0,__ATOMIC_RELEASE);
    r->ticks=mach_absolute_time();r->ua=ua;r->avg_ua=avg_ua;r->uv=uv;
    r->external=external;r->charger=charger;r->qh=qh;
    __atomic_store_n(&r->seq,seq,__ATOMIC_RELEASE);
    __atomic_store_n(&hakios_p127_fg_seq,seq,__ATOMIC_RELEASE);
}
void hakios_p127_audio_wake(void){
    /* Raw ownership also covers a lease that has just expired or lost USB. */
    if(__atomic_load_n(&hakios_p127_lease,__ATOMIC_ACQUIRE)&P127_ABOX){
        __atomic_fetch_add(&hakios_p127_audio_event,1,__ATOMIC_RELEASE);
        thread_wakeup((event_t)&hakios_p127_audio_event);
    }
}
unsigned hakios_p127_mode(void)
{
    uint64_t v=__atomic_load_n(&hakios_p127_lease,__ATOMIC_ACQUIRE);
    if(!v)return 0;
    uint64_t now=mach_absolute_time();
    if(__atomic_load_n(&hakios_p1273_offline_fixed,__ATOMIC_ACQUIRE)){
        uint64_t deadline=__atomic_load_n(&hakios_p1273_offline_deadline,__ATOMIC_ACQUIRE);
        if((deadline && now>=deadline) || __atomic_load_n(&hakios_p125_usb_up_at,__ATOMIC_ACQUIRE))return 0;
    }
    return now<(v&~(uint64_t)P127_MASK)?(unsigned)(v&P127_MASK):0;
}
uint32_t hakios_p1273_gate_watch,hakios_p1273_last_stop_reason;
uint64_t hakios_p1273_events[2][P1273_EVENT_COUNT],hakios_p1273_last_stop_lease,hakios_p1273_fault_seq,hakios_p127543_stop_at;
void hakios_p1273_event(unsigned event,uint64_t count)
{
    if(event==P1273_BT_ERROR || event==P1273_TICK_FAIL)
        __atomic_fetch_add(&hakios_p1273_fault_seq,count,__ATOMIC_RELAXED);
    unsigned watch=__atomic_load_n(&hakios_p1273_gate_watch,__ATOMIC_ACQUIRE);
    if(!watch || event>=P1273_EVENT_COUNT || !count)return;
    unsigned active=hakios_p127_mode()==watch?0:1;
    __atomic_fetch_add(&hakios_p1273_events[active][event],count,__ATOMIC_RELAXED);
}
static void p127_stop_publish(uint64_t v,unsigned reason)
{
    if(v){
        __atomic_store_n(&hakios_p127_reason,reason,__ATOMIC_RELEASE);
        __atomic_store_n(&hakios_p1273_last_stop_reason,reason,__ATOMIC_RELAXED);
        __atomic_store_n(&hakios_p127543_stop_at,mach_absolute_time(),__ATOMIC_RELAXED);
        __atomic_store_n(&hakios_p1273_last_stop_lease,v,__ATOMIC_RELEASE);
        /* Policy removal must wake even after mode becomes zero. */
        if(v&P127_ABOX){
            __atomic_fetch_add(&hakios_p127_audio_event,1,__ATOMIC_RELEASE);
            thread_wakeup((event_t)&hakios_p127_audio_event);
        }
    }
}
void hakios_p127_stop(unsigned reason)
{
    uint64_t v=__atomic_exchange_n(&hakios_p127_lease,0,__ATOMIC_ACQ_REL);
    p127_stop_publish(v,reason);
}
bool hakios_p127_stop_owned(uint64_t lease,unsigned reason)
{
    if(!lease)return false;
    uint64_t expected=lease;
    if(!__atomic_compare_exchange_n(&hakios_p127_lease,&expected,0,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return false;
    p127_stop_publish(lease,reason);return true;
}
void hakios_p127_wfi(unsigned cpu,uint64_t ticks)
{
    if(cpu>=8 || (!hakios_p127_mode() && !__atomic_load_n(&hakios_p12753_observe,__ATOMIC_ACQUIRE)) || !hakios_p125_active())return;
    unsigned b=0;
    while(b<8 && ticks>=__atomic_load_n(&hakios_p127_bounds[b],__ATOMIC_RELAXED))b++;
    __atomic_fetch_add(&hakios_p127_wfi_n[cpu][b],1,__ATOMIC_RELAXED);
    __atomic_fetch_add(&hakios_p127_wfi_ticks[cpu][b],ticks,__ATOMIC_RELAXED);
}
void hakios_p127_metric(unsigned group,unsigned index,uint64_t value)
{
    uint64_t *v=0;
    if(group==0 && index<5)v=&hakios_p127_usb[index];
    if(group==1 && index<4)v=&hakios_p127_abox[index];
    if(group==2 && index<4)v=&hakios_p127_psr[index];
    if(group==3 && index<2)v=&hakios_p127_floor[index];
    if(group==4 && index<5)v=&hakios_p127_wifi[index];
    if(group==5 && index<6)v=&hakios_p127_bt[index];
    if(group==6 && index<3)v=&hakios_p127_ticker[index];
    if(v)__atomic_fetch_add(v,value,__ATOMIC_RELAXED);
}
unsigned hakios_p126_active(unsigned bit)
{
    uint64_t lease=__atomic_load_n(&hakios_p126_lease,__ATOMIC_ACQUIRE);
    if((hakios_p127_mode()&P127_BASE) && (bit&15u))return bit&15u;
    if(!(lease&bit))return 0;
    return mach_absolute_time()<(lease&~(uint64_t)P126_MASK) ? (unsigned)(lease&bit):0;
}
void hakios_p126_hit(unsigned index)
{
    if(index<6)__atomic_fetch_add(&hakios_p126_hits[index],1,__ATOMIC_RELAXED);
}
void hakios_p126_diag_add(unsigned index,uint64_t count)
{
    if(index<9 && (__atomic_load_n(&hakios_p125_lease,__ATOMIC_RELAXED) || __atomic_load_n(&hakios_p126_lease,__ATOMIC_RELAXED)))
        __atomic_fetch_add(&hakios_p126_diag[index],count,__ATOMIC_RELAXED);
}
void hakios_p126_audio_set(unsigned state){__atomic_store_n(&hakios_p126_audio_state,state,__ATOMIC_RELEASE);hakios_p127_audio_wake();}
unsigned hakios_p126_audio_get(void){return __atomic_load_n(&hakios_p126_audio_state,__ATOMIC_ACQUIRE);}
void hakios_p126_stop(unsigned reason)
{
    uint64_t lease=__atomic_exchange_n(&hakios_p126_lease,0,__ATOMIC_ACQ_REL);
    if(lease)__atomic_store_n(&hakios_p126_reason,reason,__ATOMIC_RELEASE);
}
unsigned
hakios_p125_active(void)
{
    uint64_t v=__atomic_load_n(&hakios_p125_lease,__ATOMIC_ACQUIRE);
    if(!(v&3u))return 0;
    if(mach_absolute_time()>=(v&~3ULL))return 0;
    if((v&3u)==3u){
        if(!__atomic_load_n(&hakios_p125_usb_down_at,__ATOMIC_ACQUIRE) ||
            __atomic_load_n(&hakios_p125_usb_up_at,__ATOMIC_ACQUIRE))return 0;
        if(__atomic_load_n(&hakios_p1273_offline_fixed,__ATOMIC_ACQUIRE)){
            uint64_t deadline=__atomic_load_n(&hakios_p1273_offline_deadline,__ATOMIC_ACQUIRE);
            if(!deadline || mach_absolute_time()>=deadline || !hakios_p127_mode())return 0;
        }
        return __atomic_load_n(&hakios_p127_offline_aggregate,__ATOMIC_ACQUIRE)?1:2;
    }
    return (unsigned)(v&3u);
}
void hakios_p125_usb_event(unsigned connected)
{
    /* Retain physical link timestamps even after audit freeze. */
    __atomic_store_n(connected?&hakios_p12753_usb_up:&hakios_p12753_usb_down,mach_absolute_time(),__ATOMIC_RELEASE);
    hakios_p127_audio_wake();
    hakios_p1273_event(connected?P1273_USB_UP:P1273_USB_DOWN,1);
    if(p1274_live(mach_absolute_time()) && p1274_writer_enter(mach_absolute_time())){
        uint64_t zero=0,at=mach_absolute_time();
        uint64_t *slot=connected?&hakios_p1274_usb_up:&hakios_p1274_usb_down;
        if(!connected || __atomic_load_n(&hakios_p1274_usb_down,__ATOMIC_ACQUIRE))
            (void)__atomic_compare_exchange_n(slot,&zero,at,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
        p1274_writer_leave();
    }
    uint64_t v=__atomic_load_n(&hakios_p125_lease,__ATOMIC_ACQUIRE);
    if((v&3u)!=3u){
        /* Planned fixed capture freezes before reconnect. Retain the
         * later link-up timestamp without reopening the capture. */
        if(connected && !v &&
           __atomic_load_n(&hakios_p125_stop_reason,__ATOMIC_ACQUIRE)==10 &&
           __atomic_load_n(&hakios_p1273_offline_deadline,__ATOMIC_ACQUIRE) &&
           __atomic_load_n(&hakios_p125_usb_down_at,__ATOMIC_ACQUIRE)){
            uint64_t zero=0,now=mach_absolute_time();
            (void)__atomic_compare_exchange_n(&hakios_p125_usb_up_at,&zero,now,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
        }
        return;
    }
    uint64_t now=mach_absolute_time();
    if(now>=(v&~3ULL))return;
    uint64_t zero=0;
    if(!connected){
        /* First detected disconnect is the start. Repeated disconnects
         * never reset the capture. */
        if(__atomic_compare_exchange_n(&hakios_p125_usb_down_at,&zero,now,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)){
            __atomic_store_n(&hakios_p125_start,now,__ATOMIC_RELEASE);
            if(__atomic_load_n(&hakios_p1273_offline_fixed,__ATOMIC_ACQUIRE)){
                uint64_t duration=__atomic_load_n(&hakios_p1273_offline_duration,__ATOMIC_ACQUIRE);
                uint64_t policy=__atomic_load_n(&hakios_p127_lease,__ATOMIC_ACQUIRE)&~(uint64_t)P127_MASK;
                uint64_t end=v&~3ULL;
                /* Do not extend either lease. Short windows remain invalid
                 * in the host's exact duration check. */
                if(policy && policy<end)end=policy;
                if(duration && now+duration<end)end=now+duration;
                __atomic_store_n(&hakios_p1273_offline_deadline,end,__ATOMIC_RELEASE);
            }
        }
    }else if(__atomic_load_n(&hakios_p125_usb_down_at,__ATOMIC_ACQUIRE)){
        /* End before serial reconnect traffic; no rearm. */
        (void)__atomic_compare_exchange_n(&hakios_p125_usb_up_at,&zero,now,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
    }
}
void hakios_p125_display_event(unsigned on)
{
    if(on)__atomic_store_n(&hakios_p12753_display_on,mach_absolute_time(),__ATOMIC_RELEASE);
    if(on && p1274_live(mach_absolute_time()) && p1274_writer_enter(mach_absolute_time())){
        uint64_t zero=0,at=mach_absolute_time();
        (void)__atomic_compare_exchange_n(&hakios_p1274_display_on_at,&zero,at,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
        p1274_writer_leave();
    }
    if(on){hakios_p126_stop(4);hakios_p127_stop(4);}
    if(on)hakios_p1273_trace_stop(4,mach_absolute_time());
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
