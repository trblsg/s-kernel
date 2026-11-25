/*
 * S9 Home Idle one-boot causal A/B, included after SpringBoard's CADisplayLink caller().
 * hi=1: passive observer and manual one-target intervention.
 * hi=2: passive observer, detailed context/view snapshots and autonomous baseline /
 * Clock-only cut / recovery / repeat within a single boot. The home must be
 * stable >180s; no serial command is required. All timed cuts fail OPEN on
 * touch, scene exit, timer disappearance or timeout. Never skip GL/present().
 */
#include "s9_home_idle_policy.h"
#include "s9_hi_auto_policy.h"
#include "s9_hi_verified_policy.h"
#include "s9_p9_clock_policy.h"
#include "s9_p10_lock_policy.h"
#define HI_GROUPS 48
#define HI_CONTEXTS 40
static bool hi_enabled, hi_event_hook_ok, hi_auto_mode;
static bool hi_p123_enabled;
static uint64_t hi_p123_first,hi_p123_last,hi_p123_forwarded,hi_p123_input_ticks;
static bool hi_verified_enabled; /* sysctl hi_policy=1; only after prior A/B FINISHED */
static bool hi_p9_hidden_clock_enabled; /* hi_policy=2: hidden Home Clock target only */
/* P10 source-specific lockscreen timers, independent of P9 GPU & panel.
 * Disabled by default. Epoch/proofs expire within 1.5s; no blind frames. */
static volatile uint32_t hi_p10_mode;
static volatile bool hi_p10_cover_proven,hi_p10_display_on;
static volatile bool hi_p10_anim_full,hi_p10_anim_absent,hi_p10_commit_quiet;
static volatile uint64_t hi_p10_lock_since,hi_p10_scene_at,hi_p10_quiet_since;
static uint32_t hi_verified_refusals;
static os_unfair_lock hi_group_lock = OS_UNFAIR_LOCK_INIT;
static struct hi_group {
    char target[92], action[72];
    uint64_t callbacks, forwarded, cut, last_callbacks;
    uint64_t last_tick_us, last_view_us, verified_cuts, p9_cuts, p10_clock_cuts, p10_anim_cuts;
    uint32_t requested_fps, instances;
} hi_groups[HI_GROUPS];
static unsigned hi_groups_used, hi_stage;
static volatile uint32_t hi_gate_group;
static uint64_t hi_p124_until,hi_p124_scene_at;
static uint32_t hi_p124_display,hi_p124_cuts;
static volatile uint64_t hi_gate_deadline, hi_last_input;
static volatile bool hi_home_ready, hi_animations_absent;
static void (*hi_original_send_event)(id,SEL,id);
static char hi_target_key, hi_data_key, hi_group_key;
static const char *const hi_stages[] = {"unset", "main", "secondary", "settings"};
struct hi_proxy_data {
    SEL action;
    uint32_t group;
    uint64_t last_forward;
    uint64_t last_checked;
    uint64_t verified_since;
    enum hi_view_status state;
};
struct hi_context {
    unsigned id;
    unsigned long long last, delta;
    char window[72];
};
static struct hi_context hi_contexts[HI_CONTEXTS];
static unsigned hi_contexts_used;
static enum hi_auto_phase hi_auto_state;
static unsigned hi_auto_clock_id, hi_auto_ready_windows;
static uint64_t hi_auto_end_us, hi_auto_input_at_start;
static bool hi_auto_clock_gate, hi_auto_seen_failure;
static const char *const hi_auto_names[] = {
    "wait", "baseline", "cut1", "recover1", "cut2", "recover2", "finished", "aborted"
};
static uint64_t hi_time_us(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000; }
#include "s9_hi_clock_resolve.inc"
#include "s9_home_idle_detail.inc"
#include "s9_home_idle_scene.inc"
static void hi_auto_sample(uint64_t now);
static void hi_auto_tick(void);
static void hi_auto_cancel(const char *reason,bool manual);
static void hi_auto_rearm(void);
static bool hi_p10_maybe_skip(uint64_t now,uint64_t forwarded,
    enum s9_p10_source source);
