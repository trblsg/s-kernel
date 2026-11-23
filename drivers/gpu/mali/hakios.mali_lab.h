/* S9 G3D Power Lab: included from hakios.mali.c AFTER the sleep sysctls.
 * All new controls default OFF. No GPU MMIO is accessed by the sampler.
 * No new boot arguments or userland ABI are required.
 */
static void
g3d_lab_mon(__unused void *arg, __unused wait_result_t why)
{
	for (;;) {
		uint64_t deadline, up_ns, now, off_ns = 0, off_total_ns, wake_us;
		uint32_t status, pll, mux, mode, phase, mv, fault, clocks, rail_probe, off_count;
		uint32_t wakes, wake_failed, shadow, denied, jobs, rail_good, rail_mv;
		uint32_t busy_cnt, gpu_safe, tz_ok, awake_state, ctrl = 0;
		int32_t temp_mc;

		/* Timed kernel sleep, NOT delay(): do not burn a CPU spinning. */
		lck_mtx_lock(&mali_queue_lock);
		clock_interval_to_deadline(5000, NSEC_PER_MSEC, &deadline);
		(void)lck_mtx_sleep_deadline(&mali_queue_lock, LCK_SLEEP_DEFAULT,
		    (event_t)&g3d_lab_monitor_started, THREAD_UNINT, deadline);
		lck_mtx_unlock(&mali_queue_lock);
		/* P5 reuses this independent 5s worker across DISPLAY OFF.
		 * Only PMU/CMU (outside G3D) are read; never touch GPU MMIO. */
		if (!g3d_lab_gate && g3d_lab_mv == 950 && !g3d_lab_rail_probe &&
		    !g3d_lab_fault && !g3d_lab_phase && !__atomic_load_n(&p68_mode,__ATOMIC_RELAXED) &&
		    !__atomic_load_n(&p5_mode,__ATOMIC_RELAXED) &&
		    !__atomic_load_n(&p7_gpu_mode,__ATOMIC_RELAXED))
			continue;
		if (pmu == 0 || cmu_g3d == 0)
			continue;

		lck_mtx_lock(&mali_job_lock);
		now = mach_absolute_time();
		/* P7 only: auto-expire any real long-quiet gate started during a
		 * connected serial A/B. This remains live if the host unplugs. */
		p7_gpu_gate_watchdog(now);
		absolutetime_to_nanoseconds(now, &up_ns);
		status = rd(pmu, PMU_G3D_STATUS);
		pll = rd(cmu_g3d, PLL_CON0);
		mux = rd(cmu_g3d, MUX_G3D_USER);
		mode = g3d_lab_gate;
		phase = g3d_lab_phase;
		mv = g3d_lab_mv;
		fault = g3d_lab_fault;
		gpu_safe = g3d_lab_gpu_safe ? 1u : 0u;
		awake_state = g3d_awake ? 1u : 0u; /* driver hint, separate from PMU status */
		tz_ok = g3d_lab_protection_ok ? 1u : 0u;
		clocks = ((status & G3D_ON) == G3D_ON && (pll & PLL_ENABLE) &&
		    gov_level < GOV_LEVELS) ? g3d_rates[gov_level].mhz : 0;
		off_count = g3d_lab_off_count;
		wakes = g3d_lab_wake_count;
		wake_failed = g3d_lab_wake_failed;
		shadow = g3d_lab_shadow_count;
		denied = g3d_lab_denied_count;
		jobs = g3d_lab_job_faults;
		wake_us = g3d_lab_last_wake_us;
		if (g3d_off_at != 0 && !g3d_awake) {
			absolutetime_to_nanoseconds(now >= g3d_off_at ? now - g3d_off_at : 0, &off_ns);
		}
		off_total_ns = g3d_lab_off_residency_ns + off_ns;
		rail_probe = g3d_lab_rail_probe;
		if (rail_probe && acpm_ok &&
		    (g3d_lab_last_rail_at == 0 || g3d_lab_age_ms(now, g3d_lab_last_rail_at) >= 30000)) {
			uint8_t c = 0, v = 0;
			const int e1 = hakios_acpm_pmic_read(HAKIOS_ACPM_PMIC_PMIC, B6CTRL, &c);
			const int e2 = e1 == 0 ? hakios_acpm_pmic_read(HAKIOS_ACPM_PMIC_PMIC, B6OUT, &v) : e1;
			g3d_lab_last_rail_at = now;
			g3d_lab_rail_valid = e2 == 0;
			if (g3d_lab_rail_valid) {
				g3d_lab_rail_ctrl = c;
				g3d_lab_rail_vsel = v;
			}
		}
		rail_good = g3d_lab_rail_valid ? 1u : 0u;
		rail_mv = rail_good ? s9_g3d_lab_mv_from_vsel(g3d_lab_rail_vsel) : 0u;
		ctrl = rail_good ? g3d_lab_rail_ctrl : 0u;
		lck_mtx_unlock(&mali_job_lock);
		temp_mc = hakios_exynos_tmu_zone_mc(2);
		busy_cnt = (status & G3D_ON) == G3D_ON ? 1u : 0u;
		/* Includes true OFF samples: unlike mali-probe this independent
		 * sampler survives when the G3D worker stops calling gov_window. */
		/* Do not emit G3D Lab lines when P5 is the only observer. */
		if (g3d_lab_gate || g3d_lab_mv != 950 || g3d_lab_rail_probe ||
		    g3d_lab_fault || g3d_lab_phase)
		say("g3d-lab: up=%llums phase=%u mode=%u domain=%u gpu_safe=%u tz_ok=%u pll=%u mux=%u clk=%uMHz target_mv=%u rail_read=%u rail_valid=%u rail_mv=%u rail_ctrl=%#x off_count=%u off_total_ms=%llu wake_count=%u wake_fail=%u wake_last_us=%llu off_pending_ms=%llu shadow=%u denied=%u job_faults=%u fault=%u g3d_mc=%d\n",
		    up_ns / NSEC_PER_MSEC, phase, mode, busy_cnt, gpu_safe, tz_ok,
		    (pll & (PLL_ENABLE | PLL_STABLE)) == (PLL_ENABLE | PLL_STABLE) ? 1u : 0u,
		    (mux & MUX_SEL) ? 1u : 0u, clocks, mv, rail_probe, rail_good, rail_mv, ctrl,
		    off_count, off_total_ns/NSEC_PER_MSEC, wakes, wake_failed, wake_us, off_ns / NSEC_PER_MSEC,
		    shadow, denied, jobs, fault, temp_mc);
		/* Same monotonic uptime as P4/GL; this still executes with
		 * no GPU jobs while the display is off. */
		if (__atomic_load_n(&p5_mode,__ATOMIC_RELAXED))
			p5_monitor_snapshot(now,up_ns,status,pll,mux,awake_state != 0);
		if (__atomic_load_n(&p7_gpu_mode,__ATOMIC_RELAXED))
			p7_gpu_store(now,up_ns,status,pll,mux,awake_state!=0);
		if (__atomic_load_n(&p68_mode,__ATOMIC_RELAXED)) {
			const uint64_t jf=__atomic_load_n(&g3d_last_busy,__ATOMIC_RELAXED);
			const uint64_t ff=__atomic_load_n(&gov_frame_last,__ATOMIC_RELAXED);
			const uint64_t job_age=jf?g3d_lab_age_ms(now,jf):0;
			const uint64_t frame_age=ff?g3d_lab_age_ms(now,ff):0;
			const uint32_t quiet=(jf && ff && job_age>=10000 && frame_age>=10000)?1u:0u;
			const uint32_t domain=(status&G3D_ON)==G3D_ON?1u:0u;
			say("p68-g3d: up=%llums phase=%u domain=%u pll=%u mux=%u awake_hint=%u clock_hint_mhz=%u job_valid=%u frame_valid=%u job_age_ms=%llu frame_age_ms=%llu quiet10s=%u could_shadow=%u actual_off=%u off_count=%u wakes=%u wake_fail=%u fault=%u source=PMU_5s_readback_not_watts_or_true_idle_residency\n",
			    up_ns/NSEC_PER_MSEC,phase,domain,
			    (pll&(PLL_ENABLE|PLL_STABLE))==(PLL_ENABLE|PLL_STABLE)?1u:0u,
			    (mux&MUX_SEL)?1u:0u,awake_state,clocks,jf?1u:0u,ff?1u:0u,
			    job_age,frame_age,quiet,quiet&&domain,
			    !domain,off_count,wakes,wake_failed,fault);
		}
	}
}

