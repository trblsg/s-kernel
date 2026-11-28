# Rev8 connected, opt-in pilot; no voltage writes, no offline energy claims.
function P78-CheckRevision {
    if(-not $P12754 -or (P12-Query 'p1278_revision') -ne '8' -or (P12-Query 'p1277_revision') -ne '7' -or (P12-Query 'p12754_revision') -ne '6'){throw 'Rev8/Rev7/ABI6 mismatch'}
}
function P1278-Status([string]$Stem){
    P78-CheckRevision
    $boot=P12-Query 'p1273_boot_identity'
    $values=[ordered]@{}
    foreach($n in @('p1277_auto_stats','p1277_panel_stats','p1278_panel_stats','p1278_irq_stats','p1278_active_stats','p1277_gpu_dvs_stats','p12753_guard','p127_status')){
        $values[$n]=P125-Fields (P12-Query $n)
    }
    if((P12-Query 'p1273_boot_identity') -cne $boot){throw 'Rev8Status boot changed'}
    $r=[ordered]@{version=1;revision=8;boot=$boot;power_qualified=$false;values=$values}
    $r|ConvertTo-Json -Depth 10|Set-Content -Encoding UTF8 -LiteralPath "$Stem.rev8-status.json"
    Write-Host "Rev8 read-only status: $Stem.rev8-status.json"
}
function P78-Save([string]$Module){
    P77-Identity
    if($Module -notin $script:P78Done){$script:P78Done+=@($Module)}
    $script:P77Report.completed=@($script:P78Done)
    P77-Record 'checkpoint' @{module=$Module;completed=@($script:P78Done);boot=$script:P77Boot}
    $r=[ordered]@{version=1;revision=8;boot=$script:P77Boot;completed=@($script:P78Done);power_qualified=$false}
    $tmp="$script:P78Checkpoint.tmp"
    $r|ConvertTo-Json -Depth 5|Set-Content -Encoding UTF8 -LiteralPath $tmp
    Move-Item -LiteralPath $tmp -Destination $script:P78Checkpoint -Force
}
function P78-StockWake {
    P77-Display 'ON'
    $until=(Life-Now).AddSeconds(20)
    do{
        $a=P77-Read 'p1277_auto_stats'
        if($a.fault -ne '0'){throw 'Auto Idle restore fault'}
        if($a.own_lease -eq '0' -and $a.live_mode -eq '0'){
            $s=P77-Read 'p127_status'
            if($s.mode -eq '0' -and $s.ufs_safe -eq '1' -and [int]$s.little_cur -ge 1053000){return}
        }
        P77-Wait 1 'ON'
    }while((Life-Now) -lt $until)
    throw 'Auto Idle wake restore not proven'
}
function P78-CoolOff {
    P77-Display 'OFF'
    $until=(Life-Now).AddSeconds(90)
    do{
        $g=P77-Guard
        if([int]$g.big -le 60000 -and [int]$g.lit -le 59000 -and [int]$g.gpu -le 60000){return}
        Life-Say 'REV8 cooling OFF; Auto Idle stays enabled if owned.'
        P77-Wait 5 'OFF'
    }while((Life-Now) -lt $until)
    throw 'Rev8 cooldown timeout; do not raise thermal limits'
}
function P78-Enable([string]$Name){
    P77-Identity;[void](P77-Guard)
    if($Name -in $script:P78Owned){throw 'Duplicate Rev8 owner'}
    $script:P78Owned+=@($Name);P77-Record 'enable_pending' $Name
    [void](P12-Query $Name '1')
}
function P78-Disable([string]$Name){
    if($Name -notin $script:P78Owned){return}
    P77-Identity -Cleanup
    if($Name -eq 'p1277_panel'){
        $p=P77-Read 'p1277_panel_stats' -Cleanup
        if($script:LifeState -ne 'ON' -or $p.asleep -ne '0' -or $p.pending -ne '0' -or $p.wake_uncertain -ne '0'){throw 'Panel asleep or uncertain; do not force disarm'}
    }
    if($Name -eq 'p1277_auto_idle'){
        $script:P77Dirty=$Name
        P77-Disable
    }else{
        [void](P12-Query $Name '0' -Cleanup)
        switch($Name){
            'p1277_panel' {
                $p=P77-Read 'p1277_panel_stats' -Cleanup
                if($p.request -ne '0' -or $p.persistent -ne '0' -or $p.asleep -ne '0' -or $p.wake_uncertain -ne '0' -or $p.pending -ne '0'){throw 'Panel disarm not verified'}
            }
            'p1277_g2' {if((P77-Read 'p1278_active_stats' -Cleanup).g2_request -ne '0'){throw 'G2 restore unconfirmed'}}
            'p1277_active' {if((P77-Read 'p1278_active_stats' -Cleanup).active -ne '0'){throw 'CPU restore unconfirmed'}}
            'mali_irq' {if((P77-Read 'p1278_irq_stats' -Cleanup).enabled -ne '0'){throw 'IRQ restore unconfirmed'}}
            default {throw 'Unexpected Rev8 owner'}
        }
    }
    $script:P78Owned=@($script:P78Owned|Where-Object {$_ -ne $Name})
    P77-Record 'restore_verified' $Name
}
function P78-Panel([int]$Mode,[string]$Module){
    P78-CoolOff;P78-StockWake
    $before=P77-Read 'p1278_panel_stats'
    if($before.permission -ne '1' -or $before.failed -ne '0' -or ($Mode -eq 2 -and (([int]$before.reviewed -band 1) -eq 0))){throw 'Panel pilot is not eligible'}
    P77-Identity
    [void](P12-Query 'p1278_panel_mode' ([string]$Mode))
    P78-Enable 'p1277_panel'
    P77-Display 'OFF';P77-Wait 15 'OFF'
    $p=P77-Read 'p1277_panel_stats';$d=P77-Read 'p1278_panel_stats'
    P77-Record 'panel_off' @{mode=$Mode;panel=$p;witness=$d}
    if($p.asleep -ne '1' -or $d.off_verified -ne '1' -or [int]$d.off_mode -ne $Mode -or [uint64]$d.off_count -le [uint64]$before.off_count -or $d.failed -ne '0' -or ($Mode -eq 2 -and $p.sleep_verified -ne '1') -or ($Mode -eq 1 -and $p.sleep_verified -ne '0')){throw 'Panel OFF witness failed'}
    P78-CoolOff;P78-StockWake
    $until=(Life-Now).AddSeconds(8)
    do{
        $d=P77-Read 'p1278_panel_stats'
        if($d.failed -ne '0' -or $d.reason -ne '0'){throw 'Rev8 wake failed or fallback; no next cycle'}
        if($d.pending -eq '0' -and [uint64]$d.clean -gt [uint64]$before.clean){break}
        P77-Wait 1 'ON'
    }while((Life-Now) -lt $until)
    $p=P77-Read 'p1277_panel_stats';P77-Record 'panel_wake' @{mode=$Mode;panel=$p;witness=$d}
    if($p.asleep -ne '0' -or $p.wake_uncertain -ne '0' -or $p.pending -ne '0' -or $d.pending -ne '0' -or [uint64]$d.clean -le [uint64]$before.clean -or $d.first -eq $d.done -or ($Mode -eq 2 -and [uint64]$d.replays -le [uint64]$before.replays)){throw 'Fresh transfer or decoder recovery not proven'}
    try{P77-Review 'No gray flash and no fixed black region? Check one gentle swipe. Y only if fully normal.' 15}
    catch{
        # Retire only, never force disarm an asleep/uncertain panel.
        P77-Identity
        [void](P12-Query 'p1278_panel_review' '2')
        P77-Record 'panel_visual_rejected' @{mode=$Mode}
        throw
    }
    P77-Identity;[void](P12-Query 'p1278_panel_review' '1')
    $d=P77-Read 'p1278_panel_stats'
    if(([int]$d.reviewed -band (1 -shl ($Mode-1))) -eq 0){throw 'Panel review not latched'}
    P78-Disable 'p1277_panel';P78-Save $Module
}
function P78-ActiveWindow([string]$Label,[bool]$IRQ,[bool]$G2,[bool]$CPU){
    P78-CoolOff;P78-StockWake;[void](P77-Guard -Cold)
    $before=P77-Read 'p1278_irq_stats'
    try{
        if($IRQ){P78-Enable 'mali_irq'}
        if($G2){P78-Enable 'p1277_g2'}
        if($CPU){P78-Enable 'p1277_active'}
        Life-Say "REV8 ${Label}: same gentle UI motion for 5s; N abort."
        P77-Wait 5 'ON'
        $after=P77-Read 'p1278_irq_stats';$active=P77-Read 'p1278_active_stats'
        P77-Record 'foreground_window' @{label=$Label;before=$before;after=$after;active=$active;guard=(P77-Guard)}
        if($after.fault -ne '0' -or [uint64]$after.fallback -gt [uint64]$before.fallback -or [uint64]$after.arm_fail -gt [uint64]$before.arm_fail){throw 'GPU IRQ or hardware fault'}
        if($IRQ -and ($after.enabled -ne '1' -or $after.attached -ne '1' -or [uint64]$after.waits -le [uint64]$before.waits)){throw 'IRQ delivery not witnessed'}
        if($G2 -and ($active.g2_request -ne '1' -or $active.g2_effective -ne '1')){throw 'G2 effective request not verified'}
        if($CPU -and ($active.active -ne '1' -or [int]$active.boost_ms -gt 150 -or [int]$active.burst_ms -gt 60 -or [int]$active.lit_touch -gt 1456000)){throw 'Active CPU policy not verified'}
        P77-Review 'UI normal, no gray or black region, no stall?' 10
    }finally{
        foreach($n in @('p1277_active','p1277_g2','mali_irq')){P78-Disable $n}
    }
    P78-Save $Label
}
function P1278-RunSuite([string]$Stem){
    if(-not $Execute -or -not $P12754){throw 'Rev8Suite requires -P12754 -Execute'}
    if(@($Rev8Modules|Select-Object -Unique).Count -ne $Rev8Modules.Count){throw 'Duplicate Rev8 module'}
    $script:P77Dirty='';$script:P78Owned=@();$script:P78Done=@()
    $script:P77Boot=P12-Query 'p1273_boot_identity'
    $script:P77ReportPath="$Stem.rev8-suite.json"
    $script:P78Checkpoint="$Stem.rev8-checkpoint.json"
    $script:P77Report=[ordered]@{version=1;host_revision='rev8';boot=$script:P77Boot;modules=$Rev8Modules;completed=@();result='incomplete';cleanup='not_needed';power_verified=$false;events=[Collections.Generic.List[object]]::new()}
    $script:P127542GpuAllowed=$true
    try{
        P78-CheckRevision;P12754-Require
        if($Rev8Checkpoint){
            $c=Get-Content -Raw -LiteralPath $Rev8Checkpoint|ConvertFrom-Json
            if($c.version -ne 1 -or $c.revision -ne 8 -or $c.boot -cne $script:P77Boot){throw 'Checkpoint belongs to a different boot/revision'}
            $script:P78Done=@($c.completed|Where-Object {$_ -in @('AutoIdle','PanelShallow','PanelDeep','Baseline','IRQ','G2','CPU','Combined')})
            $script:P77Report.completed=@($script:P78Done)
        }
        $a=P77-Read 'p1277_auto_stats';$p=P77-Read 'p1277_panel_stats';$v=P77-Read 'p1278_panel_stats'
        $i=P77-Read 'p1278_irq_stats';$u=P77-Read 'p1278_active_stats';$d=P77-Read 'p1277_gpu_dvs_stats';$l=P77-Read 'p1277_layer2_stats';$b=P77-Read 'p1277_burst_stats'
        P77-Record 'baseline' @{auto=$a;panel=$p;v8=$v;irq=$i;active=$u;dvs=$d;guard=(P77-Guard)}
        if($a.permission -ne '1' -or $a.enabled -ne '0' -or $a.own_lease -ne '0' -or $a.fault -ne '0' -or $a.ufs_safe -ne '1' -or $p.request -ne '0' -or $p.persistent -ne '0' -or $p.asleep -ne '0' -or $p.pending -ne '0' -or $p.wake_uncertain -ne '0' -or $p.failed -ne '0' -or $v.failed -ne '0' -or $i.enabled -ne '0' -or $u.active -ne '0' -or $u.g2_request -ne '0' -or $d.active -ne '0' -or $d.failed -ne '0' -or $l.state -ne '0' -or $l.request -ne '0' -or $l.restored -ne '1' -or $b.request -ne '0' -or $b.trace_state -notin @('0','4')){throw 'Rev8 requires clean baseline; no takeover of existing owners'}
        if((P77-Read 'p127_status').mode -ne '0' -or (P77-Read 'p126_status').mode -ne '0' -or (P1273-GateStatus).phase -ne '0' -or (P77-Read 'p12753_status').state -notin @('0','3') -or (P12-Query 'p127542_g2') -ne '0'){throw 'Another experiment is active'}
        if('AutoIdle' -in $Rev8Modules -or 'Panel' -in $Rev8Modules -or 'Active' -in $Rev8Modules){
            if($a.stage -ne '5'){
                P78-CoolOff;P78-StockWake;P77-Review 'Touch once, confirm normal baseline UI.' 20
            }
            P77-Display 'OFF';P78-Enable 'p1277_auto_idle';P77-AutoWait 120
            P78-Save 'AutoIdle'
        }
        if('Panel' -in $Rev8Modules){
            # On resume, a saved PASS alone is insufficient: require kernel review bits.
            if('PanelShallow' -notin $script:P78Done -or (([int]$v.reviewed -band 1) -eq 0)){P78-Panel 1 'PanelShallow'}
            if('PanelDeep' -notin $script:P78Done -or (([int]$v.reviewed -band 2) -eq 0)){P78-Panel 2 'PanelDeep'}
        }
        if('Active' -in $Rev8Modules){
            foreach($w in @(@{n='Baseline';i=$false;g=$false;c=$false},@{n='IRQ';i=$true;g=$false;c=$false},@{n='G2';i=$false;g=$true;c=$false},@{n='CPU';i=$false;g=$false;c=$true},@{n='Combined';i=$true;g=$true;c=$true})){
                if($w.n -notin $script:P78Done){P78-ActiveWindow $w.n $w.i $w.g $w.c}
            }
        }
        P77-Display 'OFF'
        $script:P77Report.result='completed_driver_and_visual_pilots'
        if($KeepAutoIdle -and 'p1277_auto_idle' -in $script:P78Owned){
            P77-AutoWait 30
            $script:P78Owned=@($script:P78Owned|Where-Object {$_ -ne 'p1277_auto_idle'})
            P77-Record 'auto_idle_retained' 'Explicit -KeepAutoIdle, same-boot qualification verified'
        }
    }catch{
        P77-Record 'stop' $_.Exception.Message
        throw
    }finally{
        $errors=@()
        foreach($n in @('p1277_active','p1277_g2','mali_irq','p1277_panel','p1277_auto_idle')){
            try{P78-Disable $n}catch{$errors+=@($_.Exception.Message)}
        }
        $script:P77Report.cleanup=if($errors.Count){'unconfirmed'}else{'verified'}
        P77-Record 'cleanup' @{errors=$errors;remaining=$script:P78Owned}
        $script:P127542GpuAllowed=$false
        if($errors.Count){throw ('Rev8 cleanup uncertain: '+($errors -join '; '))}
    }
}