static void
hi_send_event(id self,SEL cmd,id event)
{
    __atomic_store_n(&hi_last_input,hi_time_us(),__ATOMIC_RELEASE);
    __atomic_store_n(&hi_p124_until,0,__ATOMIC_RELEASE); /* immediately fail open on any input */
    if(hi_p123_enabled)__atomic_store_n(&hi_p123_input_ticks,mach_absolute_time(),__ATOMIC_RELEASE);
    hi_original_send_event(self,cmd,event);
}
static unsigned
hi_register_group(id target,SEL action)
{
    const char *klass=target?object_getClassName(target):"-";
    const char *method=action?sel_getName(action):"-";
    unsigned id=0;
    os_unfair_lock_lock(&hi_group_lock);
    unsigned used=__atomic_load_n(&hi_groups_used,__ATOMIC_ACQUIRE);
    for(unsigned i=0;i<used;i++)
        if(!strcmp(hi_groups[i].target,klass) && !strcmp(hi_groups[i].action,method)){
            id=i+1;break;
        }
    if(!id && used<HI_GROUPS){
        struct hi_group *g=&hi_groups[used];
        snprintf(g->target,sizeof(g->target),"%s",klass);
        snprintf(g->action,sizeof(g->action),"%s",method);
        id=used+1;
        __atomic_store_n(&hi_groups_used,id,__ATOMIC_RELEASE);
        say("hi: link group=%u target=%s action=%s\n",id,g->target,g->action);
    }
    if(id)++hi_groups[id-1].instances;
    os_unfair_lock_unlock(&hi_group_lock);
    return id;
}
static bool
hi_group_clock(unsigned id)
{
    return id && id<=__atomic_load_n(&hi_groups_used,__ATOMIC_ACQUIRE) &&
        s9_hi_clock_source(hi_groups[id-1].target,hi_groups[id-1].action);
}
static void
hi_proxy_tick(id self,SEL cmd,id link)
{
    (void)cmd;
    CFMutableDataRef blob=(CFMutableDataRef)objc_getAssociatedObject(self,&hi_data_key);
    struct hi_proxy_data *p=blob?(struct hi_proxy_data *)CFDataGetMutableBytePtr(blob):NULL;
    id target=objc_getAssociatedObject(self,&hi_target_key);
    if(p==NULL || target==NULL || p->action==NULL)return;
    const unsigned group=p->group;
    const uint64_t now=hi_time_us();
    bool exact_clock=group && hi_group_clock(group);
    enum s9_p10_source p10_source=group && group<=HI_GROUPS?
        s9_p10_source(hi_groups[group-1].target,hi_groups[group-1].action):
        S9_P10_OTHER;
    if(group && group<=HI_GROUPS){
        struct hi_group *g=&hi_groups[group-1];
        __atomic_fetch_add(&g->callbacks,1,__ATOMIC_RELAXED);
        __atomic_store_n(&g->last_tick_us,now,__ATOMIC_RELEASE);
        if(hi_auto_mode && exact_clock){
            if(p->last_checked==0 || now<p->last_checked ||
               now-p->last_checked>=UINT64_C(2000000)){
                char why[144]="";
                enum hi_view_status state=hi_clock_resolve(target,why,sizeof(why));
                if(state!=p->state || state==HI_VIEW_UNKNOWN)
                    p->verified_since=state==HI_VIEW_UNKNOWN?0:now;
                p->state=state;
                p->last_checked=now;
                say("hi: clock_proof up=%llus owner=%s state=%s reason=%s "
                    "stable_for_ms=%llu policy=%u\n",
                    (unsigned long long)(now/1000000),object_getClassName(target),
                    hi_view_state_name(state),why,
                    (unsigned long long)(p->verified_since && now>=p->verified_since?
                    (now-p->verified_since)/1000:0),hi_verified_enabled?1u:0u);
            }
            if(g->last_view_us==0 || (hi_p123_enabled && now>=g->last_view_us &&
                now-g->last_view_us>=UINT64_C(20000000))){
                hi_clock_class_chain(target);
                g->last_view_us=now;
            }
        }
    }
    /* One explicitly armed visible Clock cadence trial, not visibility
     * inference or a global display-link/FPS limit. Forward first callback,
     * then at least once/second. Any unknown proof or input fails open. */
    uint64_t trial=__atomic_load_n(&hi_p124_until,__ATOMIC_ACQUIRE);
    uint64_t scene=__atomic_load_n(&hi_p124_scene_at,__ATOMIC_ACQUIRE);
    uint64_t input=__atomic_load_n(&hi_last_input,__ATOMIC_ACQUIRE);
    if(trial && now<trial && exact_clock && hi_event_hook_ok &&
        hi_home_ready && __atomic_load_n(&hi_p124_display,__ATOMIC_ACQUIRE)==1 &&
        scene && now>=scene && now-scene<=1500000 &&
        input && now>=input && now-input>=3000000 &&
        target==(id)objc_getClass("SBHClockApplicationIconImageView") &&
        p->last_forward && now>=p->last_forward && now-p->last_forward<1000000){
        __atomic_fetch_add(&hi_p124_cuts,1,__ATOMIC_RELAXED);
        __atomic_fetch_add(&hi_groups[group-1].cut,1,__ATOMIC_RELAXED);
        return;
    }
    bool verified_cut=exact_clock &&
        hi_verified_should_skip(now,__atomic_load_n(&hi_last_input,__ATOMIC_ACQUIRE),
            p->last_forward,p->verified_since,p->state,
            __atomic_load_n(&hi_stage,__ATOMIC_ACQUIRE)==1 &&
                __atomic_load_n(&hi_home_ready,__ATOMIC_ACQUIRE) &&
                hi_auto_state==HI_AUTO_FINISHED,
            hi_event_hook_ok,exact_clock,hi_verified_enabled);
    if(verified_cut){
        __atomic_fetch_add(&hi_groups[group-1].verified_cuts,1,__ATOMIC_RELAXED);
        __atomic_fetch_add(&hi_groups[group-1].cut,1,__ATOMIC_RELAXED);
        return;
    }
    /* P9 source fix: Clock Home-icon can continue ticking behind the
     * lockscreen or another app. When Home is definitely NOT visible and
     * input is quiet, forward >=1 callback/second. Unlike a global
     * compositor throttle, non-Clock links and real UI frames are untouched.
     * Never invoke this path for an unrecognized Objective-C target. */
    if(exact_clock && s9_p9_hidden_clock_skip(now,
        __atomic_load_n(&hi_last_input,__ATOMIC_ACQUIRE),p->last_forward,
        wallpaper_home_showing(),exact_clock,hi_event_hook_ok,
        hi_p9_hidden_clock_enabled)){
        __atomic_fetch_add(&hi_groups[group-1].p9_cuts,1,__ATOMIC_RELAXED);
        __atomic_fetch_add(&hi_groups[group-1].cut,1,__ATOMIC_RELAXED);
        return;
    }
    /* P10 never touches generic render/present or any unrecognized link.
     * A fresh verified lock screen, no input and target-specific frame
     * interval are mandatory. Animation-manager opt-in mode 2 additionally
     * requires all windows scanned, no CA animKeys and zero commits. */
    if(group && hi_p10_maybe_skip(now,p->last_forward,p10_source)){
        struct hi_group *g=&hi_groups[group-1];
        if(p10_source==S9_P10_PRECISE_CLOCK)
            __atomic_fetch_add(&g->p10_clock_cuts,1,__ATOMIC_RELAXED);
        else
            __atomic_fetch_add(&g->p10_anim_cuts,1,__ATOMIC_RELAXED);
        __atomic_fetch_add(&g->cut,1,__ATOMIC_RELAXED);
        return;
    }
    bool cut=group && group==__atomic_load_n(&hi_gate_group,__ATOMIC_ACQUIRE) &&
        s9_hi_skip(now,__atomic_load_n(&hi_gate_deadline,__ATOMIC_ACQUIRE),
           __atomic_load_n(&hi_last_input,__ATOMIC_ACQUIRE),p->last_forward,
           (__atomic_load_n(&hi_stage,__ATOMIC_ACQUIRE)==1 ||
            (hi_auto_mode && __atomic_load_n(&hi_auto_clock_gate,__ATOMIC_ACQUIRE) &&
             hi_auto_clock_id==group)),
           __atomic_load_n(&hi_home_ready,__ATOMIC_ACQUIRE),
           hi_event_hook_ok,hi_group_clock(group) ||
           __atomic_load_n(&hi_animations_absent,__ATOMIC_ACQUIRE));
    if(cut){
        __atomic_fetch_add(&hi_groups[group-1].cut,1,__ATOMIC_RELAXED);
        return;
    }
    p->last_forward=now;
    if(hi_p123_enabled && exact_clock){
        uint64_t stamp=mach_absolute_time(),zero=0;
        __atomic_compare_exchange_n(&hi_p123_first,&zero,stamp,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED);
        __atomic_store_n(&hi_p123_last,stamp,__ATOMIC_RELAXED);
        __atomic_fetch_add(&hi_p123_forwarded,1,__ATOMIC_RELAXED);
    }
    if(group && group<=HI_GROUPS)
        __atomic_fetch_add(&hi_groups[group-1].forwarded,1,__ATOMIC_RELAXED);
    ((void (*)(id,SEL,id))objc_msgSend)(target,p->action,link);
}
static Class
hi_proxy_class(void)
{
    static Class proxy;
    static dispatch_once_t once;
    dispatch_once(&once,^{
        Class cls=objc_allocateClassPair(objc_getClass("NSObject"),"S9HomeIdleLinkProxy",0);
        if(cls){
            class_addMethod(cls,sel_registerName("homeIdleTick:"),(IMP)hi_proxy_tick,"v@:@");
            objc_registerClassPair(cls);
            proxy=cls;
        }else proxy=objc_getClass("S9HomeIdleLinkProxy");
    });
    return proxy;
}
static id
hi_make_link(id receiver,SEL ctor,id target,SEL action)
{
    if(!target || !action)return original_display_link_with(receiver,ctor,target,action);
    const Class cls=hi_proxy_class();
    if(!cls)return original_display_link_with(receiver,ctor,target,action);
    id proxy=((id (*)(id,SEL))objc_msgSend)((id)cls,sel_registerName("new"));
    CFMutableDataRef blob=CFDataCreateMutable(kCFAllocatorDefault,sizeof(struct hi_proxy_data));
    if(!proxy || !blob){
        if(proxy)((void (*)(id,SEL))objc_msgSend)(proxy,sel_registerName("release"));
        if(blob)CFRelease(blob);
        return original_display_link_with(receiver,ctor,target,action);
    }
    CFDataSetLength(blob,sizeof(struct hi_proxy_data));
    struct hi_proxy_data p={.action=action,.group=hi_register_group(target,action),.last_forward=0};
    memcpy(CFDataGetMutableBytePtr(blob),&p,sizeof(p));
    objc_setAssociatedObject(proxy,&hi_data_key,(id)blob,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    objc_setAssociatedObject(proxy,&hi_target_key,target,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    CFRelease(blob);
    id link=original_display_link_with(receiver,ctor,proxy,sel_registerName("homeIdleTick:"));
    if(link && p.group){
        int number=(int)p.group;
        CFNumberRef boxed=CFNumberCreate(kCFAllocatorDefault,kCFNumberIntType,&number);
        if(boxed){
            objc_setAssociatedObject(link,&hi_group_key,(id)boxed,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
            CFRelease(boxed);
        }
    }
    ((void (*)(id,SEL))objc_msgSend)(proxy,sel_registerName("release"));
    return link;
}
static void
hi_set_requested_fps(id link,uint32_t fps)
{
    CFNumberRef boxed=(CFNumberRef)objc_getAssociatedObject(link,&hi_group_key);
    int found=0;
    if(boxed)CFNumberGetValue(boxed,kCFNumberIntType,&found);
    unsigned group=found>0?(unsigned)found:0;
    if(group && group<=HI_GROUPS)
        __atomic_store_n(&hi_groups[group-1].requested_fps,fps,__ATOMIC_RELAXED);
}
static unsigned
hi_count_animations(id layer,unsigned depth,unsigned *budget)
{
    if(!layer || depth>16 || !*budget)return 0;
    --*budget;
    id keys=SEND(id,layer,"animationKeys");
    unsigned count=keys?(unsigned)SEND(unsigned long,keys,"count"):0;
    id children=SEND(id,layer,"sublayers");
    unsigned long n=children?SEND(unsigned long,children,"count"):0;
    for(unsigned long i=0;i<n && *budget;i++)
        count+=hi_count_animations(SEND1(id,children,"objectAtIndex:",unsigned long,i),depth+1,budget);
    return count;
}
#include "s9_p10_lock_scene.inc"
static void
hi_context_reset(void)
{
    memset(hi_contexts,0,sizeof(hi_contexts));
    hi_contexts_used=0;
}
static void
hi_sample(void)
{
    if(!hi_enabled)return;
    Class cls=objc_getClass("UIWindow");
    SEL all=sel_registerName("allWindowsIncludingInternalWindows:onlyVisibleWindows:");
    id windows=cls && class_getClassMethod(cls,all)?
       ((id (*)(Class,SEL,BOOL,BOOL))objc_msgSend)(cls,all,YES,YES):NULL;
    unsigned long count=windows?SEND(unsigned long,windows,"count"):0;
    bool home=wallpaper_home_showing();
    unsigned animations=0, budget=1200, homewindows=0, contexts=0;
    for(unsigned long i=0;i<count && i<64;i++){
        id window=SEND1(id,windows,"objectAtIndex:",unsigned long,i);
        const char *name=object_getClassName(window);
        id layer=SEND(id,window,"layer");
        if(!strcmp(name,"SBHomeScreenWindow")){
            ++homewindows;
            animations+=hi_count_animations(layer,0,&budget);
        }
        id context=class_respondsToSelector(object_getClass(window),sel_registerName("_boundContext"))?
           SEND(id,window,"_boundContext"):NULL;
        if(!context)continue;
        unsigned cid=SEND(unsigned,context,"contextId");
        unsigned long long commit=SEND(unsigned long long,context,"commitId");
        if(!cid)continue;
        ++contexts;
        unsigned j=0;
        for(;j<hi_contexts_used;j++)if(hi_contexts[j].id==cid)break;
        if(j==hi_contexts_used && hi_contexts_used<HI_CONTEXTS){
            hi_contexts[j].id=cid;
            hi_contexts[j].last=commit;
            hi_contexts[j].delta=0;
            snprintf(hi_contexts[j].window,sizeof(hi_contexts[j].window),"%s",name);
            hi_contexts_used++;
        }else if(j<hi_contexts_used){
            snprintf(hi_contexts[j].window,sizeof(hi_contexts[j].window),"%s",name);
            if(commit>=hi_contexts[j].last){
                uint64_t d=commit-hi_contexts[j].last;
                hi_contexts[j].delta+=d;
            }
            hi_contexts[j].last=commit;
        }
    }
    hi_p10_sample_scene(windows,count,hi_time_us());
    bool ready=home && homewindows!=0 && budget!=0;
    __atomic_store_n(&hi_home_ready,ready,__ATOMIC_RELEASE);
    __atomic_store_n(&hi_p124_scene_at,ready?hi_time_us():0,__ATOMIC_RELEASE);
    if(!ready)__atomic_store_n(&hi_p124_until,0,__ATOMIC_RELEASE);
    __atomic_store_n(&hi_animations_absent,animations==0 && budget!=0,__ATOMIC_RELEASE);
    static unsigned tick;
    if(++tick%5!=0)return;
    if(hi_p123_enabled && tick%20==0)hi_clock_view_observe(windows,count,home);
    uint64_t now=hi_time_us(),deadline=__atomic_load_n(&hi_gate_deadline,__ATOMIC_ACQUIRE);
    unsigned gate=__atomic_load_n(&hi_gate_group,__ATOMIC_ACQUIRE);
    if(gate && now>=deadline){
        say("hi: gate=auto-off group=%u expired=1\n",gate);
        __atomic_store_n(&hi_gate_group,0,__ATOMIC_RELEASE);
        __atomic_store_n(&hi_gate_deadline,0,__ATOMIC_RELEASE);
        gate=0;
    }
    if(__atomic_load_n(&hi_p10_mode,__ATOMIC_ACQUIRE)){
        uint64_t lock_since=__atomic_load_n(&hi_p10_lock_since,__ATOMIC_ACQUIRE);
        uint64_t quiet_since=__atomic_load_n(&hi_p10_quiet_since,__ATOMIC_ACQUIRE);
        say("hi: p10_lock mode=%u display=%u cover_locked=%u "
            "stable_ms=%llu all_anim_scanned=%u anim_empty=%u commits_empty=%u "
            "anim_quiet_ms=%llu exact_sources_only=1 CA_GL_present_unchanged=1\n",
            __atomic_load_n(&hi_p10_mode,__ATOMIC_ACQUIRE),
            __atomic_load_n(&hi_p10_display_on,__ATOMIC_ACQUIRE)?1u:0u,
            __atomic_load_n(&hi_p10_cover_proven,__ATOMIC_ACQUIRE)?1u:0u,
            (unsigned long long)(lock_since&&now>=lock_since?(now-lock_since)/1000:0),
            __atomic_load_n(&hi_p10_anim_full,__ATOMIC_ACQUIRE)?1u:0u,
            __atomic_load_n(&hi_p10_anim_absent,__ATOMIC_ACQUIRE)?1u:0u,
            __atomic_load_n(&hi_p10_commit_quiet,__ATOMIC_ACQUIRE)?1u:0u,
            (unsigned long long)(quiet_since&&now>=quiet_since?
                (now-quiet_since)/1000:0));
    }
    if(hi_auto_mode){
        hi_window_context_details();
        hi_scene_snapshot();
        if(tick%2==0)hi_window_anim_snapshot();
        say("hi: verified status=%u allow=%u auto_finished=%u "
            "home_main=%u hook=%u unknown_fail_open=1\n",
            hi_verified_enabled?1u:0u,
            hi_verified_enabled && hi_auto_state==HI_AUTO_FINISHED &&
            hi_stage==1 && ready && hi_event_hook_ok?1u:0u,
            hi_auto_state==HI_AUTO_FINISHED?1u:0u,
            hi_stage==1?1u:0u,hi_event_hook_ok?1u:0u);
    }
    unsigned long long commits=0;
    for(unsigned i=0;i<hi_contexts_used;i++){
        commits+=hi_contexts[i].delta;
        hi_contexts[i].delta=0;
    }
    say("hi: phase=%s up=%llus visible_home=%u home_windows=%u windows=%lu contexts=%u "
        "commits_5s=%llu animations=%u anim_scan_complete=%u gate=%u seconds_left=%llu "
        "mali_framefloor=%d auto_state=%s auto_mode=%u\n",
        hi_stages[hi_stage],(unsigned long long)(now/1000000),ready?1:0,
        homewindows,count,contexts,commits,animations,budget?1:0,gate,
        (unsigned long long)(gate && deadline>now?(deadline-now)/1000000:0),
        sysctl_int("kern.hakios.mali_gov_frame"),
        hi_auto_names[hi_auto_state],hi_auto_mode?1u:0u);
    unsigned n=__atomic_load_n(&hi_groups_used,__ATOMIC_ACQUIRE);
    for(unsigned i=0;i<n;i++){
        struct hi_group *g=&hi_groups[i];
        uint64_t callbacks=__atomic_exchange_n(&g->callbacks,0,__ATOMIC_ACQ_REL);
        uint64_t forwarded=__atomic_exchange_n(&g->forwarded,0,__ATOMIC_ACQ_REL);
        uint64_t cut=__atomic_exchange_n(&g->cut,0,__ATOMIC_ACQ_REL);
        uint64_t verified=__atomic_exchange_n(&g->verified_cuts,0,__ATOMIC_ACQ_REL);
        uint64_t p9=__atomic_exchange_n(&g->p9_cuts,0,__ATOMIC_ACQ_REL);
        uint64_t p10clock=__atomic_exchange_n(&g->p10_clock_cuts,0,__ATOMIC_ACQ_REL);
        uint64_t p10anim=__atomic_exchange_n(&g->p10_anim_cuts,0,__ATOMIC_ACQ_REL);
        g->last_callbacks=callbacks;
        if(callbacks || cut)
            say("hi: link=%u target=%s action=%s ticks_5s=%llu forwarded=%llu "
                "cut=%llu fps_req=%u verified_cut=%llu p9_hidden_clock_cut=%llu "
                "p10_lock_clock_cut=%llu p10_quiet_anim_cut=%llu\n",
                i+1,g->target,g->action,(unsigned long long)callbacks,
                (unsigned long long)forwarded,(unsigned long long)cut,
                g->requested_fps,(unsigned long long)verified,(unsigned long long)p9,
                (unsigned long long)p10clock,(unsigned long long)p10anim);
    }
    if(hi_p123_enabled){
        uint64_t first=__atomic_exchange_n(&hi_p123_first,0,__ATOMIC_ACQ_REL);
        uint64_t last=__atomic_exchange_n(&hi_p123_last,0,__ATOMIC_ACQ_REL);
        uint64_t forwards=__atomic_exchange_n(&hi_p123_forwarded,0,__ATOMIC_ACQ_REL);
        say("p123-clock-window: ticks_now=%llu first_ticks=%llu last_ticks=%llu "
            "forwarded=%llu commits=%llu input_ticks=%llu home=%u source=window_not_frame_id\n",
            (unsigned long long)mach_absolute_time(),(unsigned long long)first,
            (unsigned long long)last,(unsigned long long)forwards,
            commits,(unsigned long long)__atomic_load_n(&hi_p123_input_ticks,__ATOMIC_RELAXED),
            ready?1u:0u);
    }
    hi_auto_sample(now);
}
static void hi_mark(unsigned stage);
static void hi_arm(unsigned selected);
/* USB's built-in console has 'sysctl', not rwtouch/notify or a shell.
 * A kernel mailbox read once per second is enough to steer every phase. */
static void
hi_usb_command(void)
{
    static int previous;
    static bool warned;
    int command=sysctl_int("kern.hakios.hi_cmd");
    if(command<0){
        __atomic_store_n(&hi_p124_until,0,__ATOMIC_RELEASE);
        if(!warned){warned=true;say("hi: CONTROL_FAIL mailbox_read=%d errno=%d "
            "auto_mode=%u passive_continues=1\n",command,errno,hi_auto_mode?1u:0u);}
        return;
    }
    if(command==previous)return;
    previous=command;
    say("hi: ctl=RECEIVED value=%d usb_kernel_to_SpringBoard=OK\n",command);
    if(command==250){
        say("hi: ping mode=%u auto_state=%s clock_link=%u home=%u "
            "input_hook=%u manual_phase=%s\n",
            hi_auto_mode?2u:1u,hi_auto_names[hi_auto_state],hi_auto_clock_id,
            hi_home_ready?1u:0u,hi_event_hook_ok?1u:0u,
            hi_stages[hi_stage]);
        return;
    }
    if(command==252){__atomic_store_n(&hi_p124_until,0,__ATOMIC_RELEASE);hi_auto_rearm();return;}
    if(command==253){
        uint64_t now=hi_time_us(),input=__atomic_load_n(&hi_last_input,__ATOMIC_ACQUIRE);
        uint64_t scene=__atomic_load_n(&hi_p124_scene_at,__ATOMIC_ACQUIRE);
        if(!hi_p123_enabled || !hi_event_hook_ok || !hi_home_ready ||
            hi_verified_enabled || hi_p9_hidden_clock_enabled ||
            __atomic_load_n(&hi_p10_mode,__ATOMIC_ACQUIRE) || hi_gate_group ||
            sysctl_int("kern.hakios.hi_display_requested")!=1 ||
            !scene || now<scene || now-scene>1500000 || !input || now<input || now-input<3000000 ||
            (hi_auto_state!=HI_AUTO_ABORTED && hi_auto_state!=HI_AUTO_FINISHED) ||
            __atomic_load_n(&hi_p124_until,__ATOMIC_ACQUIRE)>now){
            say("p124-clock: arm_refused proof=0\n");return;
        }
        __atomic_store_n(&hi_p124_display,1,__ATOMIC_RELEASE);
        __atomic_store_n(&hi_p124_cuts,0,__ATOMIC_RELAXED);
        __atomic_store_n(&hi_p124_until,now+25000000,__ATOMIC_RELEASE);
        say("p124-clock: armed=1 max_seconds=25 exact_class_timer_only=1 visible_cadence_trial=1\n");
        return;
    }
    if(command==300){hi_open_settings(false);return;}
    if(command==301){hi_open_settings(true);return;}
    __atomic_store_n(&hi_p124_until,0,__ATOMIC_RELEASE);
    if(hi_auto_mode)hi_auto_cancel("manual_usb_control",true);
    if(command>=1 && command<=3)hi_mark((unsigned)command);
    else if(command>=101 && command<=148)hi_arm((unsigned)(command-100));
    else if(command==199)hi_arm(UINT32_MAX);
    else if(command==200)layers();
    else if(command==0 || command==201){
        __atomic_store_n(&hi_gate_group,0,__ATOMIC_RELEASE);
        __atomic_store_n(&hi_gate_deadline,0,__ATOMIC_RELEASE);
        say("hi: gate=usb-stop phase=%s\n",hi_stages[hi_stage]);
    }else say("hi: usb-command=%d ignored (unsupported)\n",command);
}
static void
hi_pump(void)
{
    hi_usb_command();
    if(hi_p123_enabled && __atomic_load_n(&hi_p124_until,__ATOMIC_ACQUIRE)){
        uint64_t now=hi_time_us();
        int display=sysctl_int("kern.hakios.hi_display_requested");
        __atomic_store_n(&hi_p124_display,display==1?1:0,__ATOMIC_RELEASE);
        if(display!=1 || hi_verified_enabled || hi_p9_hidden_clock_enabled || hi_gate_group ||
            __atomic_load_n(&hi_p10_mode,__ATOMIC_ACQUIRE) || now>=__atomic_load_n(&hi_p124_until,__ATOMIC_ACQUIRE))
            __atomic_store_n(&hi_p124_until,0,__ATOMIC_RELEASE);
        say("p124-clock: active=%u cuts=%u display=%d\n",
            __atomic_load_n(&hi_p124_until,__ATOMIC_ACQUIRE)?1u:0u,
            __atomic_load_n(&hi_p124_cuts,__ATOMIC_RELAXED),display);
    }
    int lock_policy=sysctl_int("kern.hakios.hi_lock_policy");
    unsigned next_lock=lock_policy>=0 && lock_policy<=2?
        (unsigned)lock_policy:0u;
    if(next_lock!=__atomic_load_n(&hi_p10_mode,__ATOMIC_ACQUIRE)){
        __atomic_store_n(&hi_p10_mode,next_lock,__ATOMIC_RELEASE);
        __atomic_store_n(&hi_p10_cover_proven,false,__ATOMIC_RELEASE);
        __atomic_store_n(&hi_p10_lock_since,0,__ATOMIC_RELEASE);
        __atomic_store_n(&hi_p10_quiet_since,0,__ATOMIC_RELEASE);
        say("hi: p10_lock_policy=%u target_clock=SBUIPreciseClockTimer "
            "clock_min_forward_hz=1 target_UI_animation_mode2_only=1 "
            "anim_min_forward_hz=15 all_windows_proof=1 "
            "global_CA_GL_present_unchanged=1\n",next_lock);
    }
    int policy=sysctl_int("kern.hakios.hi_policy");
    bool verified=hi_auto_mode && policy==1;
    bool p9_hidden=hi_auto_mode && policy==2;
    if(hi_p9_hidden_clock_enabled!=p9_hidden){
        hi_p9_hidden_clock_enabled=p9_hidden;
        say("hi: p9_hidden_clock_policy=%u target=SBHClockApplicationIconImageView "
            "home_not_visible=1 input_quiet_s=3 min_forward_hz=1 "
            "global_CA_GL_frames_unchanged=1\\n",p9_hidden?1u:0u);
    }
    if(hi_verified_enabled!=verified){
        hi_verified_enabled=verified;
        say("hi: verified_policy=%u ui_owner_proof_required=1 "
            "unknown_always_forwards=1 previous_auto_finished=%u\n",
            verified?1u:0u,hi_auto_state==HI_AUTO_FINISHED?1u:0u);
    }
    hi_sample();
    hi_auto_tick();
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,NSEC_PER_SEC),dispatch_get_main_queue(),^{hi_pump();});
}
static void
hi_mark(unsigned stage)
{
    if(!hi_enabled)return;
    hi_sample();
    __atomic_store_n(&hi_stage,stage,__ATOMIC_RELEASE);
    hi_context_reset();
    __atomic_store_n(&hi_gate_group,0,__ATOMIC_RELEASE);
    __atomic_store_n(&hi_gate_deadline,0,__ATOMIC_RELEASE);
    __atomic_store_n(&hi_last_input,hi_time_us(),__ATOMIC_RELEASE);
    say("hi: mark=%s gate=off contexts_baseline=reset\n",hi_stages[stage]);
}
static void
hi_arm(unsigned selected)
{
    if(!hi_enabled)return;
    unsigned id=selected;
    if(id==UINT32_MAX){
        uint64_t best=0;id=0;
        for(unsigned i=0;i<__atomic_load_n(&hi_groups_used,__ATOMIC_ACQUIRE);i++)
            if(hi_groups[i].last_callbacks>best){
                id=i+1;best=hi_groups[i].last_callbacks;
            }
    }
    uint64_t now=hi_time_us();
    const bool clock_source=hi_group_clock(id);
    /* The stationary Home observed 6..9 persistent animationKeys even while
     * Clock itself is the only 10Hz callback. The original all-animations-
     * absent condition made the causal A/B impossible. Lift it ONLY for this
     * exact Clock target; other groups remain fail-open. */
    if(hi_stage!=1 || !hi_event_hook_ok || !hi_home_ready ||
        (!hi_animations_absent && !clock_source) || !id ||
        id>__atomic_load_n(&hi_groups_used,__ATOMIC_ACQUIRE)){
        say("hi: gate=REFUSED phase=%s target=%u clock_source=%u visible=%u no_anim=%u input_hook=%u\n",
            hi_stages[hi_stage],id,clock_source?1:0,
            hi_home_ready?1:0,hi_animations_absent?1:0,
            hi_event_hook_ok?1:0);
        return;
    }
    __atomic_store_n(&hi_last_input,now,__ATOMIC_RELEASE);
    __atomic_store_n(&hi_gate_deadline,now+S9_HI_GATE_US,__ATOMIC_RELEASE);
    __atomic_store_n(&hi_gate_group,id,__ATOMIC_RELEASE);
    say("hi: gate=armed target=%u class=%s action=%s clock_override=%u max_seconds=30 input_grace=2.5\n",
        id,hi_groups[id-1].target,hi_groups[id-1].action,clock_source?1:0);
}
#include "s9_home_idle_auto.inc"

static void
hi_start(void)
{
    id app=SEND(id,objc_getClass("UIApplication"),"sharedApplication");
    Class cls=app?object_getClass(app):objc_getClass("UIApplication");
    Method method=cls?class_getInstanceMethod(cls,sel_registerName("sendEvent:")):NULL;
    if(method){
        hi_original_send_event=(void (*)(id,SEL,id))method_setImplementation(method,(IMP)hi_send_event);
        hi_event_hook_ok=hi_original_send_event!=NULL;
    }
    __atomic_store_n(&hi_last_input,hi_time_us(),__ATOMIC_RELEASE);
    /* P12.3 baseline is passive; the exact Clock crossover is an explicit
     * bounded host command 252 only. Retain observers and the old guards. */
    if(hi_p123_enabled && hi_auto_mode){
        hi_auto_state=HI_AUTO_ABORTED;
        say("p123-clock: boot_passive=1 crossover_requires_explicit_252=1\n");
    }
    /* Actual userland read/write/read self-check: independent of USB typing.
     * The kernel console route is separately protected by CTLFLAG_KERN and
     * validated by CI. Auto A/B continues even when either route fails. */
    const char *mbox="kern.hakios.hi_cmd";
    int32_t probe=-1,confirmed=-1;
    size_t len=sizeof(probe);
    int read_rc=sysctlbyname(mbox,&probe,&len,NULL,0);
    int write_rc=-1,roundtrip_rc=-1;
    if(read_rc==0 && probe==0){
        write_rc=sysctlbyname(mbox,NULL,NULL,&probe,sizeof(probe));
        len=sizeof(confirmed);
        roundtrip_rc=sysctlbyname(mbox,&confirmed,&len,NULL,0);
    }
    say("hi: ctl_selftest read_rc=%d value=%d write_rc=%d readback_rc=%d "
        "readback=%d expected=0 kern_flag=SOURCE_VERIFIED "
        "auto_independent=%u\n",
        read_rc,probe,write_rc,roundtrip_rc,confirmed,hi_auto_mode?1u:0u);
    int token=0;
    notify_register_dispatch("com.hakios.hi.main",&token,dispatch_get_main_queue(),^(__unused int t){hi_mark(1);});
    notify_register_dispatch("com.hakios.hi.secondary",&token,dispatch_get_main_queue(),^(__unused int t){hi_mark(2);});
    notify_register_dispatch("com.hakios.hi.settings",&token,dispatch_get_main_queue(),^(__unused int t){hi_mark(3);});
    notify_register_dispatch("com.hakios.hi.snapshot",&token,dispatch_get_main_queue(),^(__unused int t){layers();});
    notify_register_dispatch("com.hakios.hi.stop",&token,dispatch_get_main_queue(),^(__unused int t){
        __atomic_store_n(&hi_gate_group,0,__ATOMIC_RELEASE);
        __atomic_store_n(&hi_gate_deadline,0,__ATOMIC_RELEASE);
        say("hi: gate=manual-off\n");
    });
    notify_register_dispatch("com.hakios.hi.cut.top",&token,dispatch_get_main_queue(),^(__unused int t){hi_arm(UINT32_MAX);});
    for(unsigned i=1;i<=32;i++){
        char name[64];snprintf(name,sizeof(name),"com.hakios.hi.cut.%u",i);
        unsigned select=i;
        notify_register_dispatch(name,&token,dispatch_get_main_queue(),^(__unused int t){hi_arm(select);});
    }
    say("hi: enabled oneboot links=%u policy=%s "
        "input_hook=%u group_max=%d usb_ctl=kern.hakios.hi_cmd "
        "autonomous_usb_independent=%u\n",
        hi_groups_used,hi_auto_mode?"autonomous_cross_over":"manual_forward_only",
        hi_event_hook_ok?1:0,HI_GROUPS,hi_auto_mode?1u:0u);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,2*NSEC_PER_SEC),dispatch_get_main_queue(),^{hi_pump();});
}