static void
g3d_lab_monitor_start(void)
{
	thread_t thread;
	if (__atomic_exchange_n(&g3d_lab_monitor_started, 1u, __ATOMIC_ACQ_REL) != 0)
		return;
	if (kernel_thread_start(g3d_lab_mon, NULL, &thread) == KERN_SUCCESS) {
		thread_set_thread_name(thread, "G3D power lab read-only probe");
		thread_deallocate(thread);
	} else {
		__atomic_store_n(&g3d_lab_monitor_started, 0u, __ATOMIC_RELEASE);
		say("g3d-lab: WARNING: five-second independent sampler could not start; sysctl stats still work\n");
	}
}

/* P6.8 uses the SAME existing G3D PMU sampler: read-only, no new
 * clock, G3D-MMIO read or power operation. Mode=0 keeps old behavior. */
static int
p68_mode_sysctl(__unused struct sysctl_oid *oidp,__unused void *arg1,
    __unused int arg2,struct sysctl_req *req)
{
    int v=(int)__atomic_load_n(&p68_mode,__ATOMIC_RELAXED);
    int err=sysctl_io_number(req,v,sizeof(v),&v,NULL);
    if(err || req->newptr==USER_ADDR_NULL)return err;
    if(v<0 || v>1)return EINVAL;
    __atomic_store_n(&p68_mode,(uint32_t)v,__ATOMIC_RELEASE);
    if(v)g3d_lab_monitor_start();
    say("p68: read-only G3D quiet PMU sampler=%d; gate unchanged\n",v);
    return 0;
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p68_mode,
    CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,
    p68_mode_sysctl,"I","P6.8 independent G3D 5s quiet vs PMU/PLL monitor, 0 off 1 on");

