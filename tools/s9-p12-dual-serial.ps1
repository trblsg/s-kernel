#requires -Version 5.1
<#
  P12 integrated S9 thermal A/B and natural deep-idle PMU/PLL observer.
  Requires s9-screen-lifecycle-serial.ps1 in same Windows directory.
  No forced screen ON; no CPU_OFF/C2/CPD/SICD, panel DCS, or GPU policy writes.
#>
[CmdletBinding()]
param(
    [ValidatePattern('^COM[0-9]+$')][string]$Port='COM125',
    [ValidateRange(12,90)][int]$ActiveWindowSeconds=25,
    [ValidateRange(50,900)][int]$IdleWindowSeconds=120,
    [ValidateRange(10,100)][int]$OffStableSeconds=25,
    [ValidateRange(75,85)][int]$BigStopC=82,
    [ValidateRange(70,80)][int]$LittleStopC=75,
    [ValidateRange(70,85)][int]$G3DStopC=77,
    [ValidateRange(43,55)][int]$BatteryStopC=47,
    [switch]$WfiProbe,
    [switch]$InteractiveAB,
    [switch]$WakeCheck,
    [switch]$LegacyFirmware,
    [switch]$Execute,
    [switch]$LibraryOnly
)
Set-StrictMode -Version 2
$ErrorActionPreference='Stop'
$script:P12ExecuteRequested=$Execute
$script:P12LibraryRequested=$LibraryOnly
$lib=Join-Path $PSScriptRoot 's9-screen-lifecycle-serial.ps1'
if(-not(Test-Path -LiteralPath $lib)){throw 'Missing P11 companion: s9-screen-lifecycle-serial.ps1'}
. $lib -LibraryOnly -Port $Port -OffStableSeconds $OffStableSeconds -BigStopC $BigStopC -LittleStopC $LittleStopC -G3DStopC $G3DStopC -BatteryStopC $BatteryStopC
# Dot-sourced P11 declares its own -Execute and -LibraryOnly params.
# Preserve the user's P12 flags before importing that library.
$Execute=$script:P12ExecuteRequested
$LibraryOnly=$script:P12LibraryRequested
$script:P12Command=''
$script:P12Echo=$false
$script:P12Reply=$null
$script:P12Refusal=$null
$script:P12ReadPaceMs=0
$script:P12ReadAttempts=1
$script:P12Journal=''
$script:P1272HostEnabled=$false
$script:P1273HostEnabled=$false
$script:P1274HostEnabled=$false
$script:P1274ProfileAllowed=$false
$script:P12752IdleAllowed=$false
$script:P12753HostEnabled=$false
$script:P12754HostEnabled=$false
$script:P1277HostEnabled=$false
$script:P1277SuiteAllowed=$false
$script:P1278HostEnabled=$false
$script:P1278SuiteAllowed=$false
$script:P12753ArmAllowed=$false
$script:P127544WarmAllowed=$false
$script:P127544CompositeAllowed=$false
$script:P127545LongWaitAllowed=$false
$script:P12752ProfileAllowed=$false
$script:P12751NextAllowed=$false
$script:P1273GateAllowed=$false
$script:P127542GpuAllowed=$false
$script:P127545PanelAllowed=$false
$script:P127545MfcAllowed=$false
$script:P127545MfcProbeAllowed=$false
$script:P127545CamAllowed=$false
$script:P127545CamProbeAllowed=$false
$script:P127545DispAllowed=$false
$script:P127545DispProbeAllowed=$false
$script:P127543AssistAllowed=$false
$script:P1273CombinedAllowed=$false
$script:P1272ProbeLines=New-Object 'System.Collections.Generic.List[string]'
$script:P12PreflightVerified=$false
$script:P12CpuDirty=$false
$script:P12WfiDirty=$false
$script:P12HiStopConfirmed=$false
$script:P12HiDirty=$false
# Only the P12.3 wrapper opts in; P12/P12.2 retain the old allowlist.
$script:P123HostEnabled=$false
$script:P124HostEnabled=$false
$script:P125HostEnabled=$false
$script:P126HostEnabled=$false
$script:P127HostEnabled=$false
$script:P124ClockAllowed=$false
$script:P123ClockAllowed=$false
$script:P123Strict=$false
$script:LifeHostOnly=$false
$script:P12HiAbortSeen=$false
$script:P12HiGateStopSeen=$false
$script:P12HiUnexpectedPhase=$false
$script:P12GpuOffBaseline=0L
$script:P12GpuWakeBaseline=0L
# P12.1 conservative start gate, below immutable hard thermal abort thresholds.
$script:P12StartBigC=70
$script:P12StartLittleC=68
$script:P12StartGpuC=68
$script:P12Phase='PREFLIGHT'
$script:P12Results=New-Object 'System.Collections.Generic.List[string]'
$script:P12Phases=New-Object 'System.Collections.Generic.List[object]'
$script:P12CA=@{}
# P12.7.1: accept a complete, typed USB reply following a real command
# echo even if another producer prepended text on the same physical line.
# Never strip arbitrary suffixes or accept a partial numeric/structured value.
function P12-ReplyShape([string]$Name,[string]$Reply,[bool]$Mixed){
    $hex='[0-9a-fA-F]{1,16}'
    $dec='[0-9]{1,20}'
    $fields=@{
        p126_status='mode reason haptic wifi_worker gnss touch touch_leeway shub lease_s watchdog_ms'
        p126_diag='bt_idle bt_busy bt_rx bt_tty_tx_dequeued bt_uart_error abox_quiet abox_busy shub_quiet shub_busy hal_speaker'
        p127_status='mode reason lease_s little_request little_cur ufs_safe ufs_busy floor_hits floor_bypass'
        p127_metrics='usb_stock usb_8ms usb_linkdown usb_bypass usb_other abox_stop abox_resume abox_idle_wait abox_fault psr_mask psr_unmask psr_bypass psr_fault'
        p127_wifi='attach irq watchdog fallback detach'
        p127_bt='attach irq watchdog fallback detach tx_wake'
        p127_ticker='wait due wake'
        p127_fg_seq='seq slots'
    }
    if($Name -match '^(?:p125_(?:mode|select)|p126_mode|p127_mode|p1272_probe_request|p1273_trace_(?:mode|select)|p1273_gpu_select|p1273_gov_select|p1273_cap_mode|p1273_offline_s|p12741_profile|p1275_profile|p12752_profile|p12752_idle_mode|p12752_idle_select|p12753_arm|p12753_select|p1274_select|p12751_stage_select|p1273_gate_(?:begin|review|ack)|p6_usb_(?:ticks|connected_ticks)|p69_usb_disconnected_ticks)$'){
        $n=[uint64]0
        return ($Reply -cmatch "^$dec$" -and [uint64]::TryParse($Reply,[ref]$n))
    }
    if($fields.ContainsKey($Name)){
        $pattern='^version=1'
        foreach($k in $fields[$Name].Split(' ')){$pattern+=" $k=$dec"}
        return ($Reply -cmatch ($pattern+'$'))
    }
    switch -Regex ($Name){
        '^p1272_gpu_caps$' {return ($Reply -cmatch "^version=1 ready=[01] mask=$hex hits=$dec off_hits=$dec param2=$dec off_param2=$dec reject=$dec denied=$dec live=[01]$")}
        '^p1272_probe_state$' {return ($Reply -cmatch "^version=1 eligible=[01] display=-?[0-9]+ domain=$dec pll=[01] mux=[01] wake=$dec off=$dec wake_fail=$dec fault=$dec safe=[01] tz=[01]$")}
        '^p125_status$' {
            return ($Reply -cmatch "^version=1 active=[0-3] armed=[0-3] start=$dec end=$dec now=$dec hz=$dec reason=$dec down=$dec up=$dec display_on=$dec anchor=$hex slots=512 rows=2 counters=28 kinds=15 cumulative=1(?: page_lookup=scalar)?$")
        }
        '^p125_counts([0-7])$' {
            $cpu=$Matches[1];return ($Reply -cmatch "^version=1 cpu=$cpu counters=$hex(?:,$hex){27}$")
        }
        '^p125_coverage([0-7])$' {
            $cpu=$Matches[1];return ($Reply -cmatch "^version=1 cpu=$cpu pages=$hex(?:,$hex){3} mapdrop=$dec bins=$hex(?:,$hex){4} totals=$hex/$hex(?:,$hex/$hex){13}$")
        }
        '^p127_wfi([0-7])$' {
            $cpu=$Matches[1];return ($Reply -cmatch "^version=1 cpu=$cpu sgi=$hex ppi=$hex spi=$hex bins=$hex/$hex(?:,$hex/$hex){6}$")
        }
        '^p127_fg([0-9]|1[0-5])$' {
            $page=$Matches[1];return ($Reply -cmatch "^version=1 page=$page(?: r=$dec/$dec/-?$dec/-?$dec/$dec/[01]/$dec/$dec){0,4}$")
        }
        '^p12741_revision$' {return ($Reply -ceq '2')}
        '^p1275_revision$' {return ($Reply -ceq '1')}
        '^p12751_revision$' {return ($Reply -ceq '1')}
        '^p12752_revision$' {return ($Reply -ceq '1')}
        '^p12753_revision$' {return ($Reply -ceq '1')}
        '^p12754_revision$' {return ($Reply -cmatch '^[123456]$')}
        '^p1277_layer2$' {return ($Reply -cmatch '^[0-4]$')}
        '^p1278_revision$' {return ($Reply -ceq '8')}
        '^p1278_panel_mode$' {return ($Reply -cmatch '^[12]$')}
        '^p1278_panel_review$' {return ($Reply -cmatch '^[0-3]$')}
        '^p1278_panel_stats$' {return ($Reply -cmatch "^version=1 permission=[01] mode=[12] off_mode=[0-2] off_verified=[01] off_count=$dec replays=$dec clean=$dec reviewed=[0-3] reason=$dec first=$dec done=$dec diag=$dec raw=[0-9a-f]{2}/[0-9a-f]{2}/[0-9a-f]{2}/[0-9a-f]{2} id=[0-9a-f]{6} pending=$dec failed=[01]$")}
        '^p1278_irq_stats$' {return ($Reply -cmatch "^version=1 enabled=[01] attached=[01] waits=$dec late=$dec spins=$dec naps=$dec fallback=$dec arm_fail=$dec fault=$dec$")}
        '^p1278_active_stats$' {return ($Reply -cmatch "^version=1 active=[01] boost_ms=$dec burst_ms=$dec lit_touch=$dec big_touch=$dec big_frame=$dec g2_request=[01] g2_effective=[01]$")}
        '^p1277_revision$' {return ($Reply -ceq '7')}
        '^p1277_auto_stats$' {return ($Reply -cmatch "^version=1 permission=[01] enabled=[01] state=[0-5] stage=[0-5] samples=$dec retries=$dec abox_driver_ok=[01] fault=$dec last_uv=$dec last_mhz=$dec last_reason=$dec own_lease=$dec live_mode=$dec ufs_safe=[01] interactive_qualification=0$")}
        '^p1277_layer2_stats$' {return ($Reply -cmatch "^version=1 request=[0-4] module=[0-4] state=[0-2] result=[0-4] samples=$dec restored=[01] power_qualified=0$")}
        '^p1277_burst_stats$' {return ($Reply -cmatch "^version=1 request=[01] owned=[01] trace_state=[0-5] observed_trigger=$dec trace_generation=$dec threshold=150 dwell_ms=250 pretrigger=0$")}
        '^p1277_panel_stats$' {return ($Reply -cmatch "^version=1 permission=[01] request=[01] persistent=[01] failed=[01] asleep=[01] pending=$dec sleep=$dec latch29=$dec fallback=$dec fault=$dec sleep_verified=[01] wake_uncertain=[01]$")}
        '^p1277_gpu_dvs_stats$' {return ($Reply -cmatch "^version=1 permission=[01] active=[01] failed=[01] high=$dec rebase=$dec last_configured_vsel=$dec fault=$dec$")}
        '^p1277_psci_stats$' {return ($Reply -cmatch '^version=1 permission=[01] done=[01] raw_version=0x[0-9a-f]{8} raw_features=0x[0-9a-f]{8} signed_features=-?[0-9]{1,10} osi=[01] extended=[01] conduit=SMC_EL2_possible suspend_called=0$')}
        '^(mali_irq|p127542_g2|p127545_panel|p1277_auto_idle|p1277_panel|p1277_active|p1277_gpu_dvs|p1277_g2|p1277_burst_trace)$' {return ($Reply -cmatch '^[01]$')}
        '^p127545_panel_stats$' {return ($Reply -cmatch "^version=1 mode=[01] gen=$dec pending=$dec sleep=$dec latch29=$dec fallback=$dec fault=$dec$")}
        '^p127545_disp_review$' {return ($Reply -cmatch '^(0|200000|134000)$')}
        '^p127545_disp_stats$' {return ($Reply -cmatch "^version=1 enabled=[01] transport_ok=[01] closed=[01] active=[01] target=$dec seen=$dec restored=$dec qual=[0-3] pending=$dec attempts=$dec sets=$dec restores=$dec preempt=$dec fault=$dec reviews=$dec live_n=$dec live_min=$dec live_max=$dec start=$dec end=$dec$")}
        '^p127545_cam_stats$' {return ($Reply -cmatch "^version=1 enabled=[01] idle=[01] good=[01] pending=[01] qualified=[01] active=[01] attempts=$dec restores=$dec preempt=$dec fault=$dec frames=$dec leaf=$dec saved=$dec auto=$dec dbg0=$dec dbg1=$dec dbg2=$dec start=$dec end=$dec$")}
        '^p127545_mfc_stats$' {return ($Reply -cmatch "^version=1 ready=[01] qualified=[01] active=[01] attempts=$dec restores=$dec preempt=$dec fault=$dec crc=$dec saved=$dec auto=$dec dbg0=$dec dbg1=$dec dbg2=$dec start=$dec end=$dec irq_mode=[01] irq_blocked=$dec irq_review=$dec$")}
        '^p127545_hci$' {return ($Reply -cmatch "^version=1 epoch=$dec owner=$dec unit=$dec valid=[01] v_tx=$dec v_complete=$dec v_irq=$dec v_poll=$dec f_tx=$dec f_complete=$dec f_irq=$dec f_poll=$dec active_probe=0$")}
        '^p127545_ect_(mif|int)$' {
            $table=if($Name -eq 'p127545_ect_mif'){0}else{1}
            if($Reply -cmatch "^version=1 valid=0 table=$table size=0 fingerprint=0 count=0 live_verified=0 write_allowed=0 rows=-$"){return $true}
            if($Reply -cnotmatch "^version=1 valid=1 table=$table size=([0-9]{1,10}) fingerprint=([0-9]{1,10}) count=([0-9]{1,2}) live_verified=0 write_allowed=0 rows=([0-9:,]+)$"){return $false}
            $size=[uint64]$Matches[1];$hash=[uint64]$Matches[2];$n=[uint64]$Matches[3];$rows=$Matches[4].Split(',')
            if($size -lt 16 -or $size -gt 204800 -or $hash -gt 4294967295 -or $n -lt 1 -or $n -gt 32 -or $rows.Count -ne $n){return $false}
            $seen=@{}
            foreach($row in $rows){
                if($row -cnotmatch '^(\d{1,7}):(\d{1,7})$'){return $false}
                $master=[uint32]$Matches[1];$slave=[uint32]$Matches[2]
                if($master -lt 1000 -or $master -gt 4000000 -or $slave -gt 4000000 -or ($slave -ne 0 -and $slave -lt 1000) -or $seen.ContainsKey($master)){return $false}
                $seen[$master]=$true
            }
            return $true
        }
        '^p127545_int_bounds$' {return ($Reply -cmatch "^version=1 ect=[01] fv_stable=[01] complete=[01] mif_floor=$dec fsys0_floor=$dec stage_mask=(?:[0-9]|[1-5][0-9]|6[0-3]) live_verified=0 write_allowed=0$")}
        '^p127545_cmu$' {return ($Reply -cmatch "^version=1 stable=[01] busc_khz=$dec bus1_khz=$dec disp_khz=$dec mif_pll_khz=$dec ddrphy2x_khz=$dec mif_mux=[01](?: fsys0_khz=$dec)? int_opp_verified=0 mif_opp_verified=0 write_allowed=0$")}
        '^p127544_window$' {return ($Reply -cmatch "^version=1 valid=[01] arm=$dec start=$dec end=$dec irq27=$dec wfi_n=$dec wfi_ticks=$dec ge_n=$dec ge_ticks=$dec$")}
        '^p127543_wait$' {return ($Reply -cmatch "^version=1 deadline=$dec plateau=[01]$")}
        '^p127543_rail$' {return ($Reply -cmatch "^version=1 generation=$dec count=[0-2] attempts=[0-4] uv0=$dec uv1=$dec$")}
        '^p127543_assist$' {return ($Reply -cmatch '^(0|1|5|17|33|49|65|129|257|513)$')}
        '^p127542_audio$' {return ($Reply -cmatch '^(0|[1-9]\d{0,7})$')}
        '^p127542_audio_result$' {return ($Reply -cmatch '^(0|-?[1-9]\d{0,7})$')}
        '^p1231_ufs_stats$' {return ($Reply -cmatch "^version=1 nufs=[01] switched=[01] step=$dec selftest=-?$dec fences=-?$dec dead=[01] loader_core_taken=[01] loader_gone=[01] floor_safe=[01] native_reads=$dec loader_reads=$dec read_errors=$dec writes=$dec write_errors=$dec refused=$dec write_busy=[01] native_inflight=$dec native_read_calls=$dec native_read_wall_us=$dec native_write_calls=$dec native_write_wall_us=$dec write_refused=$dec$")}
        '^p127541_stats$' {
            $shape="^version=1 skips=$dec"
            for($i=0;$i -lt 12;$i++){$bit=1 -shl $i;$shape+=" n$bit=$dec t$bit=$dec"}
            return ($Reply -cmatch ($shape+"(?: n4096=$dec t4096=$dec)?$"))
        }
        '^p12754_rate$' {return ($Reply -cmatch "^version=1 fresh=[01] cur=$dec meas=$dec$")}
        '^p12754_wait$' {return ($Reply -cmatch "^version=1 enabled=[01] preflight=$dec wait=$dec seen=$dec quiet_ticks=$dec input_seen=[01] fingers=$dec$")}
        '^p12753_guard$' {return ($Reply -cmatch "^version=1 enabled=[01] at=$dec mask=$dec big=-?$dec lit=-?$dec gpu=-?$dec bat=-?$dec age=$dec slope=-?$dec bcap=$dec lcap=$dec peak_big=-?$dec peak_lit=-?$dec changes=$dec$")}
        '^p12753_status$' {return ($Reply -cmatch "^version=1 state=[0-3] request=$dec arm=$dec down=$dec start=$dec end=$dec until=$dec up=$dec now=$dec hz=$dec reason=$dec mask=$dec count=$dec drops=$dec bus=$dec stop_big=-?$dec stop_lit=-?$dec stop_gpu=-?$dec stop_bat=-?$dec stop_age=$dec policy=$dec(?: warm=[01])?$")}
        '^p12753_page$' {return ($Reply -cmatch "^version=1 select=$dec phase=$dec at=$dec kind=[12] index=$dec raw=$dec valid=[01] errno=-?$dec latency=$dec big=-?$dec lit=-?$dec gpu=-?$dec bat=-?$dec age=$dec bs=$dec ls=$dec bkhz=$dec lkhz=$dec bmeas=$dec lmeas=$dec policy=$dec driver=$dec refresh=$dec g3d=$dec dpu=$dec isppre=$dec mfc=$dec aud=$dec mask=$dec opp_uv=$dec opp_valid=[01]$")}
        '^p12752_eligibility$' {return ($Reply -cmatch "^version=1 enabled=[01] conflict=[01] ocp=[01] ufs_safe=[01] ufs_busy=[01] quiet=[01] thermal=$dec cores=[0-4] online=$dec exit=$hex$")}
        '^p12752_idle_info$' {return ($Reply -cmatch "^version=1 mode=[012] phase=$dec count=$dec drops=$dec reason=$dec bus=$dec$")}
        '^p12752_idle_page$' {return ($Reply -cmatch "^version=1 select=$dec phase=$dec at=$dec kind=[12] index=$dec raw=$dec valid=[01] errno=-?$dec latency=$dec big=-?$dec lit=-?$dec gpu=-?$dec bat=-?$dec age=$dec bs=$dec ls=$dec bkhz=$dec lkhz=$dec bmeas=$dec lmeas=$dec policy=$dec driver=$dec refresh=$dec$")}
        '^p12751_boot_latch$' {return ($Reply -cmatch '^(?:-1|[0-7])$')}
        '^p12751_(?:next|ack)$' {return ($Reply -cmatch '^(?:-1|[0-3])$')}
        '^p12751_stage_info$' {return ($Reply -cmatch "^version=1 sealed=[01] count=$dec done=$dec drops=$dec$")}
        '^p12751_stage_focus$' {return ($Reply -cmatch '^version=1(?: s=[0-9]{1,3}){0,24}$')}
        '^p12751_stage_page$' {return ($Reply -cmatch "^version=1 select=$dec at=$dec end=$dec pid=$dec task=$dec tid=$dec stage=$dec cpu=$dec user=$dec sys=$dec minor=$dec major=$dec images0=$dec images1=$dec ok=[012] usage=[01]$")}
        '^p1275_load_state$' {return ($Reply -cmatch '^[0-2]$')}
        '^p1275_(?:mode|selected)$' {return ($Reply -cmatch '^(?:[0-9]|1[0-6])$')}
        '^p1275_focus$' {return ($Reply -cmatch '^version=1(?: s=[0-9]{4,5}){0,48}$')}
        '^p12741_info$' {return ($Reply -cmatch "^version=2 episodes=$dec episode_drops=$dec peak=-?$dec peak_select=$dec$")
        }
        '^p12741_guard$' {return ($Reply -cmatch "^version=2 armed=[01] at=$dec big=-?$dec lit=-?$dec gpu=-?$dec age=$dec battery=-?$dec fast=$dec lit_fast=$dec peak_big=-?$dec peak_lit=-?$dec changes=$dec last_change=$dec bcur=$dec lcur=$dec$")
        }
        '^p1274_metal_log_mode$' {return ($Reply -cmatch '^(?:-1|0|2)$')}
        '^p1274_restore$' {return ($Reply -cmatch "^version=1 state=[0-3] profile=[0-9] cores=[0-4] actual=$hex requested=$hex fast=$dec online=$dec exit=$hex display=-?[0-2]$")}
        '^p1274_status$' {
            return ($Reply -cmatch "^version=1 state=[012] profile=[0-9] now=$dec$" -or
                $Reply -cmatch "^version=1 state=3 profile=[1-9] start=$dec end=$dec planned=$dec reason=$dec rows=$dec fg=$dec events=$dec drops=$dec rate_events=$dec rate_drops=$dec bigmask=$hex litmask=$hex trigger_bits=$dec burst_start=$dec burst_end=$dec measure_ok=$dec measure_bad=$dec measure_skip=$dec usb_down=$dec usb_up=$dec$")
        }
        '^p1274_page$' {
            return ($Reply -cmatch "^version=1 select=$dec kind=1 at=$dec tmu_at=$dec measured_at=$dec direct=-?$dec peak=-?$dec lit=-?$dec gpu=-?$dec slope=-?$dec bp=$dec lp=$dec ba=$dec la=$dec bs=$dec ls=$dec bsp=$dec lsp=$dec bkhz=$dec lkhz=$dec bcap=$dec fast=$dec measured=$dec online=$dec recommended=$hex requested=$hex display=[012] profile=[0-9] lcap=$dec lfast=$dec$" -or
                $Reply -cmatch "^version=1 select=$dec kind=2 seq=$dec at=$dec ua=-?$dec avg=-?$dec uv=$dec external=[01] charger=$dec qh=$dec$" -or
                $Reply -cmatch "^version=1 select=$dec kind=3 at=$dec pid=$dec task=$dec tid=$dec event=[1-6] a=$dec b=$dec c=$dec d=$dec e=$dec f=$dec g=$dec$" -or
                $Reply -cmatch "^version=1 select=$dec kind=4 present=[01] bin=$dec at=$dec tmu_at=$dec bp=$dec lp=$dec ba=$dec la=$dec bs=$dec ls=$dec bsp=$dec lsp=$dec bkhz=$dec lkhz=$dec direct=-?$dec peak=-?$dec$" -or
                $Reply -cmatch "^version=1 select=$dec kind=5 cpu=[0-7] n=$dec ticks=$dec$" -or
                $Reply -cmatch "^version=1 select=$dec kind=6 at=$dec began=$dec tmu_at=$dec type=[1-5] from=$dec to=$dec cap=$dec asked=[01] error=-?$dec mc=-?$dec$" -or
                $Reply -cmatch "^version=2 select=$dec kind=7 at=$dec end=$dec bits=$dec severity=[1-3] peak=-?$dec lit_peak=-?$dec bp=$dec lp=$dec bsp=$dec lsp=$dec samples=$dec$" -or
                $Reply -cmatch "^version=2 select=$dec kind=8 episode=$dec index=$dec at=$dec direct=-?$dec lit=-?$dec bp=$dec lp=$dec bs=$dec ls=$dec bkhz=$dec lkhz=$dec fast=$dec lfast=$dec$")
        }
        '^p1273_gate_status$' {return ($Reply -cmatch "^version=1 phase=[0-3] mode=$dec generation=$dec qualified=$dec missing=$dec lease=$dec expiry=$dec faults=$dec$")}
        '^p1273_gate_(active|post)$' {
            $phase=if($Matches[1] -eq 'active'){0}else{1}
            return ($Reply -cmatch "^version=1 phase=$phase values=$hex(?:,$hex){14}$")
        }
        '^p1273_boot_identity$' {return ($Reply -cmatch '^version=1 nonce=[0-9a-f]{16}$' -and $Reply -ne 'version=1 nonce=0000000000000000')}
        '^p1273_offline_status$' {return ($Reply -cmatch "^version=1 fixed=[01] duration=$dec deadline=$dec policy_reason=$dec capture_reason=$dec$")}
        '^p1273_cap_status$' {return ($Reply -cmatch "^version=1 state=[0234] mode=[0-3] generation=$dec reason=$dec start=$dec end=$dec until=$dec windows=$dec big_cap=$dec lit_cap=$dec big_hits=$dec lit_hits=$dec big_peak=$dec lit_peak=$dec gpu_peak=$dec big_max=$dec lit_max=$dec big_asked=[01] lit_asked=[01] measured_ok=$dec measured_bad=$dec mild_done=[01]$")}
        '^p1273_psr$' {return ($Reply -cmatch "^version=1 ready=[01] boot_ms=$dec owner=[0-2] governor=[01] drained=[01] legacy_ticks=$dec legacy_mask=$dec legacy_unmask=$dec faults=$dec last_frame=$dec$")}
        '^p1273_gov_page$' {return ($Reply -cmatch "^version=1 slot=$dec present=[01] bin=$dec at=$dec big_load=$dec lit_load=$dec big_khz=$dec lit_khz=$dec big_thermal=$dec lit_thermal=$dec tmu_age=$dec big_mc=-?$dec lit_mc=-?$dec gpu_mc=-?$dec$")}
        '^p1273_gpu_status$' {return ($Reply -cmatch "^version=1 generation=$dec submitted=$dec completed=$dec overwritten=$dec first_wake=$dec slots=1024$")}
        '^p1273_gpu_page$' {return ($Reply -cmatch "^version=1 slot=$dec seq=$dec generation=$dec job=$dec tid=$dec task=$dec pid=$dec owner=$dec fragment=[01] dropped=[01] status=$dec queued=$dec began=$dec ended=$dec wake_begin=$dec wake_end=$dec wake_attempt=[01] wake_ok=[01]$")}
        '^p1273_trace_status$' {return ($Reply -cmatch "^version=1 state=[0-5] generation=$dec start=$dec end=$dec until=$dec trigger=$dec span=$dec reason=$dec buckets=128 owners=16$")}
        '^p1273_trace_cpu([0-7])$' {
            $cpu=$Matches[1];return ($Reply -cmatch "^version=1 cpu=$cpu generation=$dec mask=$hex/$hex overhead=$dec clipped=$dec$")
        }
        '^p1273_trace_page$' {return ($Reply -cmatch "^version=1 select=$dec generation=$dec present=[01] bin=$dec mask=$hex dropped=$dec dropped_ticks=$dec(?: r=$dec(?:/$dec){7}){0,2}$")}
        '^p1273_floor$' {return ($Reply -cmatch "^version=1 mode=$dec denied=[01] reason_mask=$hex live_mask=$hex input_seen=[01] fingers=$dec input_quiet=[01] ufs_safe=[01] ufs_busy=[01] thermal=$dec arm_thermal=$dec$")}
        '^p1273_bus([0-7])$' {
            $index=@(0,1,5,7,8,9,2,3)[[int]$Matches[1]]
            return ($Reply -cmatch "^version=1 index=$index seq=$dec sampled=[01] errno=-?$dec raw=$dec valid=[01] zero=[01] at=$dec age_ms=$dec latency_us=$dec$")
        }
        '^p1273_wfi([0-7])$' {
            $cpu=$Matches[1];return ($Reply -cmatch "^version=2 cpu=$cpu sgi=$hex ppi=$hex spi=$hex bins=$hex/$hex(?:,$hex/$hex){8}$")
        }
        '^p127_bus$' {return ($Reply -cmatch "^version=1 at=$dec valid=$hex mif=$dec int=$dec intcam=$dec cam=$dec disp=$dec aud=$dec$")}
        '^p127_shub$' {
            $pattern="^version=1 enabled=$hex"
            foreach($i in @(0,1,4,5,7,9)){$pattern+=" s$i=$dec/$dec"}
            return ($Reply -cmatch ($pattern+'$'))
        }
        '^p125_page$' {return ($Reply -cmatch "^version=1 page=$dec(?: r=$hex(?:/$hex){11}/[a-zA-Z0-9_.-]+){1,2}$")}
        '^g3d_lab_stats$' {
            # Older firmware may use a different field set; require typed
            # key=value tokens only for the mixed-line recovery case.
            if($Mixed){return ($Reply -cmatch '^[a-zA-Z_][a-zA-Z0-9_]*=-?[0-9]+(?: [a-zA-Z_][a-zA-Z0-9_]*=-?[0-9]+)+$')}
        }
    }
    # Preserve legacy parser compatibility for unmixed older sysctls.
    return (-not $Mixed -and $Reply.Length -gt 0)
}
$script:LifeLineObserver={
    param([string]$Line,[datetime]$At)
    if($script:P1272HostEnabled -and $Line -match '(?:^|\s)p1272-probe: .+$'){$script:P1272ProbeLines.Add($Line)}
    if($script:P12Command){
        $echo=[regex]::Escape('usb> '+$script:P12Command)
        if($Line -cmatch "(?:^|(?<=\s))$echo\s*$"){
            $script:P12Echo=$true
        }
        if($script:P12Echo){
            $ref=[regex]::Escape('usb: '+$script:P12Command+':')
            if($Line -cmatch "(?:^|(?<=\s))$ref(?<tail>.*)$" -and $Matches['tail'] -cnotmatch '^ set \(0\)\s*$'){
                $script:P12Refusal=$Line
            }
            $expected=$script:P12Command.Substring(('sysctl kern.hakios.').Length).Split('=')[0]
            $prefix=[regex]::Escape('usb: kern.hakios.'+$expected+' = ')
            if($Line -cmatch "(?:^|(?<=\s))$prefix(?<reply>.*)$"){
                $value=$Matches['reply'].Trim()
                $mixed=-not $Line.StartsWith('usb: kern.hakios.'+$expected+' = ',[StringComparison]::Ordinal)
                # Multiple USB tokens on a line are ambiguous.
                if(([regex]::Matches($Line,'(?:^|(?<=\s))usb[:>]')).Count -eq 1 -and (P12-ReplyShape $expected $value $mixed)){
                    if($null -ne $script:P12Reply -and $script:P12Reply -cne $value){
                        $script:P12Refusal='Conflicting duplicate sysctl replies: '+$expected
                    }else{
                        $script:P12Reply=$value
                        if($mixed){Life-Say "P12 MIXED_REPLY_RECOVERED $expected"}
                    }
                }
            }
        }
    }
    if($script:LifeSynced -and $Line -match '^hi: auto=ABORT .+reason=manual_usb_control'){
        $script:P12HiAbortSeen=$true
    }
    if($script:LifeSynced -and $Line -match '^hi: gate=usb-stop'){
        $script:P12HiGateStopSeen=$true
    }
    if($script:P12HiStopConfirmed -and $script:P12Phase -in @('A0','B','A1') -and
       $Line -match '^hi: auto=(baseline|cut1|recover1|cut2|recover2)'){
        $script:P12HiUnexpectedPhase=$true
    }
    if($script:LifeSynced -and $Line -match '^p12-ca: forwarded=(\d+) mean_render_us=(\d+) max_render_us=(\d+)'){
        if(-not $script:P12CA.ContainsKey($script:P12Phase)){
            $script:P12CA[$script:P12Phase]=[pscustomobject]@{Samples=0;MeanSum=0.0;Max=0.0}
        }
        $b=$script:P12CA[$script:P12Phase]
        $b.Samples++;$b.MeanSum+=[double][int]$Matches[2]
        $b.Max=[Math]::Max($b.Max,[double][int]$Matches[3])
    }
}
function P12-Query([string]$Name,[string]$Value='',[switch]$Cleanup,[ValidateRange(1,12)][int]$ReadTimeoutSeconds=12,[switch]$NoReadRetry){
    if(($ReadTimeoutSeconds -ne 12 -or $NoReadRetry) -and ($Value -ne '' -or $Cleanup)){throw 'Short timeout only for read-only query'}
    $allowed=@('hi_cmd','p12_cpu_mode','p12_cpu_stats','p12_wfi_probe','p12_wfi_stats','p12_idle_diag',
        'p12_gpu_boot_stats','p9_auto_gpu','p9_panel_stats','g3d_lab_stats',
        'p122_diag_mode','p122_gpu_stats',
        'p7_cpu_stats','p4_stats')
    if($script:P123HostEnabled){$allowed+=@('p123_floor_mode','p123_floor_stats','p1231_hash_mode','p1231_ufs_stats')}
    if($script:P124HostEnabled){$allowed+=@('p124_cpu_mode','p124_cpu_stats')+(0..7|ForEach-Object {"p124_irq$_";"p124_timer$_"})}
    if($script:P125HostEnabled){$allowed+=@('p125_mode','p125_status','p125_select','p125_page','p6_usb_ticks','p6_usb_connected_ticks','p69_usb_disconnected_ticks')+(0..7|ForEach-Object {"p125_counts$_";"p125_coverage$_"})}
    if($script:P126HostEnabled){$allowed+=@('p126_mode','p126_status','p126_diag')}
    if($script:P127HostEnabled){$allowed+=@('p127_mode','p127_status','p127_metrics','p127_wifi','p127_bt','p127_ticker','p127_bus','p127_shub','p127_fg_seq')+(0..7|ForEach-Object {"p127_wfi$_"})+(0..15|ForEach-Object {"p127_fg$_"})}
    if($script:P1272HostEnabled){$allowed+=@('p1272_gpu_caps','p1272_probe_state','p1272_probe_request')}
    if($script:P1273HostEnabled){
        $allowed+=@('p1273_floor','p1273_psr','p1273_cap_mode','p1273_cap_status','p1273_offline_s','p1273_offline_status','p1273_boot_identity','p1273_gate_status','p1273_gate_begin','p1273_gate_review','p1273_gate_ack','p1273_gate_active','p1273_gate_post','p1273_trace_mode','p1273_trace_status','p1273_trace_select','p1273_trace_page','p1273_gpu_status','p1273_gpu_select','p1273_gpu_page','p1273_gov_select','p1273_gov_page')+(0..7|ForEach-Object {"p1273_bus$_";"p1273_wfi$_";"p1273_trace_cpu$_"})
        if($Name -like 'p1273_*' -and $Name -notin @('p1273_trace_mode','p1273_trace_select','p1273_gpu_select','p1273_gov_select','p1273_cap_mode','p1273_offline_s','p1273_gate_begin','p1273_gate_review','p1273_gate_ack') -and $Value -ne ''){throw "P12.7.3 telemetry is read-only: $Name"}
    }
    if($script:P1274HostEnabled){
        $allowed+=@('p12741_profile','p1274_status','p1274_restore','p1274_select','p1274_page','p1274_metal_log_mode','p12741_revision','p12741_info','p12741_guard','p1275_profile','p1275_revision','p1275_selected','p1275_mode','p1275_focus','p1275_load_state','p12751_revision','p12751_boot_latch','p12751_next','p12751_ack','p12751_stage_info','p12751_stage_focus','p12751_stage_select','p12751_stage_page')
    }
    if($script:P12752IdleAllowed -or $script:P1274HostEnabled){$allowed+=@('p12751_boot_latch','p12752_revision','p12752_eligibility','p12752_idle_mode','p12752_idle_info','p12752_idle_select','p12752_idle_page','p12752_profile')}
    if($script:P12753HostEnabled){$allowed+=@('p12753_revision','p12753_guard','p12753_status','p12753_arm','p12753_select','p12753_page')}
    if($script:P12754HostEnabled){$allowed+=@('p12754_revision','p12754_wait','p12754_rate','p127541_stats','p1231_ufs_stats','p127542_audio','p127542_audio_result','mali_irq','p127542_g2','p127543_wait','p127543_rail','p127543_assist','p127544_window','p127545_cmu','p127545_ect_mif','p127545_ect_int','p127545_int_bounds','p127545_panel','p127545_panel_stats','p127545_hci','p127545_mfc_stats','p127545_cam_stats','p127545_disp_stats','p127545_disp_review')}
    if($script:P1277HostEnabled){$allowed+=@('p1277_revision','p1277_auto_stats','p1277_layer2_stats','p1277_burst_stats','p1277_panel_stats','p1277_gpu_dvs_stats','p1277_psci_stats')}
    if($script:P1277SuiteAllowed){$allowed+=@('p1277_auto_idle','p1277_panel','p1277_active','p1277_gpu_dvs','p1277_g2','p1277_layer2','p1277_burst_trace')}
    if($script:P1278HostEnabled){$allowed+=@('p1278_revision','p1278_panel_stats','p1278_irq_stats','p1278_active_stats')}
    if($script:P1278SuiteAllowed){$allowed+=@('p1278_panel_mode','p1278_panel_review')}
    if($Name -notin $allowed){throw "P12 sysctl disallowed $Name"}
    if($script:P124HostEnabled -and $Name -eq 'p1231_hash_mode' -and $Value -eq '1'){throw 'P12.4 rejects scalar thermal trial'}
    if($Value -ne '' -and -not (
        (($Name -eq 'p12_cpu_mode' -or $Name -eq 'p12_wfi_probe') -and $Value -in @('0','1')) -or
        ($Name -eq 'hi_cmd' -and $Value -in @('0','201')) -or
        ($Name -eq 'p122_diag_mode' -and $Value -in @('0','1','2')) -or
        ($script:P123HostEnabled -and $Name -eq 'p123_floor_mode' -and $Value -in @('0','1')) -or
        ($script:P123HostEnabled -and $Name -eq 'p1231_hash_mode' -and $Value -in @('0','1','2')) -or
        ($script:P123HostEnabled -and $script:P123ClockAllowed -and $Name -eq 'hi_cmd' -and $Value -eq '252') -or
        ($script:P1272HostEnabled -and $Name -eq 'p1272_probe_request' -and $Value -in @('0','1')) -or
        ($script:P1274HostEnabled -and $Name -eq 'p12741_profile' -and $Value -eq '0') -or
        ($script:P1274HostEnabled -and $script:P12752ProfileAllowed -and $Name -eq 'p12752_profile' -and $Value -cmatch '^[1-4]$') -or
        ($script:P12753HostEnabled -and $script:P12753ArmAllowed -and $Name -eq 'p12753_arm' -and $Value -cmatch '^(?:[0-9]|10)$') -or
        ($script:P12754HostEnabled -and $script:P12753HostEnabled -and $script:P12753ArmAllowed -and $Name -eq 'p12753_arm' -and $Value -cmatch '^1[1-4]$') -or
        ($script:P12754HostEnabled -and $script:P12753HostEnabled -and $script:P12753ArmAllowed -and $script:P127544WarmAllowed -and $Name -eq 'p12753_arm' -and ($Value -cmatch '^(?:1[7-9]|2[0-4]|2[6-9])$' -or ($Value -cmatch '^3[1-8]$' -and $script:P127544CompositeAllowed) -or ($script:P127545LongWaitAllowed -and ($Value -cmatch '^(?:8[1-8]|9[0-3])$' -or ($Value -cmatch '^(?:9[5-9]|10[0-2])$' -and $script:P127544CompositeAllowed))))) -or
        ($script:P12754HostEnabled -and $script:P12753HostEnabled -and $script:P12753ArmAllowed -and $Name -eq 'p12753_arm' -and (($script:P127545MfcProbeAllowed -and $Value -eq '110') -or ($script:P127545MfcAllowed -and $script:P127544WarmAllowed -and ($Value -eq '39' -or ($script:P127545LongWaitAllowed -and $Value -eq '103'))))) -or
        ($script:P12754HostEnabled -and $script:P12753HostEnabled -and $script:P12753ArmAllowed -and $Name -eq 'p12753_arm' -and (($script:P127545CamProbeAllowed -and $Value -eq '111') -or ($script:P127545CamAllowed -and $script:P127544WarmAllowed -and ($Value -eq '41' -or ($script:P127545LongWaitAllowed -and $Value -eq '105'))))) -or
        ($script:P12754HostEnabled -and $script:P12753HostEnabled -and $script:P12753ArmAllowed -and $Name -eq 'p12753_arm' -and (($script:P127545DispProbeAllowed -and $Value -in @('112','113')) -or ($script:P127545DispAllowed -and $script:P127544WarmAllowed -and ($Value -in @('43','44','45') -or ($script:P127545LongWaitAllowed -and $Value -in @('107','108','109')))))) -or
        ($script:P12754HostEnabled -and $script:P127545DispProbeAllowed -and $Name -eq 'p127545_disp_review' -and $Value -in @('0','200000','134000')) -or
        ($script:P12753HostEnabled -and $Name -eq 'p12753_select' -and $Value -cmatch '^\d{1,2}$' -and [int]$Value -lt 96) -or
        ($script:P12752IdleAllowed -and $Name -eq 'p12752_idle_mode' -and $Value -cmatch '^[0-2]$') -or
        ($script:P12752IdleAllowed -and $Name -eq 'p12752_idle_select' -and $Value -cmatch '^\d{1,2}$' -and [int]$Value -lt 96) -or
        ($script:P1274HostEnabled -and $script:P1274ProfileAllowed -and $Name -eq 'p1275_profile' -and $Value -match '^(?:[1-9]|1[0-6])$') -or
        ($script:P1274HostEnabled -and $script:P1274ProfileAllowed -and $Name -eq 'p12741_profile' -and $Value -match '^[1-9]$') -or
        ($script:P1274HostEnabled -and $Name -eq 'p1274_select' -and $Value -match '^\d{1,5}$' -and [int]$Value -le 14279) -or
        ($script:P1274HostEnabled -and $Name -eq 'p12751_stage_select' -and $Value -match '^\d{1,3}$' -and [int]$Value -lt 512) -or
        ($script:P1274HostEnabled -and $script:P12751NextAllowed -and $Name -eq 'p12751_next' -and $Value -cmatch '^[0-3]$') -or
        ($script:P1273HostEnabled -and $Name -eq 'p1273_gate_begin' -and $Value -eq '0') -or
        ($script:P1273HostEnabled -and $script:P1273GateAllowed -and $Name -eq 'p1273_gate_begin' -and $Value -in @('3','5','17','129','257','513','903','919')) -or
        ($script:P12754HostEnabled -and $script:P1273GateAllowed -and $Name -eq 'p1273_gate_begin' -and $Value -in @('33','49','65')) -or
        ($script:P12754HostEnabled -and $script:P127543AssistAllowed -and $Name -eq 'p127543_assist' -and $Value -in @('0','1','5','17','33','49','65','129','257','513')) -or
        ($script:P1277SuiteAllowed -and $Name -in @('p1277_auto_idle','p1277_panel','p1277_active','p1277_gpu_dvs','p1277_g2','p1277_burst_trace') -and $Value -in @('0','1')) -or
        ($script:P1278SuiteAllowed -and $Name -in @('p1278_panel_mode','p1278_panel_review') -and $Value -in @('1','2')) -or
        ($script:P1277SuiteAllowed -and $Name -eq 'p1277_layer2' -and $Value -in @('0','1','2','3','4')) -or
        ($script:P12754HostEnabled -and $script:P127542GpuAllowed -and $Name -in @('mali_irq','p127542_g2') -and $Value -in @('0','1')) -or
        ($script:P12754HostEnabled -and $script:P127545PanelAllowed -and $Name -eq 'p127545_panel' -and $Value -in @('0','1')) -or
        ($script:P12754HostEnabled -and $script:P1273GateAllowed -and $Name -eq 'p127542_audio' -and $Value -match '^[1-9]\d{0,7}$') -or
        ($script:P1273HostEnabled -and $script:P1273GateAllowed -and $Name -in @('p1273_gate_review','p1273_gate_ack') -and $Value -match '^[1-9]\d{0,9}$' -and [uint64]$Value -le 99999999) -or
        ($script:P1273HostEnabled -and $script:P1273CombinedAllowed -and $Name -eq 'p127_mode' -and $Value -in @('903','919')) -or
        ($script:P1273HostEnabled -and $Name -eq 'p1273_offline_s' -and $Value -in @('0','60')) -or
        ($script:P1273HostEnabled -and $Name -eq 'p1273_cap_mode' -and $Value -in @('0','1','2','3')) -or
        ($script:P1273HostEnabled -and $Name -eq 'p1273_gov_select' -and $Value -match '^\d{1,3}$' -and [int]$Value -lt 128) -or
        ($script:P1273HostEnabled -and $Name -eq 'p1273_gpu_select' -and $Value -match '^\d{1,4}$' -and [int]$Value -le 1024) -or
        ($script:P1273HostEnabled -and $Name -eq 'p1273_trace_mode' -and $Value -in @('0','1')) -or
        ($script:P1273HostEnabled -and $Name -eq 'p1273_trace_select' -and $Value -match '^\d{1,4}$' -and [int]$Value -lt 8192) -or
        ($script:P127HostEnabled -and $Name -eq 'p127_mode' -and $Value -in @('0','1','3','5','9','17','33','49','65','81','129','257','513')) -or
        ($script:P126HostEnabled -and $Name -eq 'p126_mode' -and $Value -match '^\d{1,2}$' -and [int]$Value -le 63) -or
        ($script:P125HostEnabled -and $Name -eq 'p125_mode' -and $Value -in @('0','1','2','3')) -or
        ($script:P125HostEnabled -and $Name -eq 'p125_select' -and $Value -match '^\d{1,4}$' -and [int]$Value -lt 2048) -or
        ($script:P124HostEnabled -and $Name -eq 'p124_cpu_mode' -and $Value -in @('0','1','2','3','4')) -or
        ($script:P124HostEnabled -and $script:P124ClockAllowed -and $Name -eq 'hi_cmd' -and $Value -eq '253'))){
        throw 'P12.2 only permits bounded CPU/WFI/HI2 and diagnostic mode=0/1/2; no GPU/panel/voltage writes'
    }
    $restore=@('p12_cpu_mode','p12_wfi_probe','hi_cmd','p122_diag_mode')
    if($script:P123HostEnabled){$restore+=@('p123_floor_mode','p1231_hash_mode')}
    if($script:P124HostEnabled){$restore+='p124_cpu_mode'}
    if($script:P125HostEnabled){$restore+='p125_mode'}
    if($script:P126HostEnabled){$restore+='p126_mode'}
    if($script:P127HostEnabled){$restore+='p127_mode'}
    if($script:P1272HostEnabled){$restore+='p1272_probe_request'}
    if($script:P1273HostEnabled){$restore+=@('p1273_trace_mode','p1273_cap_mode','p1273_offline_s','p1273_gate_begin')}
    if($script:P127542GpuAllowed){$restore+=@('mali_irq','p127542_g2')}
    if($script:P1277SuiteAllowed){$restore+=@('p1277_auto_idle','p1277_panel','p1277_active','p1277_gpu_dvs','p1277_g2','p1277_layer2','p1277_burst_trace')}
    if($script:P127545PanelAllowed){$restore+='p127545_panel'}
    if($script:P127545DispProbeAllowed){$restore+='p127545_disp_review'}
    if($script:P127543AssistAllowed){$restore+=@('p127543_assist')}
    if($script:P1274HostEnabled){$restore+='p12741_profile'}
    # P12752 recorder cleanup is an opt-in zero-only restore, not a new bypass.
    if($script:P12752IdleAllowed){$restore+='p12752_idle_mode'}
    if($script:P12753HostEnabled -and $script:P12753ArmAllowed){$restore+='p12753_arm'}
    $clockStop=(($script:P123ClockAllowed -or $script:P124ClockAllowed) -and $script:P123HostEnabled -and $Name -eq 'hi_cmd' -and $Value -eq '201')
    # Rev7 cleanup can read only identity and the exact rollback witnesses
    # without masking the zero-write ACK with an earlier thermal abort.
    $rev7CleanupRead=$script:P1277SuiteAllowed -and $Cleanup -and $Value -eq '' -and $Name -in @('p1273_boot_identity','p1277_panel_stats','p1277_auto_stats','p1277_gpu_dvs_stats','p1277_active','p1277_g2','mali_irq','p127_status','p1277_layer2_stats','p1277_burst_stats')
    $rev8CleanupRead=$script:P1278SuiteAllowed -and $Cleanup -and $Value -eq '' -and $Name -in @('p1278_panel_stats','p1278_irq_stats','p1278_active_stats')
    if($Cleanup -and -not $clockStop -and -not $rev7CleanupRead -and -not $rev8CleanupRead -and ($Name -notin $restore -or $Value -ne '0')){
        throw 'P12.1/P12.2 Cleanup bypass is reserved for bounded restore-to-zero writes only'
    }
    $cmd='sysctl kern.hakios.'+$Name
    if($Value -ne ''){$cmd+='='+$Value}
    # Mark a failed/uncertain ENABLE write dirty before sending it, so
    # a thermal abort cannot skip its bounded restore-to-zero attempt.
    if($Name -eq 'p12_cpu_mode' -and $Value -eq '1'){$script:P12CpuDirty=$true}
    if($Name -eq 'p12_wfi_probe' -and $Value -eq '1'){$script:P12WfiDirty=$true}
    if($Name -eq 'hi_cmd' -and $Value -in @('201','252','253')){$script:P12HiDirty=$true}
    $bytes=[Text.Encoding]::ASCII.GetBytes($cmd+[char]13)
    $attempts=if($Value -eq '' -and -not $Cleanup -and -not $NoReadRetry){$script:P12ReadAttempts}else{1}
    for($attempt=1;$attempt -le $attempts;$attempt++){
        $script:P12Command=''
        # Drain briefly before retry so late data cannot satisfy a new echo.
        # Read-only retries only: never retransmit a policy or cursor write.
        $pace=if($attempt -gt 1){300}else{$script:P12ReadPaceMs}
        if($pace -gt 0 -and -not $Cleanup){
            $until=(Life-Now).AddMilliseconds($pace)
            while((Life-Now) -lt $until){Life-Pump;Life-CheckSafety (Life-Now)}
        }
        $script:P12Reply=$null;$script:P12Refusal=$null;$script:P12Echo=$false
        $script:P12Command=$cmd
        $script:LifeSerial.Write($bytes,0,$bytes.Length)
        Life-Say ('P12 '+$(if($Value -eq ''){'READ '}else{'WRITE '})+$cmd)
        $deadline=(Life-Now).AddSeconds($ReadTimeoutSeconds)
        while((Life-Now) -lt $deadline -and $null -eq $script:P12Reply -and $null -eq $script:P12Refusal){
            Life-Pump
            # During cleanup, do not mask the restore ACK with old heat.
            if(-not $Cleanup){Life-CheckSafety (Life-Now)}
        }
        if($null -ne $script:P12Refusal){$reason=$script:P12Refusal;$script:P12Command='';throw "P12 sysctl rejected: $reason"}
        if($null -ne $script:P12Reply){break}
        $script:P12Command=''
        if($attempt -lt $attempts){Life-Say "P12 READ_RETRY $Name attempt=$($attempt+1)/$attempts"}
    }
    if($null -eq $script:P12Reply){$script:P12Command='';throw "Missing echo and sysctl readback $cmd"}
    $reply=$script:P12Reply
    $record=('{0:HH:mm:ss.fff}Z {1} {2}={3}' -f (Life-Now),$script:P12Phase,$Name,$reply)
    $script:P12Results.Add($record)
    if($script:P12Journal){Add-Content -Encoding UTF8 -LiteralPath $script:P12Journal -Value $record}
    $script:P12Command=''
    $script:P12Echo=$false
    if($Value -ne '' -and $reply -ne $Value){throw "P12 readback mismatch for $Name, got $reply"}
    if($Name -eq 'p12_cpu_mode' -and $Value -eq '0'){$script:P12CpuDirty=$false}
    if($Name -eq 'p12_wfi_probe' -and $Value -eq '0'){$script:P12WfiDirty=$false}
    if($Name -eq 'hi_cmd' -and $Value -eq '0'){$script:P12HiDirty=$false}
    return $reply
}

