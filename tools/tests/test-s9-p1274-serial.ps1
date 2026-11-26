#requires -Version 5.1
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../s9-p1274-serial.ps1') -LibraryOnly
function Check($ok,[string]$why){if(-not $ok){throw $why}}
Check $script:P1274HostEnabled 'P1274 host opt-in'
$st='version=1 state=3 profile=4 start=100000 end=420000 planned=420000 reason=1 rows=1 fg=17 events=1 drops=0 rate_events=0 rate_drops=0 bigmask=78 litmask=87 trigger_bits=1 burst_start=330000 burst_end=334000 measure_ok=10 measure_bad=0 measure_skip=0 usb_down=120000 usb_up=0'
Check (P12-ReplyShape 'p1274_status' $st $true) 'frozen status'
Check (-not (P12-ReplyShape 'p1274_status' ($st+' junk') $true)) 'bad status accepted'
Check (P12-ReplyShape 'p1274_status' 'version=1 state=2 profile=4 now=120000' $true) 'active status'
Check (P12-ReplyShape 'p1274_metal_log_mode' '-1' $true) 'signed mode'
Check (P12-ReplyShape 'p1274_restore' 'version=1 state=3 profile=0 cores=4 actual=ff requested=ff fast=0 online=68 exit=0 display=1' $true) 'restore'
foreach($bad in @(@('p1274_profile','4'),@('p1274_profile','10'),@('p1274_select','5008'),@('p1274_metal_log_mode','2'),@('p1274_user_event','1'))){
 $refused=$false;try{[void](P12-Query $bad[0] $bad[1])}catch{if($_.Exception.Message -match 'only permits|disallowed'){$refused=$true}else{throw}}
 Check $refused "unsafe write $bad"
}
$opts.Profile='B2';$opts.AllowIntervention=$true;$opts.Power='Connected'
$refused=$false;try{Check-Prior}catch{if($_.Exception.Message -match 'Prior reviewed'){$refused=$true}else{throw}}
Check $refused 'B2 no prior accepted'
$opts.Profile='BASE';$opts.Power='Offline';$opts.AllowOffline=$false
$refused=$false;try{Check-Prior}catch{if($_.Exception.Message -match 'AllowOffline'){$refused=$true}else{throw}}
Check $refused 'offline no opt-in accepted'
$dir=Join-Path $env:TEMP ('p1274-test-'+[guid]::NewGuid())
[void](New-Item -ItemType Directory -Path $dir)
try{
 Save-Json ([pscustomobject]@{version=1;nonce='1234';hz='1000';anchor='abcd';profile='B1';power='Offline'}) (Join-Path $dir 'capture.json')
 Set-Content -LiteralPath (Join-Path $dir 'status.txt') -Value $st
 Set-Content -LiteralPath (Join-Path $dir 'dump.complete') -Value '1234'
 Save-Json ([pscustomobject]@{ok=$true}) (Join-Path $dir 'restore.json')
 Save-Json ([pscustomobject]@{ok=$true}) (Join-Path $dir 'wake.json')
 $lines=@('version=1 select=0 kind=1 at=330000 tmu_at=330000 measured_at=329000 direct=69000 peak=70000 lit=60000 gpu=59000 slope=1000 bp=99 lp=20 ba=80 la=10 bs=250 ls=20 bsp=350 lsp=30 bkhz=1469000 lkhz=1053000 bcap=1469000 fast=0 measured=1469 online=68 recommended=ff requested=ff display=0 profile=4')
 for($i=0;$i -lt 17;$i++){
  $sel=512+$i;$seq=$i+1;$at=250000+$i*10000;$qh=(10-$i*2+65536)%65536
  $lines+="version=1 select=$sel kind=2 seq=$seq at=$at ua=-500000 avg=-500000 uv=4000000 external=0 charger=0 qh=$qh"
 }
 $lines+='version=1 select=1024 kind=3 at=330000 pid=136 task=100 tid=200 event=2 a=64 b=0 c=64 d=1000 e=70 f=0 g=0'
 Set-Content -LiteralPath (Join-Path $dir 'pages.txt') -Value $lines
 $r=Analyze-Run $dir
 Check $r.qualified 'valid synthetic phase not qualified'
 Check ($r.qh_raw_bracket_delta -eq -32) 'QH wrap delta'
 Check ([Math]::Abs($r.net_battery_wh_250_410-2*160/3600) -lt 0.00001) 'energy integral'
 Check ($r.burst_proxy_s -eq 4 -and $r.layout_calls_lower_bound -eq 64) 'burst/count'
 $lines[2]=$lines[2] -replace 'external=0','external=1'
 Set-Content -LiteralPath (Join-Path $dir 'pages.txt') -Value $lines
 $r=Analyze-Run $dir
 Check (-not $r.power_verified -and -not $r.qualified -and $null -eq $r.net_battery_wh_250_410) 'external power invalid'
 $opts.Profile='B2';$opts.AllowIntervention=$true;$opts.Power='Offline';$opts.AllowOffline=$true;$opts.PriorReport=Join-Path $dir 'summary.json'
 $refused=$false;try{Check-Prior}catch{if($_.Exception.Message -match 'not qualified'){$refused=$true}else{throw}}
 Check $refused 'unqualified report accepted'
}finally{Remove-Item -LiteralPath $dir -Recurse -Force}
Write-Host 'P1274 typed ABI, refusal, QH wrap, energy, qualification mocks PASS; no serial'