/* 0 baseline / 1 shadow / 2 real gate after long no-frame quiet /
 * 3 real gate only when >=1.5s no-frame/no-job, with wake cooldown.
 * Mode 2 is for actual screen-OFF tests; frame inactivity is a proxy,
 * not a verified hardware panel power-state signal. */
static int
g3d_lab_gate_sysctl(__unused struct sysctl_oid *oidp, __unused void *arg1,
    __unused int arg2, struct sysctl_req *req)
{
	int value = (int)g3d_lab_gate;
	int err = sysctl_io_number(req, value, sizeof(value), &value, NULL);
	if (err != 0 || req->newptr == USER_ADDR_NULL)
		return err;
	if (value < 0 || value > 3)
		return EINVAL;
	if (reached < STEPS || !mali_gov || !gov_on)
		return ENODEV;
	lck_mtx_lock(&mali_job_lock);
	if (value >= 2 && (g3d_lab_fault || g3d_lab_mv != 950 || gov_dvs ||
	    !g3d_lab_gpu_safe || !g3d_lab_protection_ok)) {
		lck_mtx_unlock(&mali_job_lock);
		return EBUSY;
	}
	if (value <= 1 && g3d_awake && (!g3d_lab_gpu_safe || !g3d_lab_protection_ok)) {
		lck_mtx_unlock(&mali_job_lock);
		say("g3d-lab: GPU needs verified g3d_lab_reset=1 before gate mode changes\n");
		return EIO;
	}
	if (value <= 1 && !g3d_awake && !g3d_power_on()) {
		lck_mtx_unlock(&mali_job_lock);
		return EIO;          /* do NOT clear g3d_sleep while GPU is OFF */
	}
	p9_auto_gpu = 0; /* explicit manual Lab gate overrides P9 auto */
	g3d_lab_gate = (uint32_t)value;
	g3d_sleep = value >= 2 ? 1u : 0u;
	lck_mtx_unlock(&mali_job_lock);
	g3d_lab_monitor_start();
	lck_mtx_lock(&mali_queue_lock);
	wakeup((caddr_t)&mali_queue_count);
	lck_mtx_unlock(&mali_queue_lock);
	say("g3d-lab: gate mode=%d: 0=alwaysON 1=shadow 2=screen-off-quiet 3=long-quiet, idle=%ums, voltage=%umV\n",
	    value, g3d_lab_idle_ms, g3d_lab_mv);
	return 0;
}
SYSCTL_PROC(_kern_hakios, OID_AUTO, g3d_lab_gate,
    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, NULL, 0,
    g3d_lab_gate_sysctl, "I", "G3D Lab gating: 0=OFF, 1=shadow, 2=screen-off quiet, 3=long quiet");

