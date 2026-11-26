#requires -Version 5.1
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../s9-p125-timer-serial.ps1') -Scenario FunctionalGate -AllowFunctionalGate -AllowCombined -LibraryOnly
function Check($ok,[string]$why){if(-not $ok){throw $why}}
Check $script:P1273HostEnabled 'gate scenario not enabled'
$reply='version=1 phase=0 mode=0 generation=5 qualified=31 missing=0 lease=0 expiry=0 faults=0'
Check (P12-ReplyShape 'p1273_gate_status' $reply $true) 'gate status shape'
$events=('0,'*14)+'0'
Check (P12-ReplyShape 'p1273_boot_identity' 'version=1 nonce=000000000000001a' $true) 'boot identity'
Check (-not (P12-ReplyShape 'p1273_boot_identity' 'version=1 nonce=0000000000000000' $true)) 'zero boot identity accepted'
Check (P12-ReplyShape 'p1273_gate_active' "version=1 phase=0 values=$events" $true) 'active shape'
Check (P12-ReplyShape 'p1273_gate_post' "version=1 phase=1 values=$events" $true) 'post shape'
Check (-not (P12-ReplyShape 'p1273_gate_post' "version=1 phase=0 values=$events" $true)) 'wrong phase accepted'
Check (-not (P12-ReplyShape 'p1273_gate_post' "version=1 phase=1 values=$events,0" $true)) 'wrong count accepted'
foreach($bad in @(@('p127_mode','903'),@('p127_mode','919'),@('p127_mode','1023'),@('p1273_gate_begin','903'),@('p1273_gate_ack','1'))){
    $refused=$false
    try{[void](P12-Query $bad[0] $bad[1])}catch{if($_.Exception.Message -match 'read-only|only permits'){$refused=$true}else{throw}}
    Check $refused "unsafe write $bad"
}
$script:qual=0;$script:phase=0
function P1273-GateStatus {return @{version='1';phase="$script:phase";qualified="$script:qual"}}
function ExpectDenied([int]$Mode,[bool]$Qualifying){
    $refused=$false
    try{P1273-CombinedPermission $Mode -Qualifying:$Qualifying}catch{$refused=$true}
    Check $refused "combined unexpectedly allowed $Mode qualifying=$Qualifying"
}
ExpectDenied 903 $true
$script:qual=31
P1273-CombinedPermission 903 -Qualifying
ExpectDenied 903 $false
ExpectDenied 919 $true
$script:qual=95
P1273-CombinedPermission 903
ExpectDenied 919 $true
$script:qual=127
P1273-CombinedPermission 919 -Qualifying
ExpectDenied 919 $false
$script:qual=255
P1273-CombinedPermission 919
$script:phase=2
ExpectDenied 903 $false
$script:phase=0
$AllowCombined=$false
ExpectDenied 903 $false
Write-Host 'P1273 typed gate and combined host prerequisite PASS (mock only)'
