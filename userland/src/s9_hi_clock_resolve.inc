/*
 * Clock visibility/owner classifier for experimental policy. The old probe
 * incorrectly assumed the CADisplayLink target itself was a UIView; actual
 * S9 showed SBHClockApplicationIconImageView UIView=0. This resolves only
 * runtime-proven UIKit views through direct target, a declared view accessor,
 * or an object-typed view ivar. No private ABI struct offsets are guessed.
 *
 * UNKNOWN never suppresses a callback. A visible candidate overrides all
 * hidden/offscreen candidates. Multiple ambiguous candidates => UNKNOWN.
 * The evidence is logged with class chain and resolved view lineage.
 */
static const char *
hi_view_state_name(enum hi_view_status state)
{
    switch(state){
    case HI_VIEW_VISIBLE:return "visible";
    case HI_VIEW_DETACHED:return "detached";
    case HI_VIEW_HIDDEN:return "hidden";
    case HI_VIEW_OUTSIDE:return "outside";
    default:return "unknown";
    }
}

static bool
hi_is_ui_view(id candidate)
{
    Class cls=objc_getClass("UIView");
    return cls && candidate &&
        SEND1(BOOL,candidate,"isKindOfClass:",Class,cls);
}

static enum hi_view_status
hi_view_state(id view,char *reason,size_t reason_size)
{
    if(!hi_is_ui_view(view)){
        snprintf(reason,reason_size,"not_a_UIView");
        return HI_VIEW_UNKNOWN;
    }
    id window=SEND(id,view,"window");
    if(!window){
        snprintf(reason,reason_size,"UIView.window_nil");
        return HI_VIEW_DETACHED;
    }
    unsigned level=0;
    bool hidden=false;
    double alpha=1.;
    for(id parent=view;parent && level<20;parent=SEND(id,parent,"superview"),level++){
        hidden|=SEND(BOOL,parent,"isHidden");
        alpha*=(double)SEND(CGFloat,parent,"alpha");
    }
    if(level>=20){
        snprintf(reason,reason_size,"too_many_ancestors");
        return HI_VIEW_UNKNOWN;
    }
    if(hidden || alpha<=0.01){
        snprintf(reason,reason_size,"hidden_or_alpha");
        return HI_VIEW_HIDDEN;
    }
    CGRect bounds=SEND(CGRect,view,"bounds");
    CGRect frame=((CGRect(*)(id,SEL,CGRect,id))objc_msgSend)
        (view,sel_registerName("convertRect:toView:"),bounds,window);
    CGRect window_bounds=SEND(CGRect,window,"bounds");
    if(!isfinite(frame.origin.x) || !isfinite(frame.origin.y) ||
       !isfinite(frame.size.width) || !isfinite(frame.size.height) ||
       frame.size.width<=0 || frame.size.height<=0){
        snprintf(reason,reason_size,"invalid_or_zero_geometry");
        return HI_VIEW_UNKNOWN;
    }
    if(!CGRectIntersectsRect(frame,window_bounds)){
        snprintf(reason,reason_size,"view_outside_window");
        return HI_VIEW_OUTSIDE;
    }
    snprintf(reason,reason_size,"view_intersects_window");
    return HI_VIEW_VISIBLE;
}