/* P9: promote the physically verified P7 OFF/ON path to an opt-in
 * screen-OFF policy. Always require the real display request *as well as*
 * ten seconds without GPU work or a new displayed frame.
 * No PMIC voltage changes, no new G3D reset implementation.
 * Explicit legacy G3D sysctls take precedence, so P8 cleanup still works.
 */
static int
p9_auto_gpu_sysctl(__unused struct sysctl_oid *oidp, __unused void *arg1,
    __unused int arg2, struct sysctl_req *req)
{
    int value = (int)__atomic_load_n(&p9_auto_gpu, __ATOMIC_RELAXED);
    int err = sysctl_io_number(req, value, sizeof(value), &value, NULL);
    if (err || req->newptr == USER_ADDR_NULL) return err;
    if (value < 0 || value > 1) return EINVAL;
    if (reached < STEPS || !mali_gov || !gov_on || !pmu || !cmu_g3d)
        return ENODEV;
    lck_mtx_lock(&mali_job_lock);
    if (value == (int)p9_auto_gpu) {
        lck_mtx_unlock(&mali_job_lock);
        return 0;
    }
    if (value) {
        /* Refuse to steal the GPU while another experiment owns it. */
        if (g3d_sleep || g3d_lab_gate || g3d_lab_fault || gov_dvs ||
            g3d_lab_mv != 950 || !g3d_awake || !g3d_lab_gpu_safe ||
            !g3d_lab_protection_ok) {
            lck_mtx_unlock(&mali_job_lock);
            return EBUSY;
        }
        g3d_lab_idle_ms = 10000;
        g3d_sleep = 1;
        g3d_lab_gate = 2;
        p9_auto_gpu = 1;
    } else {
        /* May be physically OFF. Restore BEFORE clearing the sleep gate:
         * otherwise the next submit could use inaccessible GPU MMIO. */
        if (!g3d_awake && !g3d_power_on()) {
            lck_mtx_unlock(&mali_job_lock);
            say("p9-gpu: disable REFUSED GPU wake failed; fault held\n");
            return EIO;
        }
        if (!g3d_lab_gpu_safe || !g3d_lab_protection_ok) {
            lck_mtx_unlock(&mali_job_lock);
            return EIO;
        }
        g3d_sleep = 0;
        g3d_lab_gate = 0;
        p9_auto_gpu = 0;
    }
    lck_mtx_unlock(&mali_job_lock);
    if (value) g3d_lab_monitor_start();
    lck_mtx_lock(&mali_queue_lock);
    wakeup((caddr_t)&mali_queue_count);
    lck_mtx_unlock(&mali_queue_lock);
    say("p9-gpu: auto=%d screen_off_required=1 idle_ms=10000 "
        "queue_empty_required=1 voltage_write=0 CPU_OFF=0\n", value);
    return 0;
}
SYSCTL_PROC(_kern_hakios, OID_AUTO, p9_auto_gpu,
    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, NULL, 0,
    p9_auto_gpu_sysctl, "I", "P9 screen-off automatic physical G3D gate, 0=stock 1=auto");

static int
g3d_lab_idle_sysctl(__unused struct sysctl_oid *oidp, __unused void *arg1,
    __unused int arg2, struct sysctl_req *req)
{
	int value = (int)g3d_lab_idle_ms;
	int err = sysctl_io_number(req, value, sizeof(value), &value, NULL);
	if (err != 0 || req->newptr == USER_ADDR_NULL)
		return err;
	if (value < 500 || value > 60000)
		return EINVAL;
	/* Mode 3 has an extra minimum 1500ms even if sysctl is lower. */
	g3d_lab_idle_ms = (uint32_t)value;
	say("g3d-lab: requested idle grace=%dms (mode3 minimum is 1500ms)\n", value);
	return 0;
}
SYSCTL_PROC(_kern_hakios, OID_AUTO, g3d_lab_idle_ms,
    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, NULL, 0,
    g3d_lab_idle_sysctl, "I", "time without GPU jobs AND frames before gate; 500..60000 ms");

/* Conservative manual rail experiment: only three explicit values,
 * 25mV steps, serialized with every GPU job by mali_job_lock.
 * No live automatic DVS, PLL change, or lower-than-900mV rail allowed. */
