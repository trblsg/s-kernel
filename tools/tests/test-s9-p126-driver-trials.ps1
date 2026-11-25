#requires -Version 5.1
Set-StrictMode -Version 2
$ErrorActionPreference='Stop'
. (Join-Path (Split-Path $PSScriptRoot -Parent) 's9-p125-timer-serial.ps1') -LibraryOnly
$script:trialMode=0
$script:trialReason=0
$script:trialHit=0
$script:trialFail=$false
$script:trialPhases=New-Object 'System.Collections.Generic.List[string]'
$script:trialWrites=New-Object 'System.Collections.Generic.List[string]'
function P126-Status {
    return @{version='1';mode="$script:trialMode";reason="$script:trialReason";haptic="$script:trialHit";wifi_worker='0';gnss='0';touch='0';touch_leeway='0';shub='0';lease_s='120';watchdog_ms='100'}
}
function P12-Query([string]$Name,[string]$Value='',[switch]$Cleanup){
    if($Name -ne 'p126_mode'){throw "unexpected fixture command $Name"}
    $script:trialWrites.Add("$Name=$Value")
    $script:trialMode=[int]$Value
    return $Value
}
function P125-PumpOff([int]$Seconds){}
function P125-ReadSnapshot([string]$Label,[switch]$CountsOnly){
    if(-not $CountsOnly){throw 'trial attempted detail dump'}
    return @{label=$Label}
}
function P125-Phase([string]$Label,[int]$Mode,[int]$Seconds,[string]$Stem,$Before,[switch]$CountsOnly){
    if(-not $CountsOnly -or $Mode -ne 1){throw 'trial attempted detail mode'}
    if($Label -match '-B$'){
        if($script:trialMode -eq 0){throw 'B not armed'}
        $script:trialHit+=100
        if($script:trialFail){$script:trialReason=4;$script:trialMode=0}
    }elseif($script:trialMode -ne 0){throw 'A not restored'}
    $script:trialPhases.Add($Label)
    return @{label=$Label}
}
$DriverModules='HAPTIC,WIFI'
P126-DriverTrials 'fixture'
if(($script:trialWrites -join ',') -ne 'p126_mode=1,p126_mode=0,p126_mode=2,p126_mode=0'){throw 'module mask/restore sequence'}
if(($script:trialPhases -join ',') -ne 'P126-A0,P126-HAPTIC-B,P126-HAPTIC-A1,P126-WIFI-B,P126-WIFI-A1'){throw 'shared stock baseline sequence'}
if($script:P126Dirty -or $script:trialMode -ne 0){throw 'restore not verified'}
if(-not ($script:P125Report -match 'MODULE WIFI NO_ELIGIBLE_PATH')){throw 'inactive module reported as successful'}
$DriverModules='HAPTIC,HAPTIC'
$failed=$false
try{P126-DriverTrials 'fixture'}catch{if($_.Exception.Message -match 'Invalid/duplicate'){$failed=$true}else{throw}}
if(-not $failed){throw 'duplicate modules accepted'}
$DriverModules='HAPTIC';$script:trialFail=$true;$script:trialReason=0
$failed=$false
try{P126-DriverTrials 'fixture'}catch{if($_.Exception.Message -match 'lease revoked'){$failed=$true}else{throw}}
if(-not $failed -or -not $script:P126Dirty){throw 'revoked lease did not trigger cleanup path'}
Write-Output 'P12.6 module selection, aggregate-only A/B/A, no-hit marking, restore and revocation PASS'
