#requires -Version 5.1
Set-StrictMode -Version 2
$ErrorActionPreference='Stop'
$tools=Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
. (Join-Path $tools 's9-p122-rootcause-serial.ps1') -LibraryOnly -P123 -ClockCrossover
$script:LifeSynced=$true
$script:LifeState='ON';$script:LifeScene='HOME'
$script:MockNow=[datetime]::Parse('2026-11-24T20:55:27Z').ToUniversalTime()
function Life-Now {return $script:MockNow}
function P123-Guard {}
function Life-CheckSafety([datetime]$At) {}
function P12-EarlyThermalGuard {}
$script:LifeSerial=New-Object psobject
$script:LifeSerial | Add-Member -MemberType ScriptMethod -Name Write -Value {param($data,$offset,$len)}
$script:MockKind='reject'
$script:MockRearm=$false
$script:MockEpoch=$script:MockNow
$script:MockWrites=New-Object 'System.Collections.Generic.List[string]'
function Life-Pump {
    $script:MockNow=$script:MockNow.AddSeconds(1)
    $cmd=$script:P12Command
    if($cmd){
        $script:MockWrites.Add($cmd)
        Life-Feed ('usb> '+$cmd) (Life-Now)
        if($script:MockKind -eq 'reject'){
            Life-Feed ('usb: '+$cmd+': EPERM: not marked CTLFLAG_KERN (1)') (Life-Now)
            return
        }
        $name=$cmd.Substring(('sysctl kern.hakios.').Length).Split('=')[0]
        $value=if($cmd.Contains('=')){$cmd.Split('=')[-1]}else{'0'}
        Life-Feed ('usb: '+$cmd+': set (0)') (Life-Now)
        Life-Feed ('usb: kern.hakios.'+$name+' = '+$value) (Life-Now)
        if($name -eq 'hi_cmd' -and $value -eq '252'){
            $script:MockRearm=$true;$script:MockEpoch=Life-Now
            Life-Feed 'hi: auto=REARM up=146s from_usb=1' (Life-Now)
        }
        if($name -eq 'hi_cmd' -and $value -eq '201'){
            $script:MockRearm=$false
            Life-Feed 'hi: gate=usb-stop phase=unset' (Life-Now)
        }
        if($script:MockKind -eq 'idle' -and $name -eq 'p122_diag_mode'){
            Life-Feed 'p1231-control: requested=0 observed=1 applied_unknown=1' (Life-Now)
        }
        return
    }
    if($script:MockRearm){
        $elapsed=[int]((Life-Now)-$script:MockEpoch).TotalSeconds
        switch($elapsed){
            41 {Life-Feed 'hi: auto=baseline up=186s source=3 gate=0 duration_s=20' (Life-Now)}
            62 {Life-Feed 'hi: auto=cut1 up=207s source=3 gate=1 duration_s=20' (Life-Now)}
            83 {Life-Feed 'hi: auto=recover1 up=227s source=3 gate=0 duration_s=20' (Life-Now)}
            104 {Life-Feed 'hi: auto=cut2 up=248s source=3 gate=1 duration_s=20' (Life-Now)}
            124 {Life-Feed 'hi: auto=recover2 up=269s source=3 gate=0 duration_s=20' (Life-Now)}
            145 {Life-Feed 'hi: auto=FINISHED up=290s runs=2 controls_restored=1' (Life-Now)}
        }
    }
}
$started=Life-Now
try{[void](P12-Query 'p123_floor_mode' '1');throw 'accepted rejected command'}
catch{if($_.Exception.Message -notmatch 'sysctl rejected.*EPERM'){throw}}
if(((Life-Now)-$started).TotalSeconds -ge 12){throw 'explicit EPERM did not short-circuit'}
$script:MockKind='clock'
P123-ClockTrial
if($script:P123AutoPhase -ne 'FINISHED' -or -not $script:P12HiStopConfirmed -or
   $script:MockWrites[-1] -ne 'sysctl kern.hakios.hi_cmd=201'){throw 'early-boot Clock trial failed or not stopped'}
$old='unmapped=0 '+((0..7 | ForEach-Object {"c$($_):0/0/0/0/0/0/0"}) -join ' ')
$new='unmapped=0 '+((0..7 | ForEach-Object {"c$($_):100/0/0/0/100/0/100"}) -join ' ')
P1231-ValidateIdle $old $new
try{P1231-ValidateIdle $old ($old -replace 'unmapped=0','unmapped=79402');throw 'accepted unmapped WFI'}
catch{if($_.Exception.Message -notmatch 'mapping invalid'){throw}}
try{P1231-ValidateIdle $old $old;throw 'accepted zero entries'}
catch{if($_.Exception.Message -notmatch 'no mapped idle entries'){throw}}
$script:LifeState='OFF';$script:LifeStateSince=(Life-Now).AddSeconds(-35)
Life-Feed 'g3d-lab: up=184467ms phase=0 mode=2 domain=0 gpu_safe=0 tz_ok=0 pll=0 mux=0 clk=0MHz target_mv=950 rail_read=0 rail_valid=0 rail_mv=0 rail_ctrl=0 off_count=2 off_total_ms=73001 wake_count=1 wake_fail=0 wake_last_us=905 off_pending_ms=1239 shadow=0 denied=0 job_faults=0 fault=0 g3d_mc=64000' (Life-Now)
$off=$script:LifeBuckets['OFF_STABLE']
if($off.GpuLabOffCount -ne 1 -or $off.GpuBusyCount -ne 0 -or $off.GpuCount -ne 0){throw 'g3d-lab OFF parser wrong source accounting'}
$script:LifeState='ON';$script:LifeScene='HOME'
$MicroGesture=$true
$script:MockKind='idle'
function P123-FloorSnapshot([string]$Label){'version=1 active=0 configured_floor=450 quiet=1 display=1 streak=32 gap_ms=100 eligible=0 bypass=0 presents=123 last_present_ticks=10 now_ticks=11'}
P122-Switch 0
if(-not $script:P123DeferredMode0 -or $script:P122Ack -or -not $script:P123Control0){throw 'deferred request confused with applied mode ACK'}
Life-Feed '2026-11-24 19:40:53.347480+0000 backboardd[18:2667] [TouchEvents] contact 1 presence: touching' (Life-Now)
$script:MockNow=$script:MockNow.AddMilliseconds(215)
Life-Feed '2026-11-24 19:40:53.562397+0000 backboardd[18:2697] [TouchEvents] contact 1 presence: none' (Life-Now)
if($script:P123TouchCount -ne 1 -or ($script:P123TouchUp-$script:P123TouchDown).TotalMilliseconds -ne 215){throw 'actual touch parser failed'}
try{[void](P12-Query 'p1231_hash_mode' '2' -Cleanup);throw 'unsafe hash cleanup'}
catch{if($_.Exception.Message -notmatch 'Cleanup bypass'){throw}}
Life-Feed 'p1231-hash: applied=2 context=0x1234' (Life-Now)
Life-Feed 'p1231-hash: window mode=2 fallback=0 context=0x1234 frames_end=60' (Life-Now)
if($script:P123HashApplied -ne 2 -or $script:P123HashWindows -ne 1){throw 'hash applied/window parser failed'}
Life-Feed 'p1231-hash: window mode=2 fallback=1 context=0x1234 frames_end=120' (Life-Now)
if(-not $script:P123HashFallback){throw 'hash verification fallback lost'}
Write-Output 'P12.3.1 PASS: EPERM immediate, early-boot Clock deadline+STOP, WFI mapped guard, independent G3D OFF, deferred ACK, touch event, hash fallback'