static int
g3d_lab_mv_sysctl(__unused struct sysctl_oid *oidp, __unused void *arg1,
    __unused int arg2, struct sysctl_req *req)
{
	int value = (int)g3d_lab_mv;
	int err = sysctl_io_number(req, value, sizeof(value), &value, NULL);
	uint8_t actual = 0;
	if (err != 0 || req->newptr == USER_ADDR_NULL)
		return err;
	if (value < 0 || !s9_g3d_lab_mv_allowed((uint32_t)value))
		return EINVAL;
	if (reached < STEPS || !mali_acpm || !acpm_ok || !b6out_known || !mali_gov || !gov_on)
		return ENODEV;
	lck_mtx_lock(&mali_job_lock);
	if (!g3d_awake || !g3d_lab_gpu_safe || !g3d_lab_protection_ok ||
	    g3d_sleep || g3d_lab_gate >= 2 ||
	    (rd(pmu, PMU_G3D_STATUS) & G3D_ON) != G3D_ON || gov_dvs ||
	    (g3d_lab_fault && value != 950)) {
		lck_mtx_unlock(&mali_job_lock);
		return EBUSY;
	}
	if (value == (int)g3d_lab_mv) {
		lck_mtx_unlock(&mali_job_lock);
		return 0;
	}
	if (value != 950 && (value > (int)g3d_lab_mv + 25 ||
	    value < (int)g3d_lab_mv - 25)) {
		lck_mtx_unlock(&mali_job_lock);
		return EINVAL;       /* test 950 -> 925 -> 900; no 50mV jumps */
	}
	if (hakios_acpm_pmic_read(HAKIOS_ACPM_PMIC_PMIC, B6OUT, &actual) != 0 ||
	    actual != s9_g3d_lab_vsel(g3d_lab_mv)) {
		g3d_lab_trip(5);
		lck_mtx_unlock(&mali_job_lock);
		say("g3d-lab: unexpected BUCK6 setting; voltage change REFUSED\n");
		return EIO;
	}
	if (!rail_set((uint8_t)s9_g3d_lab_vsel((uint32_t)value))) {
		bool restored;
		g3d_lab_trip(5);
		restored = rail_set(B6OUT_950MV); /* failure is NOT treated as a successful recovery */
		if (restored)
			g3d_lab_mv = 950;
		lck_mtx_unlock(&mali_job_lock);
		say("g3d-lab: PMIC write/readback failed; restore 950mV %s; no further undervolt until reset\n",
		    restored ? "VERIFIED" : "FAILED: reboot/rollback needed");
		return EIO;
	}
	g3d_lab_mv = (uint32_t)value;
	lck_mtx_unlock(&mali_job_lock);
	g3d_lab_monitor_start();
	say("g3d-lab: BUCK6 readback PASS, target=%dmV; no GPU clock or thermal changes\n", value);
	return 0;
}
SYSCTL_PROC(_kern_hakios, OID_AUTO, g3d_lab_mv,
    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, NULL, 0,
    g3d_lab_mv_sysctl, "I", "BUCK6 manual test 950/925/900mV, max 25mV per transition");

/* PMIC reads only, max once per 30sec; separate from user P3 p3rail. */
static int
g3d_lab_rail_probe_sysctl(__unused struct sysctl_oid *oidp, __unused void *arg1,
    __unused int arg2, struct sysctl_req *req)
{
	int value = (int)g3d_lab_rail_probe;
	int err = sysctl_io_number(req, value, sizeof(value), &value, NULL);
	if (err != 0 || req->newptr == USER_ADDR_NULL)
		return err;
	if (value < 0 || value > 1)
		return EINVAL;
	g3d_lab_rail_probe = (uint32_t)value;
	if (value)
		g3d_lab_monitor_start();
	say("g3d-lab: 30-second read-only PMIC sampler=%u\n", g3d_lab_rail_probe);
	return 0;
}
SYSCTL_PROC(_kern_hakios, OID_AUTO, g3d_lab_rail_probe,
    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, NULL, 0,
    g3d_lab_rail_probe_sysctl, "I", "read BUCK6 B6CTRL/B6OUT without writes every 30 seconds");

