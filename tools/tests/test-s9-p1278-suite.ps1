# Host-only transport/state mocks; never open a COM port.
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../s9-p125-timer-serial.ps1') -LibraryOnly -P12754 -Scenario Rev8Suite -Execute
function Assert($ok,[string]$msg){if(-not $ok){throw $msg}}
function Refuse([scriptblock]$Action,[string]$Pattern){$ok=$false;try{& $Action}catch{if($_.Exception.Message -match $Pattern){$ok=$true}else{throw}};Assert $ok "expected refusal: $Pattern"}
Assert (P12-ReplyShape 'p1278_revision' '81' $true) 'revision'
Assert (-not (P12-ReplyShape 'p1278_revision' '8' $true)) 'wrong revision'
Assert (P12-ReplyShape 'p1278_panel_stats' 'version=1 permission=1 mode=2 off_mode=2 off_verified=0 off_count=2 replays=1 clean=2 reviewed=3 reason=0 first=11 done=12 diag=15 raw=00/00/00/00 id=910443 pending=0 failed=0 refusal=0 drain=3 irq_raw=23040000' $true) 'typed panel reply'
Assert (-not (P12-ReplyShape 'p1278_panel_stats' 'version=1 permission=1 mode=2' $true)) 'truncated reply'
Refuse {P12-Query 'p1278_panel_mode' '3'} 'only permits'
Refuse {P12-Query 'p1278_panel_review' '1' -Cleanup} 'Cleanup bypass'
$script:now=[datetime]::UtcNow
function Life-Now{$script:now=$script:now.AddMilliseconds(50);return $script:now}
function Life-Say($x){}
function Life-Pump{$script:now=$script:now.AddMilliseconds(100)}
function Life-CheckSafety($x){}
function P12-EarlyThermalGuard{}
function P127542-ReadKey{return $null}
function P12754-Require{}
function P1273-GateStatus{return @{phase='0'}}
$script:LifeState='ON'
$script:boot='version=1 nonce=0123456789abcdef'
$script:temp=59000
$script:writes=[Collections.Generic.List[string]]::new()
$script:controls=@{p1277_auto_idle='0';p1277_panel='0';p1277_active='0';p1277_gpu_dvs='0';p1277_g2='0';mali_irq='0';p127542_g2='0'}
$script:panelMode=1;$script:offMode=0;$script:offCount=0;$script:clean=0;$script:replays=0;$script:reviewed=0;$script:failed=0;$script:waits=0;$script:reject=$false;$script:rejectNormal=$false;$script:preoffRefuse=$false;$script:uncertain=$false;$script:reason=0;$script:refusal=0;$script:tailHits=0;$script:injectUncertain=$false
function P77-Display([string]$State,[int]$Seconds=20){
    if($script:LifeState -eq $State){return}
    $script:LifeState=$State
    if($script:controls.p1277_panel -eq '1'){
        if($State -eq 'OFF'){
            if($script:preoffRefuse){
                $script:failed=1;$script:reason=2;$script:refusal=4;$script:controls.p1277_panel='0'
                return
            }
            if($script:injectUncertain){$script:uncertain=$true;$script:failed=1;$script:reason=8;$script:refusal=32;return}
            $script:offCount++;$script:offMode=$script:panelMode
        }
        else{$script:clean++;if($script:offMode -eq 2){$script:replays++}}
    }
}
function P77-Review([string]$Message,[int]$Seconds=15){
    Assert ($script:LifeState -eq 'ON') 'review OFF'
    if($script:reject -and $Message -like '*gray flash*'){throw 'simulated visual rejection'}
    if($script:rejectNormal -and $Message -like '*ordinary wake*'){throw 'simulated normal wake rejection'}
}
function P77-Wait([int]$Seconds,[string]$State){
    Assert ($script:LifeState -eq $State) 'wait state'
    [void](P77-Guard);$script:now=$script:now.AddSeconds($Seconds)
    if($State -eq 'ON' -and $script:controls.mali_irq -eq '1'){$script:waits+=5}
    if($State -eq 'ON' -and $script:controls.p1277_active -eq '1'){$script:tailHits+=2}
}
function P12-Query([string]$Name,[string]$Value='',[switch]$Cleanup){
    if($Value -ne ''){
        $script:writes.Add("$Name=$Value")
        if($Name -eq 'p1278_panel_mode'){
            Assert ($script:LifeState -eq 'ON') 'mode while OFF'
            Assert ($script:controls.p1277_auto_idle -eq '1') 'auto idle stays enabled'
            Assert ($Value -ne '2' -or ($script:reviewed -band 1)) 'deep before shallow visual'
            $script:panelMode=[int]$Value;return $Value
        }
        if($Name -eq 'p1278_panel_review'){
            if($Value -eq '2'){$script:failed=1;$script:reviewed=0;$script:controls.p1277_panel='0'}
            else{$script:reviewed=$script:reviewed -bor (1 -shl ($script:panelMode-1))}
            return $Value
        }
        Assert ($script:controls.ContainsKey($Name)) "unexpected write $Name"
        if($Name -eq 'p1277_auto_idle' -and $Value -eq '1'){Assert ($script:LifeState -eq 'OFF') 'Auto Idle must arm OFF'}
        $script:controls[$Name]=$Value;return $Value
    }
    if($script:controls.ContainsKey($Name)){return $script:controls[$Name]}
    $on=$script:LifeState -eq 'ON';$enabled=$script:controls.p1277_auto_idle -eq '1'
    $active=$enabled -and -not $on;$panel=$script:controls.p1277_panel -eq '1'
    $mode=if($active){69}else{0};$lease=if($active){123456}else{0};$floor=if($active){598000}else{1053000}
    switch($Name){
        p1273_boot_identity{return $script:boot}
        p1278_revision{return '81'}
        p1277_revision{return '7'}
        p12754_revision{return '6'}
        p12753_guard{return "version=1 enabled=1 at=1 mask=0 big=$script:temp lit=58000 gpu=59000 bat=36 age=100 slope=0 bcap=0 lcap=0 peak_big=59000 peak_lit=58000 changes=0"}
        p1277_auto_stats{return "version=1 permission=1 enabled=$([int]$enabled) state=$(if($active){4}else{3}) stage=5 samples=2 retries=0 abox_driver_ok=1 fault=0 last_uv=637500 last_mhz=598 last_reason=0 own_lease=$lease live_mode=$mode ufs_safe=1 interactive_qualification=0"}
        p127_status{return "mode=$mode ufs_safe=1 little_cur=$floor"}
        p126_status{return 'mode=0'}
        p12753_status{return 'state=0'}
        p1277_panel_stats{return "version=1 permission=1 request=$([int]$panel) persistent=$([int]$panel) failed=$script:failed asleep=$([int]($panel -and -not $on)) pending=0 sleep=$script:offCount latch29=$script:clean fallback=0 fault=0 sleep_verified=$([int]($panel -and -not $on -and $script:offMode -eq 2)) wake_uncertain=$([int]$script:uncertain)"}
        p1278_panel_stats{return "version=1 permission=1 mode=$script:panelMode off_mode=$script:offMode off_verified=$([int]($panel -and -not $on -and -not $script:uncertain)) off_count=$script:offCount replays=$script:replays clean=$script:clean reviewed=$script:reviewed reason=$script:reason first=11 done=12 diag=15 raw=00/00/00/00 id=910443 pending=0 failed=$script:failed refusal=$script:refusal drain=3 irq_raw=23040000"}
        p1278_irq_stats{return "version=1 enabled=$($script:controls.mali_irq) attached=$($script:controls.mali_irq) waits=$script:waits late=0 spins=100 naps=100 fallback=0 arm_fail=0 fault=0"}
        p1278_active_stats{return "version=1 active=$($script:controls.p1277_active) boost_ms=150 burst_ms=60 lit_touch=1248000 big_touch=1469000 big_frame=1261000 g2_request=$($script:controls.p1277_g2) g2_effective=$($script:controls.p1277_g2) policy=2 tail=$($script:controls.p1277_active) lit_tail=1053000 big_tail=1261000 lit_floor=0 big_floor=0 tail_hits=$script:tailHits tail_bypass=0"}
        p1277_gpu_dvs_stats{return 'version=1 active=0 failed=0 fault=0'}
        p1277_layer2_stats{return 'state=0 request=0 restored=1'}
        p1277_burst_stats{return 'request=0 trace_state=0'}
        default{throw "unexpected read $Name"}
    }
}
$root=Join-Path ([IO.Path]::GetTempPath()) ('p1278-host-'+[guid]::NewGuid().ToString('N'))
[void](New-Item -ItemType Directory -Path $root)
$Rev8Modules=@('AutoIdle','Panel','Active')
P1278-RunSuite (Join-Path $root 'full')
$r=Get-Content -Raw (Join-Path $root 'full.rev8-suite.json')|ConvertFrom-Json
Assert ($r.result -eq 'completed_driver_and_visual_pilots' -and $r.cleanup -eq 'verified' -and -not $r.power_verified) 'full suite result'
Assert ($r.completed.Count -eq 8 -and $script:reviewed -eq 3 -and $script:replays -eq 1) 'panel shallow/deep and all windows'
Assert ($script:controls.Values -notcontains '1') 'no control leak'
Assert (@($script:writes|Where-Object {$_ -eq 'p1277_auto_idle=1'}).Count -eq 1) 'one Auto Idle enable'
$Rev8Checkpoint=Join-Path $root 'full.rev8-checkpoint.json'
$script:writes.Clear()
P1278-RunSuite (Join-Path $root 'resume')
Assert ($script:writes.Count -eq 2) 'same-boot resume only auto enable/restore'
$script:boot='version=1 nonce=1111111111111111';$script:writes.Clear()
Refuse {P1278-RunSuite (Join-Path $root 'mismatch')} 'different boot'
Assert ($script:writes.Count -eq 0) 'wrong boot no writes'
$Rev8Checkpoint='';$script:boot='version=1 nonce=0123456789abcdef';$script:reject=$true;$Rev8Modules=@('Panel');$script:writes.Clear()
Refuse {P1278-RunSuite (Join-Path $root 'reject')} 'visual rejection'
Assert ($script:writes -contains 'p1278_panel_review=2' -and $script:writes -notcontains 'p1278_panel_mode=2') 'visual N retires before deep'
Assert ($script:controls.Values -notcontains '1') 'reject restored'
$script:failed=0;$script:reject=$false;$script:reviewed=0;$script:preoffRefuse=$true;$Rev8Modules=@('Panel','Active');$script:writes.Clear()
Refuse {P1278-RunSuite (Join-Path $root 'actual-preoff-refusal')} 'Panel refused safely before DCS'
$r=Get-Content -Raw (Join-Path $root 'actual-preoff-refusal.rev8-suite.json')|ConvertFrom-Json
Assert ($r.result -eq 'partial_panel_refused' -and $r.cleanup -eq 'verified' -and $r.panel_refusal.mask -eq '4') '060150 refusal report is partial but restored'
Assert ($r.completed -contains 'Combined' -and $r.completed -notcontains 'PanelShallow' -and $r.completed -notcontains 'PanelDeep') 'independent Active continues'
Assert ($script:writes -notcontains 'p1277_panel=0' -and $script:writes -notcontains 'p1278_panel_mode=2') 'no OFF disarm or deep writes'
Assert ($script:controls.Values -notcontains '1') 'safe refusal no control leak'
Assert (@($r.events|Where-Object step -eq 'already_retired').Count -eq 1) 'retired witness'
$script:preoffRefuse=$false;$script:rejectNormal=$true;$script:writes.Clear()
Refuse {P1278-RunSuite (Join-Path $root 'reject-normal')} 'normal wake rejection'
Assert ($script:writes -notcontains 'mali_irq=1') 'visual N stops independent Active'
$script:rejectNormal=$false;$script:failed=0;$script:reason=0;$script:refusal=0
# An uncertain DPU/TE restoration is never treated as a safe pre-off refusal.
# Begin clean, then inject the fault only at actual OFF.
$script:preoffRefuse=$false;$script:uncertain=$false;$Rev8Modules=@('Panel','Active');$script:writes.Clear()
$script:injectUncertain=$true
Refuse {P1278-RunSuite (Join-Path $root 'uncertain-panel')} 'Panel OFF witness failed; Rev8 cleanup uncertain'
$r=Get-Content -Raw (Join-Path $root 'uncertain-panel.rev8-suite.json')|ConvertFrom-Json
Assert ($r.error -eq 'Panel OFF witness failed' -and $r.cleanup -eq 'unconfirmed') 'preserve root cause separately from cleanup'
Assert ($script:writes -notcontains 'p1277_panel=0' -and $script:writes -notcontains 'mali_irq=1' -and $script:controls.p1277_auto_idle -eq '0') 'uncertain panel no further experiments, Auto Idle restored'
# Reset simulated device only.
$script:controls.p1277_panel='0';$script:injectUncertain=$false;$script:uncertain=$false;$script:reason=0;$script:refusal=0
$script:failed=0;$script:reject=$false;$script:temp=67000;$script:writes.Clear()
Refuse {P1278-RunSuite (Join-Path $root 'hot')} 'thermal stop'
Assert ($script:writes.Count -eq 0) 'hot refuses writes'
Write-Host "P1278.1 mocked host suite, same-boot resume, actual pre-off refusal, independent Active, visual rejection, uncertain cleanup/root error, thermal refusal PASS; output $root"
