#requires -Version 5.1
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../s9-p125-timer-serial.ps1') -LibraryOnly -P12754
function Check($ok,[string]$why){if(-not $ok){throw $why}}
$script:Port='COM125';$script:LifeSerial=$null;$script:LifeCurrentSegment=$null
$script:clock=[datetime]::UtcNow
$script:opens=0;$script:reads=0;$script:cursor=0;$script:failedOnce=$false;$script:wrongBoot=$false
$S=@{state='3';request='15';arm='100';start='101';end='102';count='2';reason='8';mask='256';warm='1'}
$CP=[pscustomobject]@{Nonce='version=1 nonce=1234567890abcdef'}
function Life-Now{$script:clock=$script:clock.AddMilliseconds(250);return $script:clock}
function Life-CheckSafety($Now){}
function P12-EarlyThermalGuard{}
function Life-SyncReadOnly{}
function Life-Say($Message){}
function P125-Open($Name){Check ($Name -eq 'COM125') 'reconnect changed port';$script:opens++}
function P12-Query($Name,$Value){
    if($Name -eq 'p12753_select'){$script:cursor=[int]$Value;return "$Value"}
    if($Name -eq 'p12753_page'){
        $script:reads++
        if(-not $script:failedOnce){$script:failedOnce=$true;throw [IO.IOException]::new('simulated USB handle loss')}
        return "version=1 select=$script:cursor phase=101"
    }
    if($Name -eq 'p1273_boot_identity'){if($script:wrongBoot){return 'other boot'};return $CP.Nonce}
    if($Name -eq 'p12753_status'){return (($S.Keys|ForEach-Object{"$_=$($S[$_])"}) -join ' ')}
    throw "Unexpected command $Name"
}
$r=P12754-FrozenRow $S $CP 1
Check ($r.select -eq '1' -and $script:opens -eq 1 -and $script:reads -eq 2) 'frozen row not recovered'
$script:wrongBoot=$true;$script:failedOnce=$false;$script:reads=0
$refused=$false
try{[void](P12754-FrozenRow $S $CP 1)}catch{if($_.Exception.Message -match 'boot identity changed'){$refused=$true}else{throw}}
Check ($refused -and $script:reads -eq 1) 'different boot was retried'
Write-Output 'P1277 frozen cursor reconnect and boot change refusal PASS (mock)'