static enum hi_view_status
hi_clock_resolve(id target,char *how,size_t how_size)
{
    if(!target){
        snprintf(how,how_size,"nil_target");
        return HI_VIEW_UNKNOWN;
    }
    if(hi_is_ui_view(target)){
        char reason[70];
        enum hi_view_status s=hi_view_state(target,reason,sizeof(reason));
        snprintf(how,how_size,"self:%s",reason);
        return s;
    }
    const char *const props[]={"iconImageView","imageView","iconView","view","contentView"};
    enum hi_view_status chosen=HI_VIEW_UNKNOWN;
    unsigned candidates=0;
    char selected[82]="none";
    for(unsigned i=0;i<sizeof(props)/sizeof(props[0]);i++){
        SEL sel=sel_registerName(props[i]);
        /* A matching selector name does not guarantee an object return.
         * Casting a scalar/CGRect/void method result to 'id' and calling
         * isKindOfClass: would dereference a bogus pointer and crash
         * SpringBoard. Inspect the runtime ABI BEFORE invoking any getter. */
        Method getter=class_getInstanceMethod(object_getClass(target),sel);
        char return_type[20]={0};
        if(!getter || method_getNumberOfArguments(getter)!=2)continue;
        method_getReturnType(getter,return_type,sizeof(return_type));
        if(return_type[0]!='@')continue;
        id candidate=((id(*)(id,SEL))objc_msgSend)(target,sel);
        if(!hi_is_ui_view(candidate))continue;
        char why[70];
        enum hi_view_status state=hi_view_state(candidate,why,sizeof(why));
        if(state==HI_VIEW_VISIBLE){
            snprintf(how,how_size,"selector_%s:%s",props[i],why);
            return state;
        }
        if(state==HI_VIEW_UNKNOWN)continue;
        if(candidates && chosen!=state){
            snprintf(how,how_size,"conflicting_view_properties");
            return HI_VIEW_UNKNOWN;
        }
        chosen=state;
        ++candidates;
        snprintf(selected,sizeof(selected),"selector_%s:%s",props[i],why);
    }
    /* An object ivar is inspectable only with type encoding '@'.
     * No scalar/pointer/weak-to-non-ObjC field is dereferenced. */
    for(Class cls=object_getClass(target);cls && cls!=objc_getClass("NSObject");
        cls=class_getSuperclass(cls)){
        unsigned count=0;
        Ivar *ivars=class_copyIvarList(cls,&count);
        for(unsigned i=0;i<count && i<48;i++){
            const char *name=ivar_getName(ivars[i]);
            const char *encoding=ivar_getTypeEncoding(ivars[i]);
            if(!name || !encoding || encoding[0]!='@')continue;
            if(!strstr(name,"view") && !strstr(name,"View") &&
               !strstr(name,"icon") && !strstr(name,"Icon"))continue;
            id candidate=object_getIvar(target,ivars[i]);
            if(!hi_is_ui_view(candidate))continue;
            char why[70];
            enum hi_view_status state=hi_view_state(candidate,why,sizeof(why));
            if(state==HI_VIEW_VISIBLE){
                snprintf(how,how_size,"ivar_%s:%s",name,why);
                free(ivars);
                return state;
            }
            if(state==HI_VIEW_UNKNOWN)continue;
            if(candidates && chosen!=state){
                snprintf(how,how_size,"conflicting_view_ivars");
                free(ivars);
                return HI_VIEW_UNKNOWN;
            }
            chosen=state;
            ++candidates;
            snprintf(selected,sizeof(selected),"ivar_%s:%s",name,why);
        }
        free(ivars);
    }
    if(candidates==0){
        snprintf(how,how_size,"no_verified_UIView_owner");
        return HI_VIEW_UNKNOWN;
    }
    snprintf(how,how_size,"%s",selected);
    return chosen;
}

static void
hi_clock_class_chain(id target)
{
    char chain[520]="";
    size_t used=0;
    unsigned depth=0;
    for(Class cls=target?object_getClass(target):NULL;cls && depth<14;
        cls=class_getSuperclass(cls),depth++){
        const char *name=class_getName(cls);
        int len=snprintf(chain+used,sizeof(chain)-used,"%s%s",
                         depth?">":"",name?name:"nil");
        if(len<0 || (size_t)len>=sizeof(chain)-used)break;
        used+=(size_t)len;
    }
    say("hi: clock_owner class_chain=%s UIKit_UIView=%u "
        "owner_is_view=%u candidate_resolver=runtime_checked\n",
        chain,objc_getClass("UIView")?1u:0u,
        hi_is_ui_view(target)?1u:0u);
}
