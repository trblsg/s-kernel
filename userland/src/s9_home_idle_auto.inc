/*
 * Autonomous, one-boot causal cross-over. Never dependent on the USB console.
 * It is armed only when hi=2 and an EXACT Clock target is active around 10Hz
 * while a usable Home window is on screen and input has been quiet >8s.
 * Sequence: baseline 20s, cut 20s, recover 20s, repeat cut 20s, recover 20s.
 * The causal cut suppresses only excessive Clock callbacks: 1 per second
 * continues, touch/app switch/screen lock/clock disappearance cancels.
 * A maximum of two re-attempts on interrupted tests, then passive only.
 */
static unsigned hi_auto_retries;

static void
hi_auto_disable_gate(void)
{
    __atomic_store_n(&hi_gate_group,0,__ATOMIC_RELEASE);
    __atomic_store_n(&hi_gate_deadline,0,__ATOMIC_RELEASE);
    __atomic_store_n(&hi_auto_clock_gate,false,__ATOMIC_RELEASE);
}

static void
hi_auto_enter(enum hi_auto_phase stage,uint64_t now)
{
    hi_auto_disable_gate();
    hi_auto_state=stage;
    hi_auto_end_us=now+HI_AUTO_SEGMENT_US;
    if(hi_auto_cut_phase(stage)){
        __atomic_store_n(&hi_gate_deadline,hi_auto_end_us,__ATOMIC_RELEASE);
        __atomic_store_n(&hi_auto_clock_gate,true,__ATOMIC_RELEASE);
        __atomic_store_n(&hi_gate_group,hi_auto_clock_id,__ATOMIC_RELEASE);
    }
    say("hi: auto=%s up=%llus source=%u gate=%u duration_s=20 "
        "input_safe=%u home=%u\n",hi_auto_names[stage],
        (unsigned long long)(now/1000000),hi_auto_clock_id,
        hi_auto_cut_phase(stage)?1u:0u,
        hi_event_hook_ok?1u:0u,hi_home_ready?1u:0u);
}

static void
hi_auto_cancel(const char *reason,bool manual)
{
    enum hi_auto_phase previous=hi_auto_state;
    if(!hi_auto_mode || previous==HI_AUTO_FINISHED ||
       previous==HI_AUTO_ABORTED)return;
    hi_auto_disable_gate();
    hi_auto_ready_windows=0;
    if(manual || ++hi_auto_retries>=3)
        hi_auto_state=HI_AUTO_ABORTED;
    else
        hi_auto_state=HI_AUTO_WAIT;
    say("hi: auto=ABORT from=%s reason=%s retries=%u next=%s\n",
        hi_auto_names[previous],reason,hi_auto_retries,hi_auto_names[hi_auto_state]);
}

static void
hi_auto_sample(uint64_t now)
{
    if(!hi_auto_mode || hi_auto_state!=HI_AUTO_WAIT)return;
    unsigned found=0, ticks=0;
    unsigned n=__atomic_load_n(&hi_groups_used,__ATOMIC_ACQUIRE);
    for(unsigned i=0;i<n;i++)if(hi_group_clock(i+1)){
        found=i+1;
        ticks=(unsigned)hi_groups[i].last_callbacks;
        break;
    }
    bool ready=hi_auto_candidate(now,
        __atomic_load_n(&hi_last_input,__ATOMIC_ACQUIRE),
        ticks,hi_home_ready,hi_event_hook_ok);
    hi_auto_ready_windows=ready?hi_auto_ready_windows+1:0;
    if(now>=HI_AUTO_BOOT_US && !found && !hi_auto_seen_failure){
        hi_auto_seen_failure=true;
        say("hi: auto=WAIT_NO_CLOCK_SOURCE up=%llus (passive profiling continues)\n",
            (unsigned long long)(now/1000000));
    }
    if(found && now>=HI_AUTO_BOOT_US && hi_auto_ready_windows==0 &&
       !hi_auto_seen_failure){
        hi_auto_seen_failure=true;
        say("hi: auto=WAIT_STABILITY clock_link=%u ticks_5s=%u "
            "home=%u input_quiet=%u hook=%u\n",
            found,ticks,hi_home_ready?1u:0u,
            now>=hi_last_input+HI_AUTO_INPUT_QUIET_US?1u:0u,
            hi_event_hook_ok?1u:0u);
    }
    if(hi_auto_ready_windows<2 || !found)return;
    hi_auto_clock_id=found;
    hi_auto_input_at_start=__atomic_load_n(&hi_last_input,__ATOMIC_ACQUIRE);
    say("hi: auto=START up=%llus confirmed_clock_link=%u "
        "ticks_5s=%u steady_windows=%u USB_not_required=1\n",
        (unsigned long long)(now/1000000),found,ticks,hi_auto_ready_windows);
    hi_auto_enter(HI_AUTO_BASELINE,now);
}

static void
hi_auto_tick(void)
{
    if(!hi_auto_mode || hi_auto_state<HI_AUTO_BASELINE ||
        hi_auto_state>=HI_AUTO_FINISHED)return;
    uint64_t now=hi_time_us();
    uint64_t last_clock=__atomic_load_n(&hi_groups[hi_auto_clock_id-1].last_tick_us,
                                          __ATOMIC_ACQUIRE);
    uint64_t input=__atomic_load_n(&hi_last_input,__ATOMIC_ACQUIRE);
    if(hi_auto_must_abort(now,hi_auto_input_at_start,input,last_clock,
                          __atomic_load_n(&hi_home_ready,__ATOMIC_ACQUIRE),
                          hi_event_hook_ok)){
        hi_auto_cancel("input_or_scene_or_timer_gap",false);
        return;
    }
    if(now<hi_auto_end_us)return;
    enum hi_auto_phase next=hi_auto_next(hi_auto_state);
    if(next==HI_AUTO_FINISHED){
        hi_auto_disable_gate();
        hi_auto_state=HI_AUTO_FINISHED;
        say("hi: auto=FINISHED up=%llus runs=2 controls_restored=1 "
            "manual_hi_cmd_still_available=1\n",
            (unsigned long long)(now/1000000));
        return;
    }
    hi_auto_enter(next,now);
}