/* Live read-only summary also works when the full G3D domain is OFF. */
static int
g3d_lab_stats_sysctl(__unused struct sysctl_oid *oidp, __unused void *arg1,
    __unused int arg2, struct sysctl_req *req)
{
	char line[512];
	uint64_t active_off_ns = 0, now, total_off_ns;
	uint64_t wake_last, wake_max, wake_avg;
	uint32_t pmu_on = 0, pll_on = 0, live_rail_mv, gpu_safe, tz_ok;
	lck_mtx_lock(&mali_job_lock);
	now = mach_absolute_time();
	if (pmu != 0)
		pmu_on = (rd(pmu, PMU_G3D_STATUS) & G3D_ON) == G3D_ON;
	if (cmu_g3d != 0)
		pll_on = (rd(cmu_g3d, PLL_CON0) & (PLL_ENABLE|PLL_STABLE)) ==
		    (PLL_ENABLE|PLL_STABLE);
	if (!g3d_awake && g3d_off_at != 0)
		absolutetime_to_nanoseconds(now >= g3d_off_at ? now-g3d_off_at : 0, &active_off_ns);
	total_off_ns = g3d_lab_off_residency_ns + active_off_ns;
	wake_last = g3d_lab_last_wake_us;
	wake_max = g3d_lab_max_wake_us;
	wake_avg = g3d_lab_wake_count ? g3d_lab_total_wake_us/g3d_lab_wake_count : 0;
	live_rail_mv = g3d_lab_rail_valid ? s9_g3d_lab_mv_from_vsel(g3d_lab_rail_vsel) : 0;
	gpu_safe = g3d_lab_gpu_safe ? 1u : 0u;
	tz_ok = g3d_lab_protection_ok ? 1u : 0u;
	snprintf(line, sizeof(line),
	    "phase=%u mode=%u grace_ms=%u requested_mv=%u domain=%u pll=%u gpu_safe=%u tz_ok=%u off_count=%u off_total_ms=%llu wake_count=%u wake_fail=%u wake_last_us=%llu wake_max_us=%llu wake_avg_us=%llu shadow=%u denied=%u job_faults=%u fault=%u rail_valid=%u rail_last_mv=%u",
	    g3d_lab_phase, g3d_lab_gate, g3d_lab_idle_ms, g3d_lab_mv, pmu_on, pll_on, gpu_safe, tz_ok,
	    g3d_lab_off_count, total_off_ns/NSEC_PER_MSEC,
	    g3d_lab_wake_count, g3d_lab_wake_failed, wake_last, wake_max, wake_avg,
	    g3d_lab_shadow_count, g3d_lab_denied_count, g3d_lab_job_faults,
	    g3d_lab_fault, g3d_lab_rail_valid?1u:0u, live_rail_mv);
	lck_mtx_unlock(&mali_job_lock);
	return SYSCTL_OUT(req, line, strlen(line)+1);
}
SYSCTL_PROC(_kern_hakios, OID_AUTO, g3d_lab_stats,
    CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, NULL, 0,
    g3d_lab_stats_sysctl, "A", "read-only PMU/rail/ON-OFF residency/wake/error statistics");

/* A phase tag is pure instrumentation: lets one serial log contain
 * multiple voltage/gating A/B steps with unambiguous boundaries. */
static int
g3d_lab_phase_sysctl(__unused struct sysctl_oid *oidp, __unused void *arg1,
    __unused int arg2, struct sysctl_req *req)
{
	int value = (int)g3d_lab_phase;
	int err = sysctl_io_number(req, value, sizeof(value), &value, NULL);
	uint64_t uptime_ns;
	if (err != 0 || req->newptr == USER_ADDR_NULL)
		return err;
	if (value < 0 || value > 99)
		return EINVAL;
	g3d_lab_phase = (uint32_t)value;
	g3d_lab_monitor_start();
	absolutetime_to_nanoseconds(mach_absolute_time(), &uptime_ns);
	say("g3d-lab: PHASE=%d up=%llums: no hardware setting changed by phase tag\n",
	    value, uptime_ns / NSEC_PER_MSEC);
	return 0;
}
SYSCTL_PROC(_kern_hakios, OID_AUTO, g3d_lab_phase,
    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, NULL, 0,
    g3d_lab_phase_sysctl, "I", "0..99 test-phase identifier, read-only effect on GPU hardware");

/* One sysctl to roll back to 950mV, restore domain, stop gating.
 * On any incomplete restore the fault remains latched, never claim
 * success or silently let job submission touch a powered-off GPU. */
