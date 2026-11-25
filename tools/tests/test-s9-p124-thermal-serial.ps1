#requires -Version 5.1
# P12.4 mock serial and guard fixtures. Never open a physical COM port.
Set-StrictMode -Version 2
$ErrorActionPreference='Stop'
$tools=Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
. (Join-Path $tools 's9-p122-rootcause-serial.ps1') -LibraryOnly -P124 -ClockCadence
if(-not $P123 -or -not $script:P124HostEnabled -or -not $script:P124ClockAllowed){throw 'P124 opt-in not propagated'}
$script:LifeSynced=$true
$script:LifeState='ON';$script:LifeScene='HOME'
$script:MockNow=[datetime]::Parse('2026-11-25T04:17:35Z').ToUniversalTime()
function Life-Now {return $script:MockNow}
function P123-Guard {}
function Life-CheckSafety([datetime]$At) {}
function P12-EarlyThermalGuard {}
$script:LifeSerial=New-Object psobject
$script:LifeSerial|Add-Member -MemberType ScriptMethod -Name Write -Value {param($data,$offset,$len)}
$script:MockWrites=New-Object 'System.Collections.Generic.List[string]'
$script:MockCpu=0;$script:MockHash=0;$script:MockEvaluated=0
$script:MockClock=$false;$script:MockHot=$false
function Life-Pump {
    $script:MockNow=$script:MockNow.AddSeconds(1)
    $cmd=$script:P12Command
    if(-not $cmd){return}
    $script:MockWrites.Add($cmd)
    Life-Feed ('usb> '+$cmd) (Life-Now)
    $name=$cmd.Substring(('sysctl kern.hakios.').Length).Split('=')[0]
    $value=if($cmd.Contains('=')){$cmd.Split('=')[-1]}else{'0'}
    if($name -eq 'p124_cpu_mode'){
        $script:MockCpu=[int]$value
    }elseif($name -eq 'p124_cpu_stats'){
        $value="version=1 active=$script:MockCpu reason=0 effective=$script:MockEvaluated bypass=0"
    }elseif($name -eq 'p1231_ufs_stats'){
        $value='version=1 nufs=1 switched=1 loader_gone=1 floor_safe=1 native_inflight=0'
    }elseif($name -match '^p124_irq([0-7])$'){
        $value="version=1 cpu=$($Matches[1]) phys_valid=1 sgi=10 ppi=100 spi=20 vtimer=100"
    }elseif($name -eq 'p1231_hash_mode'){
        if($cmd.Contains('=')){$script:MockHash=[int]$value}else{$value="$script:MockHash"}
        if($cmd.Contains('=')){Life-Feed "p1231-hash: applied=$value context=0x1" (Life-Now)}
    }
    Life-Feed ('usb: '+$cmd+': set (0)') (Life-Now)
    Life-Feed ('usb: kern.hakios.'+$name+' = '+$value) (Life-Now)
    if($name -eq 'hi_cmd' -and $value -eq '253'){
        $script:MockClock=$true
        Life-Feed 'p124-clock: armed=1 max_seconds=25 exact_class_timer_only=1 visible_cadence_trial=1' (Life-Now)
    }
    if($name -eq 'hi_cmd' -and $value -eq '201'){
        $script:MockClock=$false
        Life-Feed 'hi: gate=usb-stop phase=unset' (Life-Now)
    }
}
function P12-Observe([string]$Label,[int]$Seconds,[string]$Prompt){
    $script:MockNow=$script:MockNow.AddSeconds($Seconds)
    if($script:MockHot){throw 'EARLY_THERMAL_STOP mock'}
    if($script:MockCpu){$script:MockEvaluated+=100}
    if($script:MockClock){Life-Feed 'p124-clock: active=1 cuts=140 display=1' (Life-Now)}
    if($Label -match '^HASH_'){
        Life-Feed "p124-hash: mode=$script:MockHash migrations=0 verify_budget_skipped=1 fallback=0 images=20 bytes=1234" (Life-Now)
        Life-Feed "p124-hash: mode=$script:MockHash migrations=0 verify_budget_skipped=1 fallback=0 images=20 bytes=1234" (Life-Now)
    }
}
function P122-Switch([int]$Mode,[switch]$FirstMinimal) {}
function P122-Snapshot([string]$Label){'version=3 mode=2 enabled=1'}
P124-CpuProfile 'LITTLE_A0' 0
P124-CpuProfile 'LITTLE_B' 1
if($script:MockCpu -ne 0 -or $script:P124CpuDirty){throw 'LITTLE lease not restored'}
$script:MockHot=$true
try{P124-CpuProfile 'LITTLE_B' 2;throw 'hot trial accepted'}
catch{if($_.Exception.Message -notmatch 'EARLY_THERMAL_STOP'){throw}}
if($script:MockCpu -ne 0 -or $script:P124CpuDirty){throw 'hot CPU cleanup failed'}
$script:MockHot=$false
P124-ClockTrial
if($script:MockClock -or -not $script:P12HiStopConfirmed){throw 'Clock restore not confirmed'}
P124-HashTrial
if($script:MockHash -ne 0 -or $script:P123HashDirty){throw 'hash lease not restored'}
if($script:MockWrites -contains 'sysctl kern.hakios.p1231_hash_mode=1'){throw 'hot scalar reintroduced'}
$script:P124ExpectedHash=2
$script:P124HashStable=0
Life-Feed 'p124-hash: mode=0 migrations=0 verify_budget_skipped=0 fallback=0 images=20 bytes=1' (Life-Now)
Life-Feed 'p124-hash: mode=2 migrations=7 verify_budget_skipped=0 fallback=0 images=20 bytes=1' (Life-Now)
if($script:P124HashStable -ne 0){throw 'straddling/wrong mode hash window accepted'}
Life-Feed 'p124-hash: mode=2 migrations=0 verify_budget_skipped=1 fallback=0 images=20 bytes=1' (Life-Now)
if($script:P124HashStable -ne 1){throw 'stable hash window lost'}
P124-IrqSnapshot 'IRQ_OFF_STABLE_BEGIN'
foreach($i in 0..7){if($script:MockWrites -notcontains "sysctl kern.hakios.p124_irq$i"){throw "missing IRQ $i"}}
try{[void](P12-Query 'p124_cpu_mode' '4' -Cleanup);throw 'nonzero cleanup accepted'}
catch{if($_.Exception.Message -notmatch 'Cleanup bypass'){throw}}
try{[void](P12-Query 'p124_cpu_mode' '5');throw 'unsupported CPU mode accepted'}
catch{if($_.Exception.Message -notmatch 'only permits'){throw}}
try{[void](P12-Query 'p124_irq0' '1');throw 'write to read-only counter accepted'}
catch{if($_.Exception.Message -notmatch 'only permits'){throw}}
Write-Output 'P12.4 PASS: separate CPU and Clock lease, hot rollback, no scalar, stable hash filtering, IRQ read-only, safe allowlist'
