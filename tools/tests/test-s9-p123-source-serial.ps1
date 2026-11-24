#requires -Version 5.1
Set-StrictMode -Version 2
$ErrorActionPreference='Stop'
$tools=Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$runner=Join-Path $tools 's9-p122-rootcause-serial.ps1'
$preview=& powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $runner -P123 -AdaptiveFloor 2>&1 | Out-String
if($LASTEXITCODE -ne 0 -or $preview -notmatch 'P12.3 PREVIEW' -or $preview -notmatch '30s kernel expiry'){
    throw "P12.3 preview missing $preview"
}
# Dot-source the same functions, never open a physical serial port.
. $runner -LibraryOnly -P123 -AdaptiveFloor
if(-not $script:P123HostEnabled -or $script:P123ClockAllowed){throw 'Wrong P12.3 permissions'}
$script:LifeSynced=$true
$script:LifeState='ON';$script:LifeScene='HOME'
$base=Life-Now
$script:LifeLastCpuAt=$base;$script:LifeLastGpuAt=$base
$script:LifeLastBig=67;$script:LifeLastLittle=66;$script:LifeLastGpuC=66
Life-Feed 'p123-clock-window: ticks_now=100 first_ticks=90 last_ticks=99 forwarded=52 commits=50 input_ticks=42 home=1 source=window_not_frame_id' $base
Life-Feed 'p123-ca-window: first_ticks=90 last_ticks=110 renders=60 source=window_not_frame_id' $base
Life-Feed 'p123-gl-window: first_ticks=95 last_ticks=115 frames=60 mode=0 shadow_bind_attempts=200 cache_same=90 cache_invalid=10 raw_binds=50 native_skipped=0 source=window_not_frame_id' $base
if($script:P123SourceLines.Count -ne 3 -or $script:P123ClockInput -ne 42 -or
   $script:P123GlAt -eq [datetime]::MinValue -or $script:P123CaAt -eq [datetime]::MinValue){
    throw 'P12.3 live source marker parsing failed'
}
$script:P123ExpectedInput=42
P123-Guard
$script:P123ClockInput=43
try{P123-Guard;throw 'accepted changed input'}catch{if($_.Exception.Message -notmatch 'user input'){throw}}
$script:P123ClockInput=42
$script:LifeState='OFF'
try{P123-Guard;throw 'accepted OFF'}catch{if($_.Exception.Message -notmatch 'scene changed'){throw}}
$script:LifeState='ON'
$script:LifeLastGpuAt=$base.AddSeconds(-20)
try{P123-Guard;throw 'accepted stale GPU'}catch{if($_.Exception.Message -notmatch 'telemetry stale'){throw}}
$script:LifeLastGpuAt=Life-Now
$script:LifeLastLittle=70
try{P123-Guard;throw 'accepted early heat'}catch{if($_.Exception.Message -notmatch 'EARLY_THERMAL_STOP'){throw}}
$script:LifeLastLittle=66
# Mock exact USB echo + readback, including restore while already hot.
$script:LifeSerial=New-Object psobject
$script:LifeSerial | Add-Member -MemberType ScriptMethod -Name Write -Value {param($data,$offset,$len) $null=$data;$null=$offset;$null=$len}
$script:TestWrites=New-Object 'System.Collections.Generic.List[string]'
function Life-Pump {
    if(-not $script:P12Command){return}
    $cmd=$script:P12Command
    $name=$cmd.Substring(('sysctl kern.hakios.').Length).Split('=')[0]
    $value=if($cmd.Contains('=')){$cmd.Split('=')[-1]}else{'0'}
    $script:TestWrites.Add($cmd)
    Life-Feed ('usb> '+$cmd) (Life-Now)
    Life-Feed ('usb: kern.hakios.'+$name+' = '+$value) (Life-Now)
}
try{[void](P12-Query 'mali_gov_frame' '260');throw 'accepted raw clock write'}catch{if($_.Exception.Message -notmatch 'disallowed'){throw}}
try{[void](P12-Query 'p123_floor_mode' '2');throw 'accepted mode2'}catch{if($_.Exception.Message -notmatch 'only permits'){throw}}
try{[void](P12-Query 'p123_floor_mode' '1' -Cleanup);throw 'accepted nonzero cleanup'}catch{if($_.Exception.Message -notmatch 'Cleanup bypass'){throw}}
try{[void](P12-Query 'hi_cmd' '252');throw 'accepted Clock rearm in GPU test'}catch{if($_.Exception.Message -notmatch 'only permits'){throw}}
[void](P12-Query 'p123_floor_mode' '1')
$script:LifeLastBig=84
[void](P12-Query 'p123_floor_mode' '0' -Cleanup)
if($script:TestWrites[-1] -ne 'sysctl kern.hakios.p123_floor_mode=0'){throw 'hot restore not acknowledged'}
$script:LifeLastBig=67
$script:P123HostEnabled=$false
try{[void](P12-Query 'p123_floor_mode' '1');throw 'legacy runner enabled floor'}catch{if($_.Exception.Message -notmatch 'disallowed'){throw}}
$script:P123HostEnabled=$true
# Force a controlled observation failure AFTER the arm, prove immediate restore.
function P123-Guard {}
function P123-FloorSnapshot([string]$Label){
    'version=1 active=0 configured_floor=450 quiet=1 display=1 streak=8 gap_ms=100 eligible=0 bypass=0 presents=100 last_present_ticks=10 now_ticks=11'
}
function P122-Snapshot([string]$Label){'mock'}
function P12-Observe([string]$Phase,[int]$Seconds,[string]$Instruction){throw 'mock thermal abort'}
try{P123-FloorProfile 'FLOOR_B' 1;throw 'accepted mock abort'}catch{if($_.Exception.Message -notmatch 'mock thermal abort'){throw}}
if($script:P123FloorDirty -or $script:TestWrites[-1] -ne 'sysctl kern.hakios.p123_floor_mode=0'){
    throw 'P12.3 adaptive floor exception did not restore'
}
# Positive independent A/B/A; equal workload and reduced residency.
function P123-FloorSnapshot([string]$Label){
    $active=0;$eligible=0;$presents=100
    if($Label -match '_END$'){$presents=220}
    if($Label -eq 'FLOOR_B_END'){$active=1;$eligible=120}
    "version=1 active=$active configured_floor=450 quiet=1 display=1 streak=8 gap_ms=100 eligible=$eligible bypass=0 presents=$presents last_present_ticks=10 now_ticks=11"
}
function P122-Snapshot([string]$Label){
    $end=$Label -match '_END$'
    $low=$Label -match '^FLOOR_B'
    if(-not $end){
        'version=2 mode=0 windows=100 busy_ns=100000000 elapsed_ns=10000000000 frames=100 forced=100 level260_ns=0 level455_ns=10000000000 level598_ns=0 other_ns=0 jobs=600 queue_us=36000 run_us=200000'
    }else{
        $residency=if($low){12000000000L}else{22000000000L}
        $level260=22000000000L-$residency
        $forced=if($low){110}else{220}
        "version=2 mode=0 windows=220 busy_ns=340000000 elapsed_ns=22000000000 frames=220 forced=$forced level260_ns=$level260 level455_ns=$residency level598_ns=0 other_ns=0 jobs=1320 queue_us=79200 run_us=440000"
    }
}
function P12-Observe([string]$Phase,[int]$Seconds,[string]$Instruction){
    $script:P12Phases.Add([pscustomobject]@{Name=$Phase;Fps=12;Refresh=720;Presented=120})
    for($i=0;$i -lt 2;$i++){$script:P122GlWindows.Add([pscustomobject]@{Phase=$Phase;Mode=0;P95Bin=4})}
}
P123-FloorProfile 'FLOOR_A0' 0
P123-FloorProfile 'FLOOR_B' 1
P123-FloorProfile 'FLOOR_A1' 0
if($script:P123FloorDirty -or -not ($script:P123FloorRows -match 'RESULT FLOOR_B mode=1.*eligible_delta=120.*residency455_pct=16.7')){
    throw 'P12.3 positive A/B/A summary failed'
}
# Re-arm is permitted ONLY inside the explicit Clock test.
$script:P123ClockAllowed=$true
[void](P12-Query 'hi_cmd' '252')
if(-not $script:P12HiDirty){throw 'Clock rearm rollback not marked dirty'}
Life-Feed 'hi: auto=REARM up=305s from_usb=1' (Life-Now)
Life-Feed 'hi: auto=FINISHED up=409s runs=2 controls_restored=1' (Life-Now)
if(-not $script:P123AutoRearmed -or $script:P123AutoPhase -ne 'FINISHED'){throw 'Clock crossover completion not parsed'}
[void](P12-Query 'hi_cmd' '0' -Cleanup)
Write-Output 'P12.3 SOURCE PASS: preview, live source evidence, strict scene/input/thermal gates, mode allowlist, hot restore, exception rollback, separate Clock rearm'
