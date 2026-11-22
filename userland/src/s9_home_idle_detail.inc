/*
 * S9 hi=2 deep source attribution. Each sample is read-only. The per-window
 * context sequence identifies which clients commit; a CA commit does not
 * prove an actual pixel changed. The Clock view probe checks UIKit geometry:
 * 'inside_window' is not a proof of z-order visibility/occlusion.
 * This file is included only from inject_sb.c after the HI group declarations.
 */
static void
hi_clock_view_snapshot(id target,uint64_t now)
{
    Class view_class=objc_getClass("UIView");
    if(!view_class || !target ||
        !SEND1(BOOL,target,"isKindOfClass:",Class,view_class)){
        say("hi: clock_view up=%llus type=%s UIView=0\n",
            (unsigned long long)(now/1000000),target?object_getClassName(target):"nil");
        return;
    }
    id window=SEND(id,target,"window");
    bool hidden=false;
    double alpha=1.0;
    unsigned levels=0;
    char ancestry[480]={0};
    size_t used=0;
    id v=target;
    while(v && levels<12){
        const char *klass=object_getClassName(v);
        BOOL h=SEND(BOOL,v,"isHidden");
        CGFloat a=SEND(CGFloat,v,"alpha");
        hidden |=h;
        alpha*=(double)a;
        int n=snprintf(ancestry+used,sizeof(ancestry)-used,"%s%s%s",
            levels?">":"",klass?klass:"nil",h?"[hidden]":"");
        if(n<0 || (size_t)n>=sizeof(ancestry)-used)break;
        used+=(size_t)n;
        levels++;
        v=SEND(id,v,"superview");
    }
    CGRect visible=CGRectNull, window_bounds=CGRectNull;
    bool inside=false;
    if(window){
        CGRect bounds=SEND(CGRect,target,"bounds");
        visible=((CGRect(*)(id,SEL,CGRect,id))objc_msgSend)
            (target,sel_registerName("convertRect:toView:"),bounds,window);
        window_bounds=SEND(CGRect,window,"bounds");
        inside=CGRectIntersectsRect(visible,window_bounds);
    }
    say("hi: clock_view up=%llus target=%p window=%s has_window=%u "
        "intersects_window=%u hidden_ancestors=%u effective_alpha=%.3f "
        "rect_in_window=%.1f,%.1f,%.1f,%.1f window_size=%.1f,%.1f ancestry=%s\n",
        (unsigned long long)(now/1000000),(void *)target,
        window?object_getClassName(window):"none",window?1u:0u,
        inside?1u:0u,hidden?1u:0u,alpha,
        window?visible.origin.x:0.0,window?visible.origin.y:0.0,
        window?visible.size.width:0.0,window?visible.size.height:0.0,
        window?window_bounds.size.width:0.0,window?window_bounds.size.height:0.0,ancestry);
}

static void
hi_anim_layer_details(id layer,unsigned depth,unsigned *budget,unsigned *printed)
{
    if(!layer || !*budget || depth>16 || *printed>=18)return;
    --*budget;
    id keys=SEND(id,layer,"animationKeys");
    unsigned long total=keys?SEND(unsigned long,keys,"count"):0;
    if(total){
        id delegate=SEND(id,layer,"delegate");
        for(unsigned long i=0;i<total && i<4 && *printed<18;i++){
            say("hi: anim layer=%s delegate=%s key=%s depth=%u\n",
                object_getClassName(layer),delegate?object_getClassName(delegate):"none",
                text_of(SEND1(id,keys,"objectAtIndex:",unsigned long,i)),depth);
            ++*printed;
        }
    }
    id children=SEND(id,layer,"sublayers");
    unsigned long n=children?SEND(unsigned long,children,"count"):0;
    for(unsigned long i=0;i<n && *budget && *printed<18;i++)
        hi_anim_layer_details(SEND1(id,children,"objectAtIndex:",unsigned long,i),
                              depth+1,budget,printed);
}

static void
hi_window_context_details(void)
{
    /* 5s commits accumulated by hi_sample(). These are window contexts,
     * not unique transactions. Avoid grabbing the entire CA layer tree. */
    for(unsigned i=0;i<hi_contexts_used;i++){
        const struct hi_context *c=&hi_contexts[i];
        if(c->delta)
            say("hi: ctx id=%#x window=%s commits_5s=%llu current_commit=%llu\n",
                c->id,c->window,(unsigned long long)c->delta,
                (unsigned long long)c->last);
    }
}

static void
hi_window_anim_snapshot(void)
{
    Class cls=objc_getClass("UIWindow");
    SEL all=sel_registerName("allWindowsIncludingInternalWindows:onlyVisibleWindows:");
    id windows=cls && class_getClassMethod(cls,all)?
        ((id(*)(Class,SEL,BOOL,BOOL))objc_msgSend)(cls,all,YES,YES):NULL;
    unsigned long n=windows?SEND(unsigned long,windows,"count"):0;
    unsigned budget=900,printed=0;
    for(unsigned long i=0;i<n && i<40 && budget && printed<18;i++){
        id w=SEND1(id,windows,"objectAtIndex:",unsigned long,i);
        if(w && strcmp(object_getClassName(w),"SBHomeScreenWindow")==0)
            hi_anim_layer_details(SEND(id,w,"layer"),0,&budget,&printed);
    }
    say("hi: anim_summary printed=%u scan_complete=%u\n",
        printed,budget?1u:0u);
}
