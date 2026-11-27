#requires -Version 5.1
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../s9-p125-timer-serial.ps1') -LibraryOnly -P12754
function Check($ok,[string]$why){if(-not $ok){throw $why}}
function Refuse([scriptblock]$action,[string]$pattern){$refused=$false;try{& $action}catch{if($_.Exception.Message -match $pattern){$refused=$true}else{throw}};Check $refused "missing refusal $pattern"}
Check (P12-ReplyShape 'p12754_wait' 'version=1 enabled=1 preflight=0 wait=512 seen=520 quiet_ticks=123 input_seen=1 fingers=0' $true) 'wait shape'
Check (P12-ReplyShape 'p12754_rate' 'version=1 fresh=1 cur=1053000 meas=1049' $true) 'rate shape'
Check (-not (P12-ReplyShape 'p12754_wait' 'version=1 enabled=1 preflight=0 wait=0' $true)) 'truncated wait'
Check ((P1273-GateBit 33) -eq 1024 -and (P1273-GateBit 49) -eq 2048 -and (P1273-GateBit 65) -eq 4096) 'floor masks'
Refuse {P12754-PriorFloor 'LITTLE598'} 'prior same-boot'
$status='version=1 state=3 request=14 arm=1000 down=0 start=25000 end=85000 until=85000 up=0 now=100000 hz=1000 reason=10 mask=0 count=60 drops=0 bus=0 stop_big=62000 stop_lit=60500 stop_gpu=55000 stop_bat=37 stop_age=10 policy=0'
$s=P125-Fields $status
$cp=[pscustomobject]@{Nonce='bootA';Request=14;Arm='1000';Module='HARDWARE54'}
$script:mockStatus=$status;$script:mockNonce='bootA';$script:cursor='0';$script:writes=0
function P12-Query([string]$Name,[string]$Value=''){
    switch($Name){
        'p1273_boot_identity'{return $script:mockNonce}
        'p12753_status'{return $script:mockStatus}
        'p12753_select'{if($Value -ne ''){$script:writes++;$script:cursor=$Value;throw 'Missing echo and sysctl readback'};return $script:cursor}
        'p12753_page'{return "version=1 select=$script:cursor phase=25000"}
        default{throw "Unexpected query $Name"}
    }
}
$r=P12754-FrozenRow $s $cp 7
Check ($r.select -eq '7' -and $script:writes -eq 1) 'readback after lost echo'
$script:mockNonce='bootB';Refuse {P12754-FrozenRow $s $cp 8} 'boot identity changed'
$script:mockNonce='bootA';$script:mockStatus=$status.Replace('arm=1000','arm=2000')
Refuse {P12754-FrozenRow $s $cp 9} 'frozen phase changed'
Write-Host 'P12754 typed diagnostics, floor prerequisites and frozen selector recovery PASS (mock only)'
