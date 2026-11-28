# Host-only Rev7 runtime suite for firmware f780df0 / installer 1ef3469.
# Loaded only by the explicit Rev7Suite scenario. No arbitrary sysctl writes,
# no offline audit, no MIF/PSCI writes. Always stop on a failed witness.
function P77-Record([string]$Step,$Data){
    $script:P77Report.events.Add([pscustomobject]@{at=(Life-Now).ToString('o');step=$Step;data=$Data})
    $script:P77Report|ConvertTo-Json -Depth 12|Set-Content -Encoding UTF8 -LiteralPath $script:P77ReportPath
}
function P77-Read([string]$Name,[switch]$Cleanup){
    P125-Fields (P12-Query $Name -Cleanup:$Cleanup)
}
function P77-Identity([switch]$Cleanup){
    if((P12-Query 'p1273_boot_identity' -Cleanup:$Cleanup) -cne $script:P77Boot){throw 'Rev7 boot identity changed; no further writes'}
}
# Rev7 connected software/panel admission is NOT the Rev6 battery audit.
# Preserve the existing Rev7 automatic-owner ceiling; stricter cold
# admission remains for voltage/IRQ/G2 and legacy register pilots.
function P77-Guard([switch]$Cold){
    $g=P77-Read 'p12753_guard'
    $reason=''
    if($g.enabled -ne '1' -or [int]$g.age -gt 2000 -or [int]$g.bat -le 0 -or [int]$g.bat -ge 45 -or [int]$g.big -lt 0 -or [int]$g.lit -lt 0 -or [int]$g.gpu -lt 0){$reason='Rev7 invalid/hot thermal or battery telemetry'}
    elseif([int]$g.big -ge 67000 -or [int]$g.lit -ge 66000 -or [int]$g.gpu -ge 68000){$reason='Rev7 host thermal stop (67/66/68 C); cool before any further test'}
    elseif($Cold -and (P127543-ThermalExcess $g) -gt 0){$reason='Rev7 cold pilot requires BIG<=60 LITTLE<=59 GPU<=60 C'}
    if($reason){
        $observed="BIG=$($g.big) LITTLE=$($g.lit) GPU=$($g.gpu) mC BAT=$($g.bat) C age=$($g.age) ms mask=$($g.mask)"
        P77-Record 'thermal_stop' @{reason=$reason;guard=$g;display=$script:LifeState}
        throw "$reason; observed $observed"
    }
    return $g
}
function P77-Pump([switch]$IgnoreKeys){
    Life-Pump;Life-CheckSafety (Life-Now);P12-EarlyThermalGuard
    if(-not $IgnoreKeys -and (P127542-ReadKey) -eq [ConsoleKey]::N){throw 'Rev7 operator abort'}
}
function P77-Display([string]$State,[int]$Seconds=20){
    if($script:LifeState -eq $State){return}
    Life-Say "REV7: turn screen $State now (N abort). USB stays connected."
    $until=(Life-Now).AddSeconds($Seconds)
    $next=Life-Now
    while($script:LifeState -ne $State -and (Life-Now) -lt $until){
        P77-Pump
        if((Life-Now) -ge $next){[void](P77-Guard);$next=(Life-Now).AddSeconds(3)}
    }
    if($script:LifeState -ne $State){throw "Rev7 display $State not observed"}
}
function P77-Wait([int]$Seconds,[string]$State){
    $until=(Life-Now).AddSeconds($Seconds);$next=Life-Now
    while((Life-Now) -lt $until){
        P77-Pump
        if($script:LifeState -ne $State){throw "Rev7 unexpected display change during $State window"}
        if((Life-Now) -ge $next){[void](P77-Guard);$next=(Life-Now).AddSeconds(5)}
    }
}
function P77-Cool([string]$State){
    $g=P77-Guard
    if((P127543-ThermalExcess $g) -gt 0){
        P77-Display 'OFF'
        Life-Say 'REV7: bounded 60s cooldown; do not swipe or unplug.'
        $until=(Life-Now).AddSeconds(60)
        do{
            P77-Wait 5 'OFF';$g=P77-Guard
            if((P127543-ThermalExcess $g) -le 0){break}
        }while((Life-Now) -lt $until)
        if((P127543-ThermalExcess $g) -gt 0){throw 'Rev7 cooling plateau/timeout; stop, no threshold relaxation'}
    }
    P77-Display $State;[void](P77-Guard -Cold)
}
function P77-Review([string]$Message,[int]$Seconds=15){
    Life-Say "REV7: $Message Press Y if clean, N if not."
    $until=(Life-Now).AddSeconds($Seconds);$next=Life-Now
    while((Life-Now) -lt $until){
        P77-Pump -IgnoreKeys
        if($script:LifeState -ne 'ON'){throw 'Rev7 visual review requires screen ON'}
        $key=P127542-ReadKey
        if($key -eq [ConsoleKey]::N){throw 'Rev7 operator rejected'}
        if($key -eq [ConsoleKey]::Y){return}
        if((Life-Now) -ge $next){[void](P77-Guard);$next=(Life-Now).AddSeconds(5)}
    }
    throw 'Rev7 review timed out'
}
function P77-Enable([string]$Name){
    P77-Identity;[void](P77-Guard)
    if($script:P77Dirty){throw 'Rev7 prior owner not restored'}
    $script:P77Dirty=$Name;P77-Record 'enable_pending' $Name
    [void](P12-Query $Name '1')
}
function P77-Disable {
    $name=$script:P77Dirty
    if(-not $name){return}
    P77-Identity -Cleanup
    if($name -eq 'p1277_panel'){
        $p=P77-Read 'p1277_panel_stats' -Cleanup
        if($script:LifeState -ne 'ON' -or $p.asleep -ne '0' -or $p.pending -ne '0' -or $p.wake_uncertain -ne '0'){throw 'Panel asleep/uncertain: do not disarm. Wake normally and recover if abnormal.'}
    }
    [void](P12-Query $name '0' -Cleanup)
    if($name -eq 'p1277_auto_idle'){
        $until=(Life-Now).AddSeconds(20)
        do{
            $a=P77-Read 'p1277_auto_stats' -Cleanup
            P77-Record 'auto_restore' $a
            if($a.fault -ne '0'){throw 'Auto Idle fault during rollback'}
            if($a.enabled -eq '0' -and $a.own_lease -eq '0' -and $a.live_mode -eq '0'){break}
            $pace=(Life-Now).AddMilliseconds(750)
            while((Life-Now) -lt $pace){Life-Pump}
        }while((Life-Now) -lt $until)
        if($a.enabled -ne '0' -or $a.own_lease -ne '0' -or $a.live_mode -ne '0'){throw 'Auto Idle restore unconfirmed'}
        $l=P77-Read 'p1277_layer2_stats' -Cleanup
        $b=P77-Read 'p1277_burst_stats' -Cleanup
        if($l.state -ne '0' -or $l.request -ne '0' -or $l.restored -ne '1' -or $b.request -ne '0' -or $b.trace_state -notin @('0','4')){throw 'Secondary owner restore not proven'}
        $p=P77-Read 'p127_status' -Cleanup
        if($p.ufs_safe -ne '1' -or $p.mode -ne '0' -or [int]$p.little_cur -lt 1053000){throw 'Auto Idle stock floor/UFS not restored'}
    }elseif($name -eq 'p1277_panel'){
        $p=P77-Read 'p1277_panel_stats' -Cleanup
        if($p.request -ne '0' -or $p.persistent -ne '0' -or $p.wake_uncertain -ne '0' -or $p.failed -ne '0' -or $p.fault -ne '0'){throw 'Panel restore not proven'}
    }elseif($name -eq 'p1277_gpu_dvs'){
        $p=P77-Read 'p1277_gpu_dvs_stats' -Cleanup
        if($p.active -ne '0' -or $p.failed -ne '0' -or $p.fault -ne '0'){throw 'GPU DVS restore not proven'}
    }
    P77-Identity -Cleanup
    P77-Record 'restore_verified' $name
    $script:P77Dirty=''
}
function P77-AutoWait([int]$Seconds){
    $until=(Life-Now).AddSeconds($Seconds)
    do{
        if($script:LifeState -ne 'OFF'){throw 'Auto Idle requires display OFF'}
        [void](P77-Guard)
        $a=P77-Read 'p1277_auto_stats';P77-Record 'auto_progress' $a
        if($a.fault -ne '0' -or $a.enabled -ne '1' -or $a.ufs_safe -ne '1'){throw 'Auto Idle retired or UFS unsafe'}
        if($a.state -eq '4' -and $a.stage -eq '5' -and $a.live_mode -in @('65','69') -and $a.own_lease -ne '0'){
            $p=P77-Read 'p127_status'
            if($p.mode -eq $a.live_mode -and $p.little_cur -eq '598000' -and $a.last_uv -eq '637500'){
                P77-Record 'auto_active' $p
                if($a.live_mode -ne '69' -or $a.abox_driver_ok -ne '1'){throw 'LITTLE598 observed; ABOX driver evidence missing (partial only)'}
                return
            }
        }
        P77-Wait 4 'OFF'
    }while((Life-Now) -lt $until)
    throw 'Auto Idle wait timeout; partial progress retained; no repeated enable'
}
function P77-GpuOff{
    $s=P12-Query 'g3d_lab_stats'
    if($s -notmatch 'domain=0 pll=0' -or $s -notmatch '\bfault=0(?:\s|$)'){throw 'GPU autonomous OFF not verified'}
    P77-Record 'gpu_off' $s
}
# Secondary experiments are always owned by the active Auto Idle parent.
# Its zero-write cancels both child requests even if their ACK is lost.
function P77-Layer2([int]$Module){
    P77-Display 'OFF';[void](P77-Guard)
    P77-Enable 'p1277_auto_idle';P77-AutoWait 120
    P77-Identity
    P77-Record 'layer2_request' $Module
    [void](P12-Query 'p1277_layer2' ([string]$Module))
    $until=(Life-Now).AddSeconds(32)
    do{
        P77-Wait 3 'OFF'
        $l=P77-Read 'p1277_layer2_stats';P77-Record 'layer2_progress' $l
        if($l.result -eq '4'){throw 'Layer2 uncertain restore'}
        if($l.state -eq '0' -and $l.request -eq '0' -and $l.module -eq [string]$Module){
            if($l.result -ne '1' -or $l.restored -ne '1' -or [int]$l.samples -lt 20){throw 'Layer2 refused/preempted/incomplete; check prior same-boot qualification'}
            P77-Disable;return
        }
    }while((Life-Now) -lt $until)
    throw 'Layer2 bounded wait expired'
}
function P77-Burst([string]$Stem){
    $b=P77-Read 'p1277_burst_stats'
    if($b.trace_state -ne '0' -or $b.owned -ne '0'){throw 'Existing trace: do not overwrite frozen data'}
    P77-Display 'OFF';[void](P77-Guard)
    P77-Enable 'p1277_auto_idle';P77-AutoWait 120
    P77-Identity;P77-Record 'burst_request' 1
    [void](P12-Query 'p1277_burst_trace' '1')
    $until=(Life-Now).AddSeconds(135)
    do{
        P77-Wait 3 'OFF'
        $b=P77-Read 'p1277_burst_stats';P77-Record 'burst_progress' $b
        if($b.trace_state -eq '4' -and $b.observed_trigger -ne '0'){break}
        if($b.request -eq '0' -and $b.owned -eq '0'){break}
    }while((Life-Now) -lt $until)
    P77-Disable
    $b=P77-Read 'p1277_burst_stats';P77-Record 'burst_final' $b
    if($b.trace_state -eq '4' -and $b.observed_trigger -ne '0'){
        P77-Identity;P1273-TraceDump $Stem
        P77-Record 'trace_dump' 'post-trigger only; inspect coverage and drop counts'
    }elseif($b.trace_state -eq '0' -and $b.owned -eq '0' -and $b.observed_trigger -eq '0'){
        P77-Record 'burst_no_trigger' 'No sustained CPU burst captured; not a failure or attribution'
    }else{throw 'Burst state incomplete or uncertain'}
}
# Reuse original connected-only pilot implementations, preserving their
# exact wire whitelist, CRC/IRQ/visual qualifications and cancellation.
function P77-Legacy([string]$Module,[string]$Stem){
    if($script:P77Dirty){throw 'Rev7 parent owner still active'}
    P77-Identity
    if($Module -eq 'Camera'){
        P77-Cool 'ON'
        P77-Review 'Open camera, wait for live preview, close camera, then press Y.' 30
    }
    P77-Cool 'OFF';P77-Identity
    $script:P12753ArmAllowed=$true
    $script:P127545MfcProbeAllowed=$Module -eq 'MFC'
    $script:P127545CamProbeAllowed=$Module -eq 'Camera'
    $script:P127545DispProbeAllowed=$Module -in @('DISP200','DISP134')
    try{
        switch($Module){
            'MFC' {P127545-MfcProbe "$Stem-mfc"}
            'Camera' {P127545-CamProbe "$Stem-camera"}
            'DISP200' {$Disp545KHz=200000;P127545-DispProbe "$Stem-disp200"}
            'DISP134' {$Disp545KHz=134000;P127545-DispProbe "$Stem-disp134"}
            'Audio' {
                $AllowFunctionalGate=$true;$HeadlessAudio=$true;$HeadlessNetwork=$false;$GateModule='ABOX'
                $script:P12753ArmAllowed=$false
                P1273-FunctionalGate "$Stem-audio"
            }
            default {throw 'Unknown legacy pilot'}
        }
        P77-Identity
        $l=P77-Read 'p1277_layer2_stats'
        if($l.restored -ne '1' -or $l.state -ne '0' -or (P77-Read 'p127_status').mode -ne '0'){throw 'Legacy pilot restore not proven'}
    }catch{
        $script:P77Report.cleanup='legacy_unconfirmed'
        P77-Record 'legacy_stop' $_.Exception.Message
        throw
    }finally{
        $script:P12753ArmAllowed=$false
        $script:P127545MfcProbeAllowed=$false
        $script:P127545CamProbeAllowed=$false
        $script:P127545DispProbeAllowed=$false
        # Outer P125 cleanup still owns any dirty audio gate.
    }
}
function P1277-RunSuite([string]$Stem){
    if(-not $P12754 -or -not $Execute){throw 'Rev7Suite requires -P12754 -Execute'}
    if(@($Rev7Modules|Select-Object -Unique).Count -ne $Rev7Modules.Count -or -not $Rev7Modules.Count){throw 'Select each Rev7 module at most once'}
    $script:P77Dirty='';$script:P77Boot=P12-Query 'p1273_boot_identity'
    $script:P77ReportPath="$Stem.rev7-suite.json"
    $script:P77Report=[ordered]@{version=1;host_revision='auto2';boot=$script:P77Boot;modules=$Rev7Modules;result='incomplete';cleanup='not_needed';power_verified=$false;events=[Collections.Generic.List[object]]::new()}
    $script:P127542GpuAllowed=$true
    try{
        if((P12-Query 'p1277_revision') -ne '7' -or (P12-Query 'p12754_revision') -ne '6'){throw 'Rev7/ABI6 mismatch'}
        P12754-Require
        $a=P77-Read 'p1277_auto_stats';$p=P77-Read 'p1277_panel_stats';$d=P77-Read 'p1277_gpu_dvs_stats';$l=P77-Read 'p1277_layer2_stats';$b=P77-Read 'p1277_burst_stats'
        P77-Record 'baseline' @{auto=$a;panel=$p;dvs=$d;layer2=$l;burst=$b;guard=(P77-Guard);psci=(P77-Read 'p1277_psci_stats')}
        if($a.permission -ne '1' -or $a.enabled -ne '0' -or $a.own_lease -ne '0' -or $a.fault -ne '0' -or $a.ufs_safe -ne '1' -or
            $p.request -ne '0' -or $p.persistent -ne '0' -or $p.asleep -ne '0' -or $p.pending -ne '0' -or $p.wake_uncertain -ne '0' -or $p.failed -ne '0' -or $p.fault -ne '0' -or
            $d.active -ne '0' -or $d.failed -ne '0' -or $d.fault -ne '0' -or $l.state -ne '0' -or $l.request -ne '0' -or $l.restored -ne '1' -or $l.result -eq '4' -or $b.request -ne '0' -or $b.trace_state -notin @('0','4')){throw 'Rev7 requires clean baseline; do not override an existing owner'}
        if((P77-Read 'p127_status').mode -ne '0' -or (P77-Read 'p126_status').mode -ne '0' -or (P1273-GateStatus).phase -ne '0' -or (P77-Read 'p12753_status').state -notin @('0','3') -or (P12-Query 'p1277_active') -ne '0' -or (P12-Query 'mali_irq') -ne '0' -or (P12-Query 'p127542_g2') -ne '0'){throw 'Another experiment is active'}
        P77-Display 'ON'
        P77-Review 'Touch once, check normal UI before starting.' 20
        if((P12-Query 'p1277_g2') -ne '0'){throw 'Existing G2 owner'}
        foreach($module in $Rev7Modules){
            $script:P12Phase="REV7-$module";P77-Record 'module_start' $module
            switch($module){
                'AutoIdle' {
                    # Initial touch/UI check above already satisfies the user
                    # interaction prerequisite; let the device go OFF before
                    # requesting the kernel's guarded automatic policy.
                    P77-Display 'OFF';[void](P77-Guard)
                    P77-Enable 'p1277_auto_idle';P77-AutoWait 120
                    P77-Display 'ON'
                    $until=(Life-Now).AddSeconds(20)
                    do{$a=P77-Read 'p1277_auto_stats';if($a.fault -ne '0'){throw 'Auto Idle wake fault'};if($a.own_lease -eq '0' -and $a.live_mode -eq '0'){break};P77-Pump}while((Life-Now) -lt $until)
                    if($a.own_lease -ne '0' -or $a.live_mode -ne '0'){throw 'Auto Idle wake restore not verified'}
                    P77-Review 'Check touch/scroll after wake; driver ABOX witness is NOT audible qualification.'
                    P77-Display 'OFF';P77-AutoWait 35
                    P77-Disable
                }
                'Panel' {
                    P77-Display 'ON';[void](P77-Guard)
                    $base=P77-Read 'p1277_panel_stats';if($base.permission -ne '1' -or $base.fault -ne '0'){throw 'Panel not eligible'}
                    P77-Enable 'p1277_panel'
                    for($i=1;$i -le 3;$i++){
                        P77-Display 'OFF';P77-Wait 20 'OFF'
                        $off=P77-Read 'p1277_panel_stats';P77-GpuOff
                        P77-Record "panel_off_$i" $off
                        if($off.asleep -ne '1' -or $off.sleep_verified -ne '1' -or $off.persistent -ne '1' -or [uint64]$off.sleep -ne ([uint64]$base.sleep+1) -or $off.failed -ne '0' -or $off.fault -ne $base.fault){throw 'DCS sleep not verified'}
                        P77-Display 'ON'
                        $until=(Life-Now).AddSeconds(10)
                        do{$after=P77-Read 'p1277_panel_stats';if($after.pending -eq '0'){break};P77-Pump}while((Life-Now) -lt $until)
                        P77-Record "panel_on_$i" $after
                        if($after.asleep -ne '0' -or $after.pending -ne '0' -or $after.wake_uncertain -ne '0' -or $after.failed -ne '0' -or $after.fallback -ne $base.fallback -or $after.fault -ne $base.fault -or [uint64]$after.latch29 -ne ([uint64]$base.latch29+1)){throw 'Panel fresh wake failed'}
                        P77-Review "Panel cycle $i/3: check fresh picture, brightness, no tearing."
                        $base=$after
                    }
                    P77-Disable
                }
                'Active' {
                    P77-Display 'ON';[void](P77-Guard)
                    Life-Say 'REV7: stock, swipe gently for 8 seconds.';P77-Wait 8 'ON'
                    P77-Record 'active_baseline' @{guard=(P77-Guard);rate=(P77-Read 'p12754_rate')}
                    P77-Enable 'p1277_active'
                    Life-Say 'REV7: repeat same gentle swipes for 8 seconds.';P77-Wait 8 'ON'
                    P77-Record 'active_observed' @{guard=(P77-Guard);rate=(P77-Read 'p12754_rate');control=(P12-Query 'p1277_active')}
                    P77-Review 'Confirm touch and scrolling are normal.'
                    P77-Disable
                }
                'IRQ' {
                    P77-Cool 'OFF'
                    P77-Enable 'mali_irq';P77-Display 'ON'
                    Life-Say 'REV7: swipe gently for 8 seconds.';P77-Wait 8 'ON'
                    P77-Record 'irq_observed' @{control=(P12-Query 'mali_irq');gpu=(P12-Query 'g3d_lab_stats')}
                    P77-Review 'Confirm GPU IRQ window has no render hang.'
                    P77-Disable
                }
                'DVS' {
                    P77-Cool 'OFF';P77-Wait 20 'OFF';P77-GpuOff
                    P77-Display 'ON';P77-Enable 'p1277_gpu_dvs'
                    $before=P77-Read 'p1277_gpu_dvs_stats';P77-Record 'dvs_enabled' $before
                    if($before.active -ne '1' -or $before.failed -ne '0' -or $before.last_configured_vsel -ne '88'){throw '850mV pilot not verified'}
                    for($i=1;$i -le 3;$i++){
                        P77-Display 'OFF';P77-Wait 20 'OFF';P77-GpuOff
                        P77-Display 'ON';P77-Wait 3 'ON'
                        $after=P77-Read 'p1277_gpu_dvs_stats';P77-Record "dvs_wake_$i" $after
                        if($after.active -ne '1' -or $after.failed -ne '0' -or $after.fault -ne '0' -or $after.last_configured_vsel -ne '88' -or [uint64]$after.high -le [uint64]$before.high -or [uint64]$after.rebase -le [uint64]$before.rebase){throw 'DVS wake/rail witness failed'}
                        P77-Review "GPU DVS cycle $i/3: confirm normal render."
                        $before=$after
                    }
                    P77-Disable
                }
                'Audio' {P77-Legacy 'Audio' $Stem}
                'MFC' {P77-Legacy 'MFC' $Stem}
                'DISP200' {P77-Legacy 'DISP200' $Stem}
                'DISP134' {P77-Legacy 'DISP134' $Stem}
                'Camera' {P77-Legacy 'Camera' $Stem}
                'Layer2MFC' {P77-Layer2 1}
                'Layer2Cam' {P77-Layer2 2}
                'Layer2Disp200' {P77-Layer2 3}
                'Layer2Disp134' {P77-Layer2 4}
                'Burst' {P77-Burst $Stem}
                'G2' {
                    P77-Cool 'ON'
                    P77-Enable 'p1277_g2';Life-Say 'REV7: swipe gently for 8 seconds.';P77-Wait 8 'ON'
                    P77-Record 'g2_observed' @{control=(P12-Query 'p1277_g2')}
                    P77-Review 'Check icons, textures and animations. G2 readback is not skip-count proof.'
                    P77-Disable
                }
            }
            P77-Identity;P77-Record 'module_complete' $module
        }
        $script:P77Report.result='complete';$script:P77Report.cleanup='verified'
    }catch{
        $script:P77Report.result='incomplete';P77-Record 'stop' $_.Exception.Message
        throw
    }finally{
        if($script:P77Dirty){
            try{P77-Disable;$script:P77Report.cleanup='verified'}catch{
                $script:P77Report.cleanup='unconfirmed'
                P77-Record 'cleanup_unconfirmed' @{control=$script:P77Dirty;reason=$_.Exception.Message}
                Life-Say 'REV7 CLEANUP UNCONFIRMED: stop. Persistent requests may remain; wake normally and use recovery if device abnormal. Do not run next experiment.'
            }
        }
        $script:P127542GpuAllowed=$false
        P77-Record 'final' @{result=$script:P77Report.result;cleanup=$script:P77Report.cleanup}
    }
}
