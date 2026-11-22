/*
 * S9 Home Idle one-boot causal A/B, included after SpringBoard's CADisplayLink caller().
 * Boot: hi=1. No work, hooks or frame suppression without that exact token.
 * Read-only by default. A manual notification can thin exactly ONE identified
 * CADisplayLink target for at most 30 s. The guard fails OPEN for input, visible
 * animations, an app/lockscreen in front, missing event hook, and any time expiry.
 * Never touches the frame end, GL, CA transactions or the panel.
 */
#include "s9_home_idle_policy.h"
#define HI_GROUPS 48
#define HI_CONTEXTS 40
static bool hi_enabled, hi_event_hook_ok;
static os_unfair_lock hi_group_lock = OS_UNFAIR_LOCK_INIT;
static struct hi_group {
    char target[92], action[72];
    uint64_t callbacks, forwarded, cut, last_callbacks;
    uint32_t requested_fps, instances;
} hi_groups[HI_GROUPS];
static unsigned hi_groups_used, hi_stage;
static volatile uint32_t hi_gate_group;
static volatile uint64_t hi_gate_deadline, hi_last_input;
static volatile bool hi_home_ready, hi_animations_absent;
static void (*hi_original_send_event)(id,SEL,id);
static char hi_target_key, hi_data_key, hi_group_key;
static const char *const hi_stages[] = {"unset", "main", "secondary", "settings"};
struct hi_proxy_data {
    SEL action;
    uint32_t group;
    uint64_t last_forward;
};
struct hi_context {
    unsigned id;
    unsigned long long last, delta;
};
static struct hi_context hi_contexts[HI_CONTEXTS];
static unsigned hi_contexts_used;
static uint64_t hi_time_us(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000; }
static void
hi_send_event(id self,SEL cmd,id event)
{
    __atomic_store_n(&hi_last_input,hi_time_us(),__ATOMIC_RELEASE);
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
    if(group && group<=HI_GROUPS)
        __atomic_fetch_add(&hi_groups[group-1].callbacks,1,__ATOMIC_RELAXED);
    bool cut=group && group==__atomic_load_n(&hi_gate_group,__ATOMIC_ACQUIRE) &&
        s9_hi_skip(now,__atomic_load_n(&hi_gate_deadline,__ATOMIC_ACQUIRE),
           __atomic_load_n(&hi_last_input,__ATOMIC_ACQUIRE),p->last_forward,
           __atomic_load_n(&hi_stage,__ATOMIC_ACQUIRE)==1,__atomic_load_n(&hi_home_ready,__ATOMIC_ACQUIRE),
           hi_event_hook_ok,__atomic_load_n(&hi_animations_absent,__ATOMIC_ACQUIRE));
    if(cut){
        __atomic_fetch_add(&hi_groups[group-1].cut,1,__ATOMIC_RELAXED);
        return;
    }
    p->last_forward=now;
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
            hi_contexts_used++;
        }else if(j<hi_contexts_used){
            if(commit>=hi_contexts[j].last){
                uint64_t d=commit-hi_contexts[j].last;
                hi_contexts[j].delta+=d;
            }
            hi_contexts[j].last=commit;
        }
    }
    bool ready=home && homewindows!=0 && budget!=0;
    __atomic_store_n(&hi_home_ready,ready,__ATOMIC_RELEASE);
    __atomic_store_n(&hi_animations_absent,animations==0 && budget!=0,__ATOMIC_RELEASE);
    static unsigned tick;
    if(++tick%5!=0)return;
    uint64_t now=hi_time_us(),deadline=__atomic_load_n(&hi_gate_deadline,__ATOMIC_ACQUIRE);
    unsigned gate=__atomic_load_n(&hi_gate_group,__ATOMIC_ACQUIRE);
    if(gate && now>=deadline){
        say("hi: gate=auto-off group=%u expired=1\n",gate);
        __atomic_store_n(&hi_gate_group,0,__ATOMIC_RELEASE);
        __atomic_store_n(&hi_gate_deadline,0,__ATOMIC_RELEASE);
        gate=0;
    }
    unsigned long long commits=0;
    for(unsigned i=0;i<hi_contexts_used;i++){
        commits+=hi_contexts[i].delta;
        hi_contexts[i].delta=0;
    }
    say("hi: phase=%s up=%llus visible_home=%u home_windows=%u windows=%lu contexts=%u "
        "commits_5s=%llu animations=%u anim_scan_complete=%u gate=%u seconds_left=%llu "
        "mali_framefloor=%d\n",
        hi_stages[hi_stage],(unsigned long long)(now/1000000),ready?1:0,
        homewindows,count,contexts,commits,animations,budget?1:0,gate,
        (unsigned long long)(gate && deadline>now?(deadline-now)/1000000:0),
        sysctl_int("kern.hakios.mali_gov_frame"));
    unsigned n=__atomic_load_n(&hi_groups_used,__ATOMIC_ACQUIRE);
    for(unsigned i=0;i<n;i++){
        struct hi_group *g=&hi_groups[i];
        uint64_t callbacks=__atomic_exchange_n(&g->callbacks,0,__ATOMIC_ACQ_REL);
        uint64_t forwarded=__atomic_exchange_n(&g->forwarded,0,__ATOMIC_ACQ_REL);
        uint64_t cut=__atomic_exchange_n(&g->cut,0,__ATOMIC_ACQ_REL);
        g->last_callbacks=callbacks;
        if(callbacks || cut)
            say("hi: link=%u target=%s action=%s ticks_5s=%llu forwarded=%llu cut=%llu fps_req=%u\n",
                i+1,g->target,g->action,(unsigned long long)callbacks,
                (unsigned long long)forwarded,(unsigned long long)cut,g->requested_fps);
    }
}
static void hi_mark(unsigned stage);
static void hi_arm(unsigned selected);
/* USB's built-in console has 'sysctl', not rwtouch/notify or a shell.
 * A kernel mailbox read once per second is enough to steer every phase. */
static void
hi_usb_command(void)
{
    static int previous;
    int command=sysctl_int("kern.hakios.hi_cmd");
    if(command<0 || command==previous)return;
    previous=command;
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
    hi_sample();
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
    if(hi_stage!=1 || !hi_event_hook_ok || !hi_home_ready ||
        !hi_animations_absent || !id || id>__atomic_load_n(&hi_groups_used,__ATOMIC_ACQUIRE)){
        say("hi: gate=REFUSED phase=%s target=%u visible=%u no_anim=%u input_hook=%u\n",
            hi_stages[hi_stage],id,hi_home_ready?1:0,hi_animations_absent?1:0,
            hi_event_hook_ok?1:0);
        return;
    }
    __atomic_store_n(&hi_last_input,now,__ATOMIC_RELEASE);
    __atomic_store_n(&hi_gate_deadline,now+S9_HI_GATE_US,__ATOMIC_RELEASE);
    __atomic_store_n(&hi_gate_group,id,__ATOMIC_RELEASE);
    say("hi: gate=armed target=%u class=%s action=%s max_seconds=30 input_grace=2.5\n",
        id,hi_groups[id-1].target,hi_groups[id-1].action);
}
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
    say("hi: enabled oneboot links=%u policy=forward-only until explicit cut "
        "input_hook=%u group_max=%d usb_ctl=kern.hakios.hi_cmd no_boot_arg_changes_after_this=1\n",
        hi_groups_used,hi_event_hook_ok?1:0,HI_GROUPS);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,2*NSEC_PER_SEC),dispatch_get_main_queue(),^{hi_pump();});
}
