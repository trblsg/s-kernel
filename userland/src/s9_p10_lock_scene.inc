/* P10: never suppress global CA render/GL present.
 * Learn the active lock screen from SBLockScreenManager.isUILocked AND an
 * actual, unhidden SBCoverSheetWindow AND GenericARMPE's display request.
 * Every missing/unknown Objective-C ABI or window fails open.
 *
 * Sampled on SpringBoard's main thread (hi_sample once/second). Cadisplaylink
 * forwarding checks a fresh sample and independently rechecks isUILocked.
 */
static bool
hi_p10_is_locked(void)
{
    Class cls=objc_getClass("SBLockScreenManager");
    SEL shared_sel=sel_registerName("sharedInstance");
    SEL lock_sel=sel_registerName("isUILocked");
    Method shared=cls?class_getClassMethod(cls,shared_sel):NULL;
    Method locked=cls?class_getInstanceMethod(cls,lock_sel):NULL;
    char shared_return[8]={0},lock_return[8]={0};
    if(!shared || !locked || method_getNumberOfArguments(shared)!=2 ||
       method_getNumberOfArguments(locked)!=2)return false;
    method_getReturnType(shared,shared_return,sizeof(shared_return));
    method_getReturnType(locked,lock_return,sizeof(lock_return));
    if(shared_return[0]!='@' ||
       (lock_return[0]!='B' && lock_return[0]!='c'))return false;
    id manager=((id(*)(Class,SEL))objc_msgSend)(cls,shared_sel);
    return manager && ((BOOL(*)(id,SEL))objc_msgSend)(manager,lock_sel);
}

static void
hi_p10_sample_scene(id windows,unsigned long count,uint64_t now)
{
    unsigned mode=__atomic_load_n(&hi_p10_mode,__ATOMIC_ACQUIRE);
    if(!mode){
        __atomic_store_n(&hi_p10_cover_proven,false,__ATOMIC_RELEASE);
        __atomic_store_n(&hi_p10_lock_since,0,__ATOMIC_RELEASE);
        __atomic_store_n(&hi_p10_quiet_since,0,__ATOMIC_RELEASE);
        return;
    }
    bool display_on=sysctl_int("kern.hakios.hi_display_requested")==1;
    bool locked=hi_p10_is_locked();
    bool cover=false;
    unsigned cover_count=0;
    for(unsigned long i=0;windows && i<count && i<64;i++){
        id w=SEND1(id,windows,"objectAtIndex:",unsigned long,i);
        if(!w || strcmp(object_getClassName(w),"SBCoverSheetWindow"))continue;
        ++cover_count;
        /* A hidden/transparent cover window does not prove lockscreen. */
        if(!SEND(BOOL,w,"isHidden") && SEND(CGFloat,w,"alpha")>=0.99){
            CGRect bounds=SEND(CGRect,w,"bounds");
            if(isfinite(bounds.size.width) && isfinite(bounds.size.height) &&
               bounds.size.width>100 && bounds.size.height>100)
                cover=true;
        }
    }
    bool scene=display_on && locked && cover && cover_count==1 &&
        count>0 && count<=64 && hi_event_hook_ok;
    uint64_t old=__atomic_load_n(&hi_p10_lock_since,__ATOMIC_ACQUIRE);
    __atomic_store_n(&hi_p10_display_on,display_on,__ATOMIC_RELEASE);
    __atomic_store_n(&hi_p10_cover_proven,scene,__ATOMIC_RELEASE);
    __atomic_store_n(&hi_p10_scene_at,now,__ATOMIC_RELEASE);
    __atomic_store_n(&hi_p10_lock_since,scene?(old?old:now):0,__ATOMIC_RELEASE);
    /* Mode 2: last-resort UIKit-animation timer only, and only if ALL
     * visible UIWindow CA subtrees were exhaustively scanned with no
     * active animationKeys and no visible-window context commits.
     * This is intentionally stricter than source-specific Clock mode. */
    bool full=false,no_anim=false,no_commit=false;
    if(scene && mode==2){
        unsigned budget=2500,animations=0;
        for(unsigned long i=0;windows && i<count && i<64 && budget;i++){
            id w=SEND1(id,windows,"objectAtIndex:",unsigned long,i);
            if(w)animations+=hi_count_animations(SEND(id,w,"layer"),0,&budget);
        }
        full=budget>0;
        no_anim=full && animations==0;
        no_commit=hi_contexts_used>0;
        for(unsigned i=0;i<hi_contexts_used;i++)
            if(hi_contexts[i].delta)no_commit=false;
    }
    uint64_t quiet=__atomic_load_n(&hi_p10_quiet_since,__ATOMIC_ACQUIRE);
    if(!scene || !full || !no_anim || !no_commit)quiet=0;
    else if(!quiet)quiet=now;
    __atomic_store_n(&hi_p10_quiet_since,quiet,__ATOMIC_RELEASE);
    __atomic_store_n(&hi_p10_anim_full,full,__ATOMIC_RELEASE);
    __atomic_store_n(&hi_p10_anim_absent,no_anim,__ATOMIC_RELEASE);
    __atomic_store_n(&hi_p10_commit_quiet,no_commit,__ATOMIC_RELEASE);
}

static bool
hi_p10_maybe_skip(uint64_t now,uint64_t forwarded,
    enum s9_p10_source source)
{
    unsigned mode=__atomic_load_n(&hi_p10_mode,__ATOMIC_ACQUIRE);
    if(!mode || source==S9_P10_OTHER ||
       !__atomic_load_n(&hi_p10_cover_proven,__ATOMIC_ACQUIRE))
        return false;
    return s9_p10_skip(now,
        __atomic_load_n(&hi_last_input,__ATOMIC_ACQUIRE),forwarded,
        __atomic_load_n(&hi_p10_lock_since,__ATOMIC_ACQUIRE),
        __atomic_load_n(&hi_p10_scene_at,__ATOMIC_ACQUIRE),
        __atomic_load_n(&hi_p10_quiet_since,__ATOMIC_ACQUIRE),
        __atomic_load_n(&hi_p10_display_on,__ATOMIC_ACQUIRE),
        hi_p10_is_locked(),__atomic_load_n(&hi_p10_cover_proven,__ATOMIC_ACQUIRE),
        hi_event_hook_ok,__atomic_load_n(&hi_p10_anim_full,__ATOMIC_ACQUIRE),
        __atomic_load_n(&hi_p10_anim_absent,__ATOMIC_ACQUIRE),
        __atomic_load_n(&hi_p10_commit_quiet,__ATOMIC_ACQUIRE),
        mode,source);
}
