#requires -Version 5.1
# P12.4.1 admission and combined orchestration; no real COM port.
Set-StrictMode -Version 2
$ErrorActionPreference='Stop'
$tools=Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
. (Join-Path $tools 's9-p122-rootcause-serial.ps1') -LibraryOnly -P124 -QuietSuite -IncludeIdle -WfiProbe
if(-not $script:P124ClockAllowed -or -not $IncludeIdle -or -not $WfiProbe){throw 'suite flags lost'}
function Sample([double]$b,[double]$l,[double]$g){
    [pscustomobject]@{type='CPU';display='ON';big_c=$b;little_c=$l;gpu_temp_c=$g}
}
$stable=@((Sample 67 65.6 65),(Sample 67 65.6 65),(Sample 67 65.6 65),(Sample 67 65.6 65))
if(-not (P1241-ThermalReady Quiet $stable 67 65.6 65)){throw 'real refused warm floor baseline still rejected'}
if(-not (P1241-ThermalReady Hash $stable 67 65.6 65)){throw 'warm fast hash baseline rejected'}
if(P1241-ThermalReady Gesture $stable 67 65.6 65){throw 'gesture margin bypassed'}
if(P1241-ThermalReady Quiet $stable 67 67.1 65){throw 'hot quiet arm accepted'}
if(P1241-ThermalReady Quiet @($stable[0]) 67 65.6 65){throw 'missing samples accepted'}
$rising=@((Sample 65 64 64),(Sample 65 64 64),(Sample 67 66 65),(Sample 67 66 65))
if(P1241-ThermalReady Quiet $rising 67 66 65){throw 'rising quiet baseline accepted'}
$script:P12HiStopConfirmed=$true
$script:P122CurrentMode=0
$script:order=New-Object 'System.Collections.Generic.List[string]'
$script:skipHash=$true;$script:failGpu=$false
function P1241-RequireReady([string]$Kind){
    if($Kind -eq 'Hash' -and $script:skipHash){throw 'P1241_SKIP Hash headroom'}
}
function P12-Query([string]$Name,[string]$Value='',[switch]$Cleanup){
    if($Name -eq 'p1231_ufs_stats'){'loader_gone=1 floor_safe=1 native_inflight=0'}
    else{throw "unexpected mock query $Name"}
}
function P124-CpuProfile([string]$Label,[int]$Mode){$script:order.Add($Label)}
function P123-FloorProfile([string]$Name,[int]$Mode){
    $script:order.Add($Name)
    if($script:failGpu){throw 'EARLY_THERMAL_STOP mock'}
}
function P124-ClockTrial {$script:order.Add('CLOCK')}
function P124-HashTrial {$script:order.Add('HASH')}
P1241-QuietSuite
if(($script:order -join ',') -ne 'LITTLE_A0,LITTLE_B,LITTLE_A1,FLOOR_A0,FLOOR_B,FLOOR_A1,CLOCK'){throw 'suite ordering or skip broken'}
if($script:P1241SuiteResults.Count -ne 4 -or $script:P1241SuiteResults[-1] -notmatch '^MODULE HASH SKIPPED'){throw 'partial suite incorrectly reported'}
$script:order.Clear();$script:P1241SuiteResults.Clear();$script:failGpu=$true
try{P1241-QuietSuite;throw 'thermal stop ignored'}catch{if($_.Exception.Message -notmatch 'EARLY_THERMAL_STOP'){throw}}
if($script:order -contains 'CLOCK' -or $script:order -contains 'HASH'){throw 'continued after thermal stop'}
if($script:P1241SuiteResults[-1] -notmatch '^MODULE GPU FAILED'){throw 'failure status not recorded'}
Write-Output 'P12.4.1 PASS: real warm baseline, trend, gesture margin, suite order, skip, hard abort'
