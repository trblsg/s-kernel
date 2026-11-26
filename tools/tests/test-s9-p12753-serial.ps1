#requires -Version 5.1
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../s9-p125-timer-serial.ps1') -LibraryOnly
function Check($ok,[string]$why){if(-not $ok){throw $why}}
function Refuse([scriptblock]$action,[string]$pattern){$refused=$false;try{& $action}catch{if($_.Exception.Message -match $pattern){$refused=$true}else{throw}};Check $refused "missing refusal $pattern"}
Check (P12-ReplyShape 'p12753_revision' '1' $true) 'revision'
$status='version=1 state=3 request=10 arm=1000 down=2000 start=25000 end=85000 until=85000 up=90000 now=100000 hz=1000 reason=10 mask=0 count=30 drops=0 bus=0 stop_big=62000 stop_lit=60500 stop_gpu=55000 stop_bat=37 stop_age=10 policy=0'
Check (P12-ReplyShape 'p12753_status' $status $true) 'status'
Check (-not (P12-ReplyShape 'p12753_status' ($status+' junk') $true)) 'malformed status'
$row='version=1 select=0 phase=25000 at=25000 kind=1 index=0 raw=0 valid=1 errno=0 latency=0 big=62000 lit=60500 gpu=55000 bat=37 age=10 bs=4 ls=10 bkhz=650000 lkhz=1053000 bmeas=650 lmeas=1053 policy=0 driver=0 refresh=300 g3d=0 dpu=15 isppre=0 mfc=0 aud=15 mask=0 opp_uv=0 opp_valid=0'
Check (P12-ReplyShape 'p12753_page' $row $true) 'page'
Refuse {P12-Query 'p12753_arm' '1'} 'disallowed|only permits'
$script:P12753HostEnabled=$true;$script:P12753ArmAllowed=$false
Refuse {P12-Query 'p12753_arm' '1'} 'only permits'
Refuse {P12-Query 'p12753_select' '96'} 'only permits'
$script:P12753ArmAllowed=$true
Refuse {P12-Query 'p12753_arm' '11'} 'only permits'
$s=P125-Fields $status
$rows=@(0..29|ForEach-Object {$r=P125-Fields $row;$r.at=[string](25000+2000*$_);[pscustomobject]$r})
$fg=@(0..5|ForEach-Object {[pscustomobject]@{seq=1+$_;ticks=27000+10000*$_;external=0;current_ua=-500000;voltage_uv=4000000}})
$p=P12753-Power $s $rows $fg 60
Check ($p.power_verified -and [Math]::Abs($p.mean_discharge_w-2) -lt 0.0001 -and $p.covered_s -eq 50) 'valid trapezoid'
$s.reason='2';Check (-not (P12753-Power $s $rows $fg 60).power_verified) 'thermal cutoff'
$s.reason='10';$s.up='30000';Check (-not (P12753-Power $s $rows $fg 60).power_verified) 'early reconnect'
$s.up='90000';$s.down='20000';Check (-not (P12753-Power $s $rows $fg 60).power_verified) 'unsettled link'
$s.down='2000';$fg[1].external=1;Check (-not (P12753-Power $s $rows $fg 60).power_verified) 'charging'
$fg[1].external=0;$fg[2].seq=10;Check (-not (P12753-Power $s $rows $fg 60).power_verified) 'sequence gap'
$fg[2].seq=3;$rows[4].driver='15';Check (-not (P12753-Power $s $rows $fg 60).power_verified) 'false stock'
$rows[4].driver='0';$rows[4].g3d='4294967295';Check (-not (P12753-Power $s $rows $fg 60).power_verified) 'unknown GPU'
$rows[4].g3d='0';Check (-not (P12753-Power $s $rows @($fg[0],$fg[1]) 60).power_verified) 'insufficient FG'
function Report($module,$start,$w){[pscustomobject]@{version=1;power_verified=$true;applied=$true;restored=$true;diagnostic=$false;module=$module;start=$start;end=$start+60000;boot='same';mean_discharge_w=$w;window_s=60;start_big_mc=62000;start_lit_mc=60500;mean_voltage_uv=4000000}}
$a=Report 'STOCK60' 100000 2.2;$b=Report 'BASE15' 200000 1.9;$c=Report 'STOCK60' 300000 2.1
$r=P12753-Compare $a $b $c;Check ([Math]::Abs($r.delta_mW-250) -lt 0.01) 'ABA delta'
$c.boot='other';Refuse {P12753-Compare $a $b $c} 'boot'
$c.boot='same';$c.mean_discharge_w=3;Refuse {P12753-Compare $a $b $c} 'drift'
Write-Host 'P12753 parser, stock evidence, FG integration, cutoff, reconnect and A/B/A validation PASS'