static int
g3d_lab_reset_sysctl(__unused struct sysctl_oid *oidp, __unused void *arg1,
    __unused int arg2, struct sysctl_req *req)
{
	int value = 0;
	const char *which = NULL;
	int err = sysctl_io_number(req, value, sizeof(value), &value, NULL);
	if (err != 0 || req->newptr == USER_ADDR_NULL)
		return err;
	if (value != 1)
		return EINVAL;
	lck_mtx_lock(&mali_job_lock);
	if (!g3d_awake && !g3d_power_on()) {
		lck_mtx_unlock(&mali_job_lock);
		say("g3d-lab: RESET failed: power recovery incomplete, reboot/rollback needed\n");
		return EIO;
	}
	/* Restore/verify full voltage BEFORE resetting cores if the live
	 * GPU was left unsafe by a previous PMIC/SMC fault. */
	if (!acpm_ok || !rail_set(B6OUT_950MV)) {
		g3d_lab_trip(5);
		lck_mtx_unlock(&mali_job_lock);
		say("g3d-lab: RESET failed: BUCK6 not verified at 950mV, reboot/rollback needed\n");
		return EIO;
	}
	if (g3d_awake && !g3d_lab_protection_ok) {
		/* PMU can be ON even when the DTZPC restore failed. The SMC
		 * and bus-divider restore MUST pass before GPU reset/MMIO. */
		powered_here = true;
		if (!step_dtzpc()) {
			g3d_lab_trip(3);
			lck_mtx_unlock(&mali_job_lock);
			say("g3d-lab: RESET refused: DTZPC restore FAILED, no GPU MMIO permitted\n");
			return EIO;
		}
		g3d_lab_protection_ok = true;
	}
	if (g3d_awake && !g3d_lab_gpu_safe) {
		if (!gpu_reset() || !gpu_cores_on(&which)) {
			g3d_lab_trip(3);
			lck_mtx_unlock(&mali_job_lock);
			say("g3d-lab: RESET failed: GPU cores did not recover; reboot required\n");
			return EIO;
		}
		lck_mtx_lock(&mali_lock);
		as0_update(pt_transtab, pt_transcfg());
		lck_mtx_unlock(&mali_lock);
		gpu_answered = true;
		g3d_lab_gpu_safe = true;
		if (mali_irq)
			(void)mali_irq_arm();
	}
	if ((rd(pmu, PMU_G3D_STATUS) & G3D_ON) != G3D_ON ||
	    !g3d_lab_gpu_safe || !g3d_lab_protection_ok) {
		g3d_lab_trip(3);
		lck_mtx_unlock(&mali_job_lock);
		return EIO;
	}
	g3d_lab_mv = 950;
	g3d_lab_gate = 0;
	p9_auto_gpu = 0;
	g3d_lab_fault = 0;
	g3d_sleep = 0;
	lck_mtx_unlock(&mali_job_lock);
	lck_mtx_lock(&mali_queue_lock);
	wakeup((caddr_t)&mali_queue_count);
	lck_mtx_unlock(&mali_queue_lock);
	say("g3d-lab: RESET VERIFIED: G3D ON, BUCK6 950mV, gate OFF, fault cleared\n");
	return 0;
}
SYSCTL_PROC(_kern_hakios, OID_AUTO, g3d_lab_reset,
    CTLTYPE_INT | CTLFLAG_WR | CTLFLAG_LOCKED | CTLFLAG_KERN, NULL, 0,
    g3d_lab_reset_sysctl, "I", "write 1: verify G3D ON and BUCK6 950mV, stop all Lab experiments");

/* Called AFTER a completed/failed job, outside the GPU submit lock. */
static void
g3d_lab_restore_after_job_error(void)
{
	if (g3d_lab_mv == 950)
		return;
	lck_mtx_lock(&mali_job_lock);
	g3d_lab_job_faults++;
	g3d_lab_trip(6);
	if (g3d_awake && acpm_ok && rail_set(B6OUT_950MV)) {
		g3d_lab_mv = 950;
		say("g3d-lab: GPU job error during undervolt; BUCK6 restored to 950mV, further changes latched OFF\n");
	} else {
		g3d_lab_gpu_safe = false; /* never dispatch another job with unknown rail voltage */
		say("g3d-lab: GPU job error: BUCK6 restore failed, GPU dispatch blocked until recovery\n");
	}
	lck_mtx_unlock(&mali_job_lock);
}
