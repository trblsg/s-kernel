/*
 * Live foreground proof and two launch paths for Settings.
 * (1) SpringBoardServices open URL from a background dispatch queue, using
 *     the same runtime symbol/signature already used in battery_test.c.
 * (2) The existing entitled /redwood/bin/battery_test open URL helper, spawned
 *     as a separate process if the direct service route refuses; or explicitly
 *     by hi_cmd=301 if the first route accepted but Settings did not appear.
 *
 * Neither a kernel sysctl ACK nor a URL-open return value proves Settings is
 * actually displayed. The Windows serial harness separately requires
 * settings_confirmed=1 from SpringBoard's observed frontmost application.
 */
static void
hi_scene_snapshot(void)
{
    id app=SEND(id,objc_getClass("UIApplication"),"sharedApplication");
    id front=NULL,identifier=NULL;
    SEL front_sel=sel_registerName("_accessibilityFrontMostApplication");
    if(app && class_respondsToSelector(object_getClass(app),front_sel))
        front=((id(*)(id,SEL))objc_msgSend)(app,front_sel);
    if(front){
        Class c=object_getClass(front);
        if(class_respondsToSelector(c,sel_registerName("bundleIdentifier")))
            identifier=SEND(id,front,"bundleIdentifier");
        if(!identifier && class_respondsToSelector(c,sel_registerName("displayIdentifier")))
            identifier=SEND(id,front,"displayIdentifier");
    }
    const char *bundle=identifier?SEND(const char*,identifier,"UTF8String"):"unknown";
    if(!bundle)bundle="unknown";
    say("hi: scene home=%u frontmost_class=%s bundle=%s "
        "app_state=%ld settings_confirmed=%u\n",
        wallpaper_home_showing()?1u:0u,front?object_getClassName(front):"none",
        bundle,app?SEND(long,app,"applicationState"):-1L,
        strcmp(bundle,"com.apple.Preferences")==0?1u:0u);
}

static void
hi_settings_spawn_helper(void)
{
    pid_t child=0;
    char *const argv[]={"/redwood/bin/battery_test","open","prefs:root=General",NULL};
    char *const envp[]={"PATH=/redwood/bin:/usr/bin:/bin",NULL};
    int err=posix_spawn(&child,argv[0],NULL,NULL,argv,envp);
    say("hi: settings_open method=battery_test_spawn error=%d pid=%d "
        "requested=prefs:root=General verify_scene_next=1\n",err,(int)child);
}

static void
hi_open_settings(bool force_helper)
{
    if(!hi_auto_mode || hi_auto_state!=HI_AUTO_FINISHED){
        say("hi: settings_open REFUSED mode=%u auto_state=%s "
            "need_finished=1\n",hi_auto_mode?1u:0u,hi_auto_names[hi_auto_state]);
        return;
    }
    say("hi: settings_open request=1 route=%s after_crossover=1\n",
        force_helper?"helper":"service_then_helper");
    /* Calling a synchronous SpringBoardServices URL opener on the main run
     * loop could deadlock the SpringBoard IPC server. Always use background.
     * This function is opt-in on explicit USB request, never boot triggered. */
    dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT,0),^{
        if(force_helper){hi_settings_spawn_helper();return;}
        void *sbs=dlopen("/System/Library/PrivateFrameworks/SpringBoardServices.framework/SpringBoardServices",RTLD_LAZY);
        int (*open_url)(CFURLRef,Boolean)=sbs?(int(*)(CFURLRef,Boolean))
            dlsym(sbs,"SBSOpenSensitiveURLAndUnlock"):NULL;
        const char *url_text="prefs:root=General";
        CFURLRef url=CFURLCreateWithBytes(kCFAllocatorDefault,
            (const UInt8*)url_text,(CFIndex)strlen(url_text),kCFStringEncodingUTF8,NULL);
        int accepted=0;
        if(open_url && url)accepted=open_url(url,true);
        say("hi: settings_open method=SBSOpenSensitiveURLAndUnlock "
            "available=%u accepted=%u verify_scene_next=1\n",
            open_url?1u:0u,accepted?1u:0u);
        if(url)CFRelease(url);
        if(!accepted)hi_settings_spawn_helper();
    });
}
