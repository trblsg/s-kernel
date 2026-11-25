#requires -Version 5.1
Set-StrictMode -Version 2
$ErrorActionPreference='Stop'
. (Join-Path (Split-Path $PSScriptRoot -Parent) 's9-p125-timer-serial.ps1') -LibraryOnly
$at=[datetime]::UtcNow
function Reset-Reply([string]$Cmd){
    $script:P12Command=$Cmd;$script:P12Reply=$null;$script:P12Echo=$false;$script:P12Refusal=$null
}
$cmd='sysctl kern.hakios.p6_usb_connected_ticks'
Reset-Reply $cmd
Life-Feed '2026-11-25 16:28:33.235 wifid[15:1066] Error: Failed to set locale, retrying US usb: kern.hakios.p6_usb_connected_ticks = 80464' $at
if($null -ne $script:P12Reply){throw 'unsolicited mixed reply accepted'}
Life-Feed "usb> $cmd=0" $at
if($script:P12Echo){throw 'prefix echo incorrectly matched'}
Life-Feed "usb> $cmd" $at
Life-Feed 'usb: kern.hakios.p6_usb_ticks = 80500' $at
Life-Feed 'usb: kern.hakios.p6_usb_connected_ticks = 80464 trailing text' $at
Life-Feed 'usb: kern.hakios.p6_usb_connected_ticks = 18446744073709551616' $at
if($null -ne $script:P12Reply){throw 'wrong name, malformed suffix or overflow accepted'}
Life-Feed '2026-11-25 16:28:33.235 wifid[15:1066] Error: Failed to set locale, retrying US usb: kern.hakios.p6_usb_connected_ticks = 80464' $at
if($script:P12Reply -ne '80464'){throw '090722 mixed readback not recovered'}
Life-Feed 'usb: kern.hakios.p6_usb_connected_ticks = 80465' $at
if($null -eq $script:P12Refusal){throw 'conflicting reply not rejected'}
Reset-Reply 'sysctl kern.hakios.p127_wfi3'
Life-Feed 'usb> sysctl kern.hakios.p127_wfi3' $at
Life-Feed 'log usb: kern.hakios.p127_wfi3 = version=1 cpu=3 sgi=0 ppi=1 spi=0 bins=0/0,1/ff,0/0,0/0,0/0,0/0' $at
if($null -ne $script:P12Reply){throw 'truncated WFI accepted'}
Life-Feed 'log usb: kern.hakios.p127_wfi3 = version=1 cpu=3 sgi=0 ppi=1 spi=0 bins=0/0,1/ff,0/0,0/0,0/0,0/0,0/0' $at
if($null -eq $script:P12Reply){throw 'mixed WFI rejected'}
Reset-Reply 'sysctl kern.hakios.p127_mode=3'
Life-Feed 'usb> sysctl kern.hakios.p127_mode=3' $at
Life-Feed 'log usb: sysctl kern.hakios.p127_mode=3: set (1)' $at
if($null -eq $script:P12Refusal){throw 'mixed refusal not recognized'}
if(-not (P12-ReplyShape 'p127_fg0' 'version=1 page=0 r=1/100/-450000/-460000/4000000/0/0/12' $true)){throw 'signed FG rejected'}
if(P12-ReplyShape 'p127_fg0' 'version=1 page=0 r=1/100/-450000/-460000/4000000/0/0' $true){throw 'truncated FG accepted'}
# Fake monotonic clock and serial. First READ reply missing, retry must
# require its own echo. A WRITE missing reply is sent exactly once.
$script:tick=$at;$script:writes=0;$script:sent=0;$script:failWrite=$false
function Life-Now {$script:tick=$script:tick.AddMilliseconds(500);return $script:tick}
function Life-CheckSafety([datetime]$Now){}
$script:LifeSerial=New-Object psobject
$script:LifeSerial|Add-Member -MemberType ScriptMethod -Name Write -Value {param($b,$start,$len) $script:writes++;$script:sent=0}
function Life-Pump {
    if(-not $script:P12Command){return}
    if($script:sent){return}
    $script:sent=1
    if($script:failWrite){return}
    if($script:writes -eq 1){
        Life-Feed "usb> $($script:P12Command)" (Life-Now)
        return
    }
    Life-Feed 'usb: kern.hakios.p6_usb_connected_ticks = 111' (Life-Now)
    if($null -ne $script:P12Reply){throw 'retry accepted stale reply before echo'}
    Life-Feed "usb> $($script:P12Command)" (Life-Now)
    Life-Feed 'wifid: note usb: kern.hakios.p6_usb_connected_ticks = 222' (Life-Now)
}
$script:P12ReadPaceMs=60;$script:P12ReadAttempts=2
if((P12-Query 'p6_usb_connected_ticks') -ne '222' -or $script:writes -ne 2){throw 'bounded READ retry failed'}
$script:failWrite=$true;$script:writes=0
$failed=$false
try{[void](P12-Query 'p127_mode' '3')}catch{if($_.Exception.Message -match 'Missing echo'){$failed=$true}else{throw}}
if(-not $failed -or $script:writes -ne 1){throw 'WRITE was retried or accepted without ACK'}
Write-Output 'P12.7.1 exact 090722 mixed reply, strict typed readback, stale echo, bounded read retry and single write PASS'
