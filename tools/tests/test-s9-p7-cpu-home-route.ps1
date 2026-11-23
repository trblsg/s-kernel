#requires -Version 5.1
# Host-only proof that the real P7 CPU path uses the quiet Home route.
# The previous Settings URL request failed on device with:
# hi: settings_open REFUSED mode=1 auto_state=aborted need_finished=1
# Do not run COM, hardware power transitions, or actual scene navigation.
Set-StrictMode -Version 2
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\s9-p7-power-ab.inc.ps1')
$script:HomeAttempts=0
$script:PhaseAttempts=@()
$script:MockHome=$true
$script:MockBigOnline=4
$script:SceneHome=$false
$script:SceneSettings=$false
$script:LastCpuSample=$null
function Say([string]$Message){ }
function P7-Open-QuietHome{
    $script:HomeAttempts++
    $script:SceneHome=$script:MockHome
    $script:SceneSettings=$false
}
function P7-Read-G3DStats{
    return @{
        domain=1; pll=1; gpu_safe=1; tz_ok=1; mode=0
        fault=0; wake_fail=0; requested_mv=950
        rail_valid=1; rail_last_mv=950
    }
}
function P7-Set-Phase([int]$Phase){
    $script:PhaseAttempts+= $Phase
    throw 'REACHED_P7_CPU_PHASE_60'
}
function Assert-CPU-Route([bool]$HomeReady,[int]$BigOnline,[string]$Expected){
    $script:HomeAttempts=0
    $script:PhaseAttempts=@()
    $script:MockHome=$HomeReady
    $script:MockBigOnline=$BigOnline
    $script:SceneHome=$false
    $script:SceneSettings=$false
    $script:LastCpuSample=[pscustomobject]@{
        Display=1; LittleOnline=4; BigOnline=$BigOnline
    }
    $msg=''
    try{
        P7-Run-CPUAB
        throw 'UNEXPECTED_RETURN_FROM_CPU_AB'
    }catch{$msg=$_.Exception.Message}
    if($msg -notlike ('*'+$Expected+'*')){
        throw ('Unexpected CPU path: expected '+$Expected+' got '+$msg)
    }
    if($script:HomeAttempts -ne 1){
        throw 'CPU path did not visit Home verification exactly once'
    }
    if($Expected -eq 'REACHED_P7_CPU_PHASE_60'){
        if($script:PhaseAttempts.Count -ne 1 -or $script:PhaseAttempts[0] -ne 60){
            throw 'CPU A/B did not enter phase60 directly from quiet Home'
        }
    }elseif($script:PhaseAttempts.Count -ne 0){
        throw 'CPU phase advanced without Home/8-online safety proof'
    }
}
Assert-CPU-Route $true 4 'REACHED_P7_CPU_PHASE_60'
Assert-CPU-Route $false 4 'secondary Home foreground proof was lost'
Assert-CPU-Route $true 3 'all eight cores online'
Write-Output 'P7 CPU QUIET HOME ROUTE PASS: accepted Home+8 cores, rejected unverified Home and 3/4 BIG; no Settings command or COM.'