# P12.1 fail-closed cool-start gate: live-only thermal telemetry.
function P12-CoolEnough([double]$Big,[double]$Little,[double]$Gpu){
    return ($Big -le $script:P12StartBigC -and $Little -le $script:P12StartLittleC -and
        $Gpu -le $script:P12StartGpuC)
}
function P12-CoolStart([int]$Seconds=12){
    $script:P12Phase='COOL_PREFLIGHT'
    $before=$script:LifeSamples.Count
    Life-Say "P12.1 COOL_PREFLIGHT $Seconds seconds: NO SWIPES. Need fresh CPU/GPU and BIG <= $($script:P12StartBigC)C LITTLE <= $($script:P12StartLittleC)C G3D <= $($script:P12StartGpuC)C; hard thermal stops still enforced."
    $until=(Life-Now).AddSeconds($Seconds)
    while((Life-Now) -lt $until){
        Life-Pump
        $now=Life-Now
        Life-CheckSafety $now
        P12-EarlyThermalGuard
        if($script:LifeState -ne 'ON'){
            throw 'P12.1 cool preflight: screen is not ON; refuse active A/B. Leave screen OFF for cooling.'
        }
    }
    $cpu=@($script:LifeSamples | Select-Object -Skip $before | Where-Object {$_.type -eq 'CPU'})
    $gpu=@($script:LifeSamples | Select-Object -Skip $before | Where-Object {
        $_.gpu_temp_c -ne '' -and $_.display -eq 'ON'
    })
    $now=Life-Now
    if($cpu.Count -lt 3 -or $gpu.Count -lt 2 -or
       $script:LifeLastCpuAt -eq [datetime]::MinValue -or
       $script:LifeLastGpuAt -eq [datetime]::MinValue -or
       ($now-$script:LifeLastCpuAt).TotalSeconds -gt 4 -or
       ($now-$script:LifeLastGpuAt).TotalSeconds -gt 8){
        throw "P12.1 preflight lacks fresh thermal telemetry: cpu_n=$($cpu.Count) gpu_n=$($gpu.Count); refuse interaction"
    }
    $big=[double]$script:LifeLastBig
    $little=[double]$script:LifeLastLittle
    $g3d=[double]$script:LifeLastGpuC
    if(-not (P12-CoolEnough $big $little $g3d)){
        throw "P12.1 COOL_START_REFUSED BIG=$big LITTLE=$little G3D=$g3d. No swipes: turn screen OFF/cool; do NOT raise thresholds."
    }
    Life-Say "P12.1 COOL_START_PASS fresh cpu=$($cpu.Count) gpu=$($gpu.Count) BIG=$big LITTLE=$little G3D=$g3d"
}
function P12-EarlyThermalGuard {
    $now=Life-Now
    if(($now-$script:LifeLastCpuAt).TotalSeconds -lt 4 -and
       $null -ne $script:LifeLastBig -and $null -ne $script:LifeLastLittle){
        if($script:LifeLastBig -ge 76 -or $script:LifeLastLittle -ge 70){
            throw "P12.1 EARLY_THERMAL_STOP BIG=$($script:LifeLastBig)C LITTLE=$($script:LifeLastLittle)C (hard stops unmodified)"
        }
    }
    if(($now-$script:LifeLastGpuAt).TotalSeconds -lt 8 -and
       $null -ne $script:LifeLastGpuC -and $script:LifeLastGpuC -ge 72){
        throw "P12.1 EARLY_THERMAL_STOP G3D=$($script:LifeLastGpuC)C (hard stop unmodified)"
    }
}
function P12-QuiesceHI2 {
    # hi_cmd=201 is an existing SpringBoard manual STOP, NOT a
    # frame skip. It cancels hi=2 crossover and clears its gated Clock.
    $script:P12Phase='QUIESCE_HI2'
    $script:P12HiGateStopSeen=$false
    # Repeated launches in one boot may find mailbox=201 already.
    # Toggle to 0 before 201 so the SpringBoard one-second poll sees a
    # NEW command and emits a real gate=usb-stop acknowledgement.
    if((P12-Query 'hi_cmd') -eq '201'){
        [void](P12-Query 'hi_cmd' '0')
        $pause=(Life-Now).AddSeconds(2)
        while((Life-Now) -lt $pause){Life-Pump;Life-CheckSafety (Life-Now)}
    }
    [void](P12-Query 'hi_cmd' '201')
    $until=(Life-Now).AddSeconds(7)
    while((Life-Now) -lt $until -and -not $script:P12HiGateStopSeen){
        Life-Pump
        Life-CheckSafety (Life-Now)
    }
    if(-not $script:P12HiGateStopSeen){
        throw 'P12.1 hi_cmd=201 readback but NO SpringBoard gate=usb-stop ACK. Refuse A/B until HI2 interference is proven stopped.'
    }
    $script:P12HiStopConfirmed=$true
    Life-Say 'P12.1 HI2_QUIESCED gate=usb-stop ACK: autonomous cut/recover suppressed until manual rearm (hi_cmd=252) or reboot'
}

