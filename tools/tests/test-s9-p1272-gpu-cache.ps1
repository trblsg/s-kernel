#requires -Version 5.1
Set-StrictMode -Version 2
$ErrorActionPreference='Stop'
. (Join-Path (Split-Path $PSScriptRoot -Parent) 's9-p125-timer-serial.ps1') -LibraryOnly
$script:req=0;$script:done=$false;$script:missing=$false;$script:bad=$false
$script:writes=New-Object 'System.Collections.Generic.List[string]'
$script:fakeNow=[datetime]::UtcNow
function Life-Now {$script:fakeNow=$script:fakeNow.AddSeconds(1);return $script:fakeNow}
function P125-Status{return @{armed='0'}}
function P126-Status{return @{mode='0'}}
function P127-Status{return @{mode='0'}}
function P12-Query([string]$Name,[string]$Value='',[switch]$Cleanup){
    switch($Name){
        'p1272_gpu_caps' {
            $h=if($script:done){100}else{0};$p=if($script:done){1}else{0};$denied=if($script:bad){1}else{0}
            return "version=1 ready=1 mask=1ffffffffff hits=$h off_hits=$h param2=$p off_param2=$p reject=0 denied=$denied live=0"
        }
        'p1272_probe_state' {
            $wake=if($script:done){1}else{0};$off=$wake+1
            return "version=1 eligible=1 display=0 domain=0 pll=0 mux=0 wake=$wake off=$off wake_fail=0 fault=0 safe=0 tz=0"
        }
        'p1272_probe_request' {
            if($Value -ne ''){$script:req=[int]$Value;$script:writes.Add($Value)}
            return "$script:req"
        }
        default {throw "unexpected command $Name"}
    }
}
function P125-PumpOff([int]$Seconds){
    if($script:req -eq 1 -and -not $script:done -and -not $script:missing){
        $script:done=$true
        Life-Feed 'p1272-probe: OFF_GET_PARAM PASS count=41 domain=0 pll=0 mux=0 wake=0 off=1' (Life-Now)
        Life-Feed 'p1272-probe: COMPLETE PASS wake=0->1 domain=15 safe=1 tz=1 caps_equal=1 job_good=1' (Life-Now)
    }
}
P1272-Check
if(($script:writes -join ',') -ne '1,0' -or $script:P1272Dirty -or -not ($script:P125Report -match 'GPU_CACHE_CHECK PASS')){throw 'qualification/cancel proof'}
$script:done=$false;$script:missing=$true;$script:writes.Clear()
$failed=$false;try{P1272-Check}catch{if($_.Exception.Message -match 'missing fresh OFF/real-job proof'){$failed=$true}else{throw}}
if(-not $failed -or -not $script:P1272Dirty -or ($script:writes -join ',') -ne '1'){throw 'missing proof accepted or repeated request'}
$script:req=0;$script:bad=$true
$failed=$false;try{P1272-Caps}catch{if($_.Exception.Message -match 'MMIO guard check failed'){$failed=$true}else{throw}}
if(-not $failed){throw 'MMIO denied accepted'}
if(-not (P12-ReplyShape 'p1272_gpu_caps' 'version=1 ready=1 mask=1ffffffffff hits=2 off_hits=1 param2=1 off_param2=1 reject=0 denied=0 live=0' $true)){throw 'mixed caps shape'}
if(P12-ReplyShape 'p1272_gpu_caps' 'version=1 ready=1 mask=1ffffffffff hits=2' $true){throw 'truncated caps accepted'}
Write-Output 'P12.7.2 host fresh GET_PARAM/wake/re-gate proof, one-shot request, missing/denied fail-closed PASS'