function P12-Observe([string]$Phase,[int]$Seconds,[string]$Instruction){
    $script:P12Phase=$Phase
    $start=Life-Now
    $sampleStart=$script:LifeSamples.Count
    Life-Say "P12 $Phase $Seconds seconds: $Instruction"
    $until=$start.AddSeconds($Seconds)
    while((Life-Now) -lt $until){
        Life-Pump
        $now=Life-Now
        Life-CheckStable $now
        Life-CheckSafety $now
        P12-EarlyThermalGuard
        if($script:P123Strict){P123-Guard}
        if($script:P12HiUnexpectedPhase){
            throw 'P12.1 HI2_AUTO_REARMED during interactive A/B: abort invalid causal comparison'
        }
    }
    $sample=@($script:LifeSamples | Select-Object -Skip $sampleStart | Where-Object {$_.display -eq 'ON'})
    $cpu=@($sample | Where-Object {$_.type -eq 'CPU'})
    $frames=@($sample | Where-Object {$_.type -eq 'FRAME'})
    $miss=@($sample | Where-Object {$_.type -eq 'FRAME_MISS'})
    $big='n/a';$peak='n/a'
    if($cpu.Count){
        $temps=@($cpu | ForEach-Object {[double]::Parse($_.big_c,[Globalization.CultureInfo]::InvariantCulture)})
        $big=[Math]::Round(($temps | Measure-Object -Average).Average,1)
        $peak=($temps | Measure-Object -Maximum).Maximum
    }
    $presented=0;$refresh=0;$missed=0
    foreach($s in $frames){$presented += [int]$s.fps_presented;$refresh += [int]$s.fps_refresh}
    foreach($s in $miss){$missed += [int]$s.fps_missed}
    $clock=@($sample | Where-Object {$_.gpu_clock_mhz -ne ''} | ForEach-Object {[double]$_.gpu_clock_mhz})
    $busy=@($sample | Where-Object {$_.gpu_busy_pct -ne ''} | ForEach-Object {[double]$_.gpu_busy_pct})
    $clockMean='n/a';$busyMean='n/a';$clockMax='n/a'
    if($clock.Count){$clockMean=[Math]::Round(($clock | Measure-Object -Average).Average,1);$clockMax=($clock | Measure-Object -Maximum).Maximum}
    if($busy.Count){$busyMean=[Math]::Round(($busy | Measure-Object -Average).Average,1)}
    $stat=[pscustomobject]@{Name=$Phase;Window=$Seconds;Cpu=$cpu.Count;Fps=$frames.Count;BigMean=$big;BigPeak=$peak;Presented=$presented;Refresh=$refresh;Missed=$missed;GpuClkN=$clock.Count;GpuClkMean=$clockMean;GpuClkMax=$clockMax;GpuBusyN=$busy.Count;GpuBusyMean=$busyMean}
    $script:P12Phases.Add($stat)
    Life-Say ("P12 $Phase ON cpu_n=$($stat.Cpu) fps_n=$($stat.Fps) BIG_mean=$big BIG_peak=$peak frames=$presented/$refresh missed=$missed G3D_clk_n=$($clock.Count) G3D_clk_avg_MHz=$clockMean G3D_busy_n=$($busy.Count) G3D_busy_avg_pct=$busyMean")
}
function P12-ObserveIdle{
    $script:P12Phase='NATURAL_OFF'
    Life-Say "P12.1 IDLE $IdleWindowSeconds seconds: manually press POWER ONCE to turn screen OFF (auto-lock disabled). NO SWIPES. After >=60s OFF, optional -WakeCheck: press POWER once to wake."
    $beg=Life-Now
    $lastProbe=$beg
    $offStableSeen=$false
    $offGpuConfirmed=$false
    $wakeChecked=$false
    while(((Life-Now)-$beg).TotalSeconds -lt $IdleWindowSeconds){
        Life-Pump
        $now=Life-Now
        Life-CheckStable $now
        Life-CheckSafety $now
        if($script:LifeState -eq 'OFF' -and ($now-$script:LifeStateSince).TotalSeconds -ge $OffStableSeconds){
            if(-not $offStableSeen -and $script:P124HostEnabled -and $WfiProbe){P124-IrqSnapshot 'IRQ_OFF_STABLE_BEGIN'}
            $offStableSeen=$true
            if(($now-$lastProbe).TotalSeconds -ge 20){
                $lastProbe=$now
                $offStats=P12-Query 'g3d_lab_stats'
                $newOff=0L
                if($offStats -match '\boff_count=(\d+)'){$newOff=[long]$Matches[1]}
                if($offStats -match 'domain=0 pll=0' -and $offStats -match 'off_total_ms=([1-9][0-9]*)' -and
                   $newOff -gt $script:P12GpuOffBaseline -and $offStats -match '\bfault=0(?:\s|$)'){
                    $offGpuConfirmed=$true
                }
                [void](P12-Query 'p9_auto_gpu')
                if($WfiProbe){
                    [void](P12-Query 'p12_wfi_stats')
                    if(-not $LegacyFirmware){[void](P12-Query 'p12_idle_diag')}
                    if($script:P124HostEnabled){P124-IrqSnapshot 'IRQ_OFF_STABLE_PERIODIC'}
                }
            }
        }
        if($WakeCheck -and $offStableSeen -and $script:LifeState -eq 'ON' -and -not $wakeChecked){
            # Human physical button supplies the wake source; this script
            # NEVER simulates wake or writes the G3D PMU/PLL.
            $prevOff=@($script:LifeSegments | Where-Object {$_.State -eq 'OFF' -and $null -ne $_.End} | Select-Object -Last 1)
            if($prevOff.Count -ne 1 -or ($prevOff[0].End-$prevOff[0].Start).TotalSeconds -lt 60 -or -not $offGpuConfirmed){
                throw 'P12.1 WAKE_REFUSED: need >=60s physical screen OFF AND actual G3D domain=0 pll=0 off_total_ms>0 before wake measurement'
            }
            $script:P12Phase='MANUAL_WAKE_VERIFY'
            $afterWake=(Life-Now).AddSeconds(3)
            while((Life-Now) -lt $afterWake){Life-Pump;Life-CheckSafety (Life-Now)}
            $stats=P12-Query 'g3d_lab_stats'
            $wakes=0L
            if($stats -match '\bwake_count=(\d+)'){$wakes=[long]$Matches[1]}
            if($stats -notmatch 'domain=1 pll=1' -or $wakes -le $script:P12GpuWakeBaseline -or $stats -notmatch '\bwake_fail=0(?:\s|$)'){
                throw "P12.1 GPU manual wake not verified; PMU/PLL or new wake counter not advanced (baseline=$($script:P12GpuWakeBaseline)): $stats"
            }
            $wakeChecked=$true
            Life-Say 'P12.1 MANUAL_WAKE VERIFIED: GPU PMU+PLL ON, wake_count increased, wake_fail=0'
            $script:P12Phase='NATURAL_OFF'
        }
    }
    if(-not $offStableSeen){throw 'P12.1 NO_OFF_STABLE: manual power button OFF was not observed; no idle claim'}
    if($WakeCheck -and -not $wakeChecked){throw 'P12.1 NO_WAKE: -WakeCheck requested but >=60s physical OFF -> GPU domain/pll OFF -> manual wake could not be verified'}
}
function P12-Report([string]$Stem,[string]$Reason){
    Life-Report $Stem (Life-Now) $Reason
    $out=New-Object 'System.Collections.Generic.List[string]'
    $out.Add('P12.1 SAFE dual source capture: PASSIVE by default; interactive only by explicit opt-in after cool-start.')
    $out.Add('HI2 manual stop mailbox=201 is used ONLY for -InteractiveAB; default passive capture leaves HI2 unchanged. A stopped HI2 crossover remains aborted until explicit rearm/reboot.')
    $out.Add('HI2 quiesce confirmed='+$script:P12HiStopConfirmed+'; unexpected_auto_phase='+$script:P12HiUnexpectedPhase+'; interactive_requested='+[bool]$InteractiveAB+'; wake_requested='+[bool]$WakeCheck+'; legacy_firmware='+[bool]$LegacyFirmware)
    $out.Add('G3D baseline off_count='+$script:P12GpuOffBaseline+' wake_count='+$script:P12GpuWakeBaseline+'; -WakeCheck requires fresh increments, PMU/PLL OFF then ON')
    $out.Add('P12 integrated thermal/idle hardware A/B - DO NOT CLAIM POWER OR FPS IMPROVEMENT WITHOUT SAME-GESTURE COMPARISON')
    $out.Add('P12 legacy A0/B/A1 CPU no-touch. P12.4 optional floor/boost trials explicitly labeled; stock load target and thermal ceilings retained.')
    $out.Add('P12.1 WFI observation also records per-core idle entry/denied/timer/WFE/no-WFI/exit; NOT actual C2/CPD/SICD or system suspend.')
    $out.Add('CPU frame mode0/1 needs verified sysctl readback; WFI observation is NOT actual C2/CPD/SICD or system suspend.')
    $out.Add('GPU boot gate uses P9 controls unchanged, verified via independent g3d_lab_stats PMU/PLL and off_total_ms.')
    $out.Add('No CPU_OFF, no forced Lock Screen, no panel DCS sleep, no voltage commands.')
    $out.Add('STOP='+$Reason)
    foreach($p in $script:P12Phases){
        $out.Add("PHASE $($p.Name) ON_cpu_n=$($p.Cpu) ON_fps_n=$($p.Fps) BIG_mean=$($p.BigMean) BIG_peak=$($p.BigPeak) presented=$($p.Presented) refresh=$($p.Refresh) missed=$($p.Missed) gpu_clk_n=$($p.GpuClkN) gpu_clk_MHz=$($p.GpuClkMean) gpu_clk_max_MHz=$($p.GpuClkMax) gpu_busy_n=$($p.GpuBusyN) gpu_busy_pct=$($p.GpuBusyMean)")
        if($p.Cpu -lt 5 -or $p.Fps -lt 5){$out.Add("PHASE $($p.Name) insufficient ON coverage: invalid A/B comparison")}
    }
    foreach($name in @('A0','B','A1')){
        if($script:P12CA.ContainsKey($name)){
            $v=$script:P12CA[$name]
            $out.Add("CA $name batches=$($v.Samples) mean_render_us=$([Math]::Round($v.MeanSum/$v.Samples)) max_render_us=$($v.Max)")
        }
    }
    $out.Add('SYSCTL ACKS AND PMU STATS')
    foreach($s in $script:P12Results){$out.Add($s)}
    $path=$Stem+'.p12-report.txt'
    [IO.File]::WriteAllLines($path,$out.ToArray(),[Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllLines($Stem+'.events.txt',$script:LifeEvents.ToArray(),[Text.UTF8Encoding]::new($false))
    Life-Say ('P12 REPORT: '+$path)
    foreach($line in $out){Write-Host $line}
}
function P12-Run {
    if(-not $Execute){
        Write-Host 'P12.1 PREVIEW: SAFE PASSIVE by default. Cool-start, preserve existing HI2 policy, observe ON without swipes, then manual power-button OFF.'
        Write-Host "ON window $ActiveWindowSeconds s; OFF observation $IdleWindowSeconds s. NO forced screen ON."
        Write-Host 'Keep PuTTY CLOSED. NO TOUCH / SWIPES in default mode. -InteractiveAB is opt-in ONLY after naturally cool baseline; early thermal guard aborts.'
        Write-Host 'Only -InteractiveAB writes hi_cmd=201 to stop legacy HI2 crossover (verified SpringBoard ACK); passive default leaves HI2 untouched.'
        Write-Host 'P12 bounded sysctl writes: hi_cmd=201/0 and CPU mode0/1 ONLY with -InteractiveAB; WFI0/1 only if requested, plus restoring inherited nonzero modes.'
        Write-Host 'Optional -WakeCheck: after OFF>=25s manually press power button once, then read GPU domain/PLL and wake counters.'
        Write-Host 'P12 NEVER writes GPU/panel/CPU_OFF/voltage policies or forces a wake.'
        Write-Host 'On old P12 kernel g197d148, use -LegacyFirmware to skip NEW p12_idle_diag; full idle-path counters require matched new kernel.'
        Write-Host 'Files required together: s9-p12-dual-serial.ps1 and s9-screen-lifecycle-serial.ps1.'
        return
    }
    if($env:OS -ne 'Windows_NT'){throw 'Windows only'}
    if([IO.Ports.SerialPort]::GetPortNames() -notcontains $Port){throw "Port $Port absent"}
    $stem=Join-Path (Get-Location).Path ('putty-S9-p12-dual-'+(Get-Date -Format 'yyyyMMdd-HHmmss'))
    $raw=$stem+'.log'
    if(Test-Path $raw){throw 'Existing raw log not overwritten'}
    $errorText=$null
    try{
        $script:LifeSerial=[IO.Ports.SerialPort]::new($Port,115200,[IO.Ports.Parity]::None,8,[IO.Ports.StopBits]::One)
        $script:LifeSerial.DtrEnable=$true
        $script:LifeSerial.ReadTimeout=160
        $script:LifeSerial.WriteTimeout=4000
        $script:LifeSerial.ReadBufferSize=131072
        $script:LifeSerial.Open()
        $script:LifeRaw=[IO.FileStream]::new($raw,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read)
        Life-SyncReadOnly
        $priorCpu=P12-Query 'p12_cpu_stats'
        if($priorCpu -notmatch '^mode=[01] '){throw 'Wrong kernel: P12 sysctl not recognized'}
        $script:P12PreflightVerified=$true
        if($priorCpu -match '^mode=1 '){
            $script:P12CpuDirty=$true
            [void](P12-Query 'p12_cpu_mode' '0' -Cleanup)
            Life-Say 'P12.1 recovered inherited CPU trial mode1 to 0 before ANY heat experiment'
        }
        [void](P12-Query 'p12_gpu_boot_stats')
        [void](P12-Query 'p9_auto_gpu')
        $gpuStart=P12-Query 'g3d_lab_stats'
        if($gpuStart -match '\boff_count=(\d+)'){$script:P12GpuOffBaseline=[long]$Matches[1]}
        if($gpuStart -match '\bwake_count=(\d+)'){$script:P12GpuWakeBaseline=[long]$Matches[1]}
        [void](P12-Query 'p9_panel_stats')
        $priorWfi=P12-Query 'p12_wfi_stats'
        if($priorWfi -match '^enabled=1 '){
            $script:P12WfiDirty=$true
            [void](P12-Query 'p12_wfi_probe' '0' -Cleanup)
            Life-Say 'P12.1 recovered inherited WFI probe1 to 0 before preflight'
        }
        if(-not $LegacyFirmware){[void](P12-Query 'p12_idle_diag')}
        [void](P12-Query 'p7_cpu_stats')
        # A0/B/A1 are never run on an already hot boot. Live, fresh,
        # threshold-gated preflight precedes ALL performance policy writes.
        P12-CoolStart 12
        if($InteractiveAB){
            # Quiesce legacy HI2 ONLY during causal A/B. Passive capture
            # must leave the production timer/clock policy unchanged.
            P12-QuiesceHI2
            P12-EarlyThermalGuard
            [void](P12-Query 'p12_cpu_mode' '0')
            P12-Observe 'A0' $ActiveWindowSeconds 'STOCK CPU target; natural modest interaction ONLY if still cool'
            [void](P12-Query 'p12_cpu_stats')
            P12-EarlyThermalGuard
            [void](P12-Query 'p12_cpu_mode' '1')
            P12-Observe 'B' $ActiveWindowSeconds 'P12 mode1; original touch boost and frame deadlines intact'
            [void](P12-Query 'p12_cpu_stats')
            [void](P12-Query 'p12_cpu_mode' '0')
            P12-Observe 'A1' $ActiveWindowSeconds 'STOCK restored; matched minimal interaction'
            [void](P12-Query 'p12_cpu_stats')
        }else{
            P12-Observe 'PASSIVE_ON' $ActiveWindowSeconds 'NO swipes, no touches; collect natural render/thermal baseline'
        }
        if($WfiProbe){[void](P12-Query 'p12_wfi_probe' '1')}
        P12-ObserveIdle
        if($WfiProbe){
            [void](P12-Query 'p12_wfi_stats')
            if(-not $LegacyFirmware){[void](P12-Query 'p12_idle_diag')}
        }
        [void](P12-Query 'g3d_lab_stats')
        [void](P12-Query 'p12_gpu_boot_stats')
        Life-Say 'P12 capture complete; restoring all modified CPU observational policies'
    }catch{
        $errorText=$_.Exception.Message
        Life-Say ('P12 STOPPED: '+$errorText)
    }finally{
        if($script:P12PreflightVerified -and $null -ne $script:LifeSerial -and $script:LifeSerial.IsOpen){
            # Bounded restores must not be blocked by the SAME hot sample
            # that correctly caused the experiment to abort. Only write a
            # control if this run attempted to change it.
            if($script:P12CpuDirty){
                try{[void](P12-Query 'p12_cpu_mode' '0' -Cleanup);Life-Say 'P12 CPU mode0 RESTORE ACK confirmed'}
                catch{Life-Say ('P12 CPU mode0 restore UNSURE: '+$_.Exception.Message)}
            }
            if($script:P12WfiDirty){
                try{[void](P12-Query 'p12_wfi_probe' '0' -Cleanup);Life-Say 'P12 WFI probe0 RESTORE ACK confirmed'}
                catch{Life-Say ('P12 WFI probe0 restore UNSURE: '+$_.Exception.Message)}
            }
            if($script:P12HiDirty){
                try{[void](P12-Query 'hi_cmd' '0' -Cleanup);Life-Say 'P12 HI2 mailbox0 ACK; autonomous crossover remains canceled until explicit rearm/reboot'}
                catch{Life-Say ('P12 HI2 mailbox restore UNSURE: '+$_.Exception.Message)}
            }
        }
        if($null -ne $script:LifeRaw){try{$script:LifeRaw.Flush()}catch{};$script:LifeRaw.Dispose();$script:LifeRaw=$null}
        if($null -ne $script:LifeSerial){$script:LifeSerial.Dispose();$script:LifeSerial=$null}
        if(Test-Path -LiteralPath $raw){
            try{P12-Report $stem $(if($errorText){$errorText}else{'COMPLETE'})}
            catch{Life-Say ('P12 report failed; raw retained: '+$_.Exception.Message)}
            Write-Host ('RAW LOG: '+$raw)
        }
    }
    if($errorText){throw ('P12 capture incomplete: '+$errorText)}
}
if(-not $LibraryOnly){P12-Run}
