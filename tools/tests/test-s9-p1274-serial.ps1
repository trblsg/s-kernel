#requires -Version 5.1
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../s9-p1274-serial.ps1') -LibraryOnly
function Check($ok,[string]$why){if(-not $ok){throw $why}}
Check $script:P1274HostEnabled 'P1274 host opt-in'
$st='version=1 state=3 profile=4 start=100000 end=720000 planned=720000 reason=1 rows=1 fg=47 events=1 drops=0 rate_events=0 rate_drops=0 bigmask=78 litmask=87 trigger_bits=1 burst_start=330000 burst_end=334000 measure_ok=10 measure_bad=0 measure_skip=0 usb_down=120000 usb_up=0'
Check (P12-ReplyShape 'p1274_status' $st $true) 'frozen status'
Check (-not (P12-ReplyShape 'p1274_status' ($st+' junk') $true)) 'bad status accepted'
Check (P12-ReplyShape 'p1274_status' 'version=1 state=2 profile=4 now=120000' $true) 'active status'
Check (P12-ReplyShape 'p1274_metal_log_mode' '-1' $true) 'signed mode'
Check (P12-ReplyShape 'p12741_revision' '2' $true) 'revision'
Check (P12-ReplyShape 'p1275_revision' '1' $true) 'P1275 revision'
Check (P12-ReplyShape 'p12751_boot_latch' '7' $true) 'P12751 boot latch'
Check (-not (P12-ReplyShape 'p12751_boot_latch' '8' $true)) 'P12751 invalid boot latch'
Check (P12-ReplyShape 'p12751_stage_info' 'version=1 sealed=1 count=2 done=1 drops=0' $true) 'P12751 stage info'
Check (P12-ReplyShape 'p12751_stage_page' 'version=1 select=0 at=20000 end=721000 pid=134 task=123 tid=456 stage=11 cpu=12345 user=123 sys=10 minor=0 major=0 images0=17 images1=19 ok=1 usage=1' $true) 'P12751 stage page'
Check (P12-ReplyShape 'p1275_selected' '16' $true) 'P1275 selected'
Check (-not (P12-ReplyShape 'p1275_selected' '17' $true)) 'P1275 invalid selected'
Check (P12-ReplyShape 'p1275_focus' 'version=1 s=8192 s=12287' $true) 'P1275 focus'
Check (-not (P12-ReplyShape 'p1275_focus' 'version=1 s=8192 junk' $true)) 'P1275 malformed focus'
Check (-not (P12-ReplyShape 'p12741_revision' '1' $true)) 'old revision accepted'
Check (P12-ReplyShape 'p12741_guard' 'version=2 armed=1 at=720001 big=64000 lit=63000 gpu=62000 age=1 battery=41 fast=0 lit_fast=0 peak_big=70000 peak_lit=64000 changes=2 last_change=500000 bcur=650000 lcur=1053000' $true) 'live guard shape'
Check (P12-ReplyShape 'p1274_restore' 'version=1 state=3 profile=0 cores=4 actual=ff requested=ff fast=0 online=68 exit=0 display=1' $true) 'restore'
foreach($bad in @(@('p1274_profile','4'),@('p12741_profile','4'),@('p12741_profile','10'),@('p1274_select','14280'),@('p12741_guard','1'),@('p1274_metal_log_mode','2'),@('p1274_user_event','1'),@('p1275_profile','4'),@('p1275_profile','17'),@('p1275_selected','4'),@('p1275_mode','4'),@('p12751_boot_latch','3'),@('p12751_next','3'),@('p12751_ack','3'),@('p12751_stage_select','512'))){
 $refused=$false;try{[void](P12-Query $bad[0] $bad[1])}catch{if($_.Exception.Message -match 'only permits|disallowed'){$refused=$true}else{throw}}
 Check $refused "unsafe write $bad"
}
$opts.Profile='LOG2';$opts.Experiment='EarlyLoad';$opts.Power='Connected';$opts.AllowIntervention=$false
$refused=$false;try{Check-Prior}catch{if($_.Exception.Message -match 'requires -AllowIntervention'){$refused=$true}else{throw}}
Check $refused 'P1275 missing opt-in accepted'
$opts.AllowIntervention=$true;Check-Prior
$opts.Power='Offline';$refused=$false;try{Check-Prior}catch{if($_.Exception.Message -match 'LOG2 Connected'){$refused=$true}else{throw}}
Check $refused 'P1275 offline accepted'
$opts.Experiment='Legacy'
$opts.Profile='B2';$opts.AllowIntervention=$true;$opts.Power='Connected'
$refused=$false;try{Check-Prior}catch{if($_.Exception.Message -match 'Prior reviewed'){$refused=$true}else{throw}}
Check $refused 'B2 no prior accepted'
$opts.Profile='BASE';$opts.Power='Offline';$opts.AllowOffline=$false
$refused=$false;try{Check-Prior}catch{if($_.Exception.Message -match 'AllowOffline'){$refused=$true}else{throw}}
Check $refused 'offline no opt-in accepted'
$dir=Join-Path $env:TEMP ('p1274-test-'+[guid]::NewGuid())
[void](New-Item -ItemType Directory -Path $dir)
try{
 Save-Json ([pscustomobject]@{version=2;nonce='1234';hz='1000';anchor='abcd';profile='B1';power='Offline'}) (Join-Path $dir 'capture.json')
 Set-Content -LiteralPath (Join-Path $dir 'status.txt') -Value $st
 Set-Content -LiteralPath (Join-Path $dir 'episode-info.txt') -Value 'version=2 episodes=1 episode_drops=0 peak=70000 peak_select=6000'
 Set-Content -LiteralPath (Join-Path $dir 'dump.complete') -Value '1234'
 Save-Json ([pscustomobject]@{ok=$true;guard=@{peak_big=70000;peak_lit=64000}}) (Join-Path $dir 'restore.json')
 Save-Json ([pscustomobject]@{ok=$true}) (Join-Path $dir 'wake.json')
 $lines=@('version=1 select=6000 kind=1 at=330000 tmu_at=330000 measured_at=329000 direct=69000 peak=70000 lit=60000 gpu=59000 slope=1000 bp=99 lp=20 ba=80 la=10 bs=250 ls=20 bsp=350 lsp=30 bkhz=1469000 lkhz=1053000 bcap=1469000 fast=0 measured=1469 online=68 recommended=ff requested=ff display=0 profile=4 lcap=1794000 lfast=0')
 for($i=0;$i -lt 47;$i++){
  $sel=512+$i;$seq=$i+1;$at=250000+$i*10000;$qh=(10-$i*2+65536)%65536
  $lines+="version=1 select=$sel kind=2 seq=$seq at=$at ua=-500000 avg=-500000 uv=4000000 external=0 charger=0 qh=$qh"
 }
 $lines+='version=1 select=8192 kind=3 at=330000 pid=136 task=100 tid=200 event=2 a=64 b=0 c=64 d=1000 e=70 f=0 g=0'
 $lines+='version=2 select=5100 kind=7 at=330000 end=334000 bits=3 severity=2 peak=70000 lit_peak=60000 bp=99 lp=20 bsp=350 lsp=30 samples=1'
 $lines+='version=2 select=13000 kind=8 episode=0 index=0 at=330000 direct=69000 lit=60000 bp=99 lp=20 bs=250 ls=20 bkhz=1469000 lkhz=1053000 fast=1469000 lfast=0'
 Set-Content -LiteralPath (Join-Path $dir 'pages.txt') -Value $lines
 $r=Analyze-Run $dir
 Check $r.qualified 'valid synthetic phase not qualified'
 Check ($r.qh_raw_bracket_delta -eq -92) 'QH wrap delta'
 Check ([Math]::Abs($r.net_battery_wh_250_710-2*460/3600) -lt 0.00001) 'energy integral'
 Check ($r.burst_proxy_s -eq 4 -and $r.layout_calls_lower_bound -eq 64 -and $r.episodes -eq 1) 'burst/count'
 $partial=@($lines | Where-Object {$_ -notmatch 'kind=1 |kind=8 '})
 Set-Content -LiteralPath (Join-Path $dir 'pages.txt') -Value $partial
 $r=Analyze-Run $dir
 Check ($r.peak_big_c -eq 70 -and -not $r.coverage -and -not $r.qualified) 'partial immutable peak/qualification'
 $lines[2]=$lines[2] -replace 'external=0','external=1'
 Set-Content -LiteralPath (Join-Path $dir 'pages.txt') -Value $lines
 $r=Analyze-Run $dir
 Check (-not $r.power_verified -and -not $r.qualified -and $null -eq $r.net_battery_wh_250_710) 'external power invalid'
 $opts.Profile='B2';$opts.AllowIntervention=$true;$opts.Power='Offline';$opts.AllowOffline=$true;$opts.PriorReport=Join-Path $dir 'summary.json'
 $refused=$false;try{Check-Prior}catch{if($_.Exception.Message -match 'not qualified'){$refused=$true}else{throw}}
 Check $refused 'unqualified report accepted'
 # Exercise real priority Dump + checkpoint logic with an in-memory serial
 # reply model. A 16-page batch must retain the primary evidence before
 # walking the 128 detail rows. Second batch resumes exactly.
 $opts.RunDir=Join-Path $dir 'mock-dump';[void](New-Item -ItemType Directory -Path $opts.RunDir)
 $script:mockPages=@{}
 foreach($line in $lines){
  $v=P125-Fields $line
  $script:mockPages[[int]$v.select]=$line
 }
 for($i=0;$i -lt 128;$i++){
  $sel=4096+$i
  $script:mockPages[$sel]="version=1 select=$sel kind=4 present=0 bin=0 at=0 tmu_at=0 bp=0 lp=0 ba=0 la=0 bs=0 ls=0 bsp=0 lsp=0 bkhz=0 lkhz=0 direct=0 peak=0"
 }
 for($i=0;$i -lt 8;$i++){
  $sel=5000+$i;$script:mockPages[$sel]="version=1 select=$sel kind=5 cpu=$i n=0 ticks=0"
 }
 $script:mockOrder=[System.Collections.Generic.List[string]]::new()
 $script:mockSelect=0;$script:mockTraceSelect=0;$script:mockStageSelect=0;$script:mockFair=$false
 $script:mockStatus=P125-Fields $st
 function Read-Frozen($meta){
  Stable-Line (Join-Path $opts.RunDir 'status.txt') $st
  Stable-Line (Join-Path $opts.RunDir 'episode-info.txt') 'version=2 episodes=1 episode_drops=0 peak=70000 peak_select=6000'
  Stable-Line (Join-Path $opts.RunDir 'trace-status.txt') 'version=1 state=4 generation=1 start=329750 end=330500 until=330500 trigger=330000 span=250 reason=7 buckets=128 owners=16'
  return $script:mockStatus
 }
 function P12-EarlyThermalGuard{}
 function Restore-Sample{return $true}
 function P12-Query([string]$Name,[string]$Value='',[switch]$Cleanup){
  switch -Regex ($Name){
   '^p12741_guard$' {return 'version=2 armed=1 at=720001 big=64000 lit=63000 gpu=62000 age=1 battery=41 fast=0 lit_fast=0 peak_big=70000 peak_lit=64000 changes=2 last_change=500000 bcur=650000 lcur=1053000'}
   '^p1275_focus$' {return 'version=1 s=8192'}
   '^p12751_stage_info$' {return 'version=1 sealed=1 count=2 done=1 drops=0'}
   '^p12751_stage_focus$' {return 'version=1 s=0 s=1'}
   '^p12751_stage_select$' {$script:mockStageSelect=[int]$Value;return $Value}
   '^p12751_stage_page$' {$script:mockOrder.Add("S$script:mockStageSelect");if($script:mockStageSelect -eq 0){return 'version=1 select=0 at=20000 end=721000 pid=134 task=123 tid=456 stage=11 cpu=12345 user=123 sys=10 minor=0 major=0 images0=17 images1=19 ok=1 usage=1'}else{return 'version=1 select=1 at=330000 end=0 pid=135 task=124 tid=457 stage=7 cpu=18446744073709551615 user=0 sys=0 minor=0 major=0 images0=22 images1=0 ok=2 usage=0'}}
   '^p1274_select$' {$script:mockSelect=[int]$Value;return $Value}
   '^p1274_page$' {$script:mockOrder.Add("P$script:mockSelect");return $script:mockPages[$script:mockSelect]}
   '^p1273_gpu_status$' {return 'version=1 generation=1 submitted=0 completed=0 overwritten=0 first_wake=0 slots=1024'}
   '^p1273_trace_cpu([0-7])$' {$cpu=[int]$Matches[1];$mask=if($cpu -eq 3 -or ($script:mockFair -and $cpu -eq 0)){'2/0'}else{'0/0'};return "version=1 cpu=$cpu generation=1 mask=$mask overhead=0 clipped=0"}
   '^p1273_trace_select$' {$script:mockTraceSelect=[int]$Value;return $Value}
   '^p1273_trace_page$' {$script:mockOrder.Add("T$script:mockTraceSelect");return "version=1 select=$script:mockTraceSelect generation=1 present=1 bin=1 mask=1 dropped=0 dropped_ticks=0 r=1/2/136/2/100/0/1/1"}
   default {throw "unexpected mock query $Name"}
  }
 }
 $meta=[pscustomobject]@{version=2;nonce='1234';hz='1000';profile='B1'}
 $opts.MaxPages=16;$script:budget=16
 $batch=$false;try{Dump $meta}catch{if($_.Exception.Message -eq 'P1274_BATCH_LIMIT'){$batch=$true}else{throw}}
 Check $batch 'bounded batch must stop'
 Check ($script:mockOrder[0] -eq 'P5100' -and $script:mockOrder[1] -eq 'P6000' -and $script:mockOrder[2] -eq 'P8192' -and $script:mockOrder[3] -eq 'P13000' -and $script:mockOrder[4] -eq 'T3080') 'priority episode/peak/user/owner order'
 Check (-not (Test-Path (Join-Path $opts.RunDir 'dump.complete'))) 'partial marked complete'
 $opts.MaxPages=512;$script:budget=512
 Dump $meta
 Check (Test-Path (Join-Path $opts.RunDir 'dump.complete')) 'resume failed'
 Check (@($script:mockOrder|Where-Object {$_ -eq 'P5100'}).Count -eq 1) 'resume reread checkpointed header'
 $opts.RunDir=Join-Path $dir 'mock-p1275';[void](New-Item -ItemType Directory -Path $opts.RunDir)
 $script:mockOrder.Clear();$meta|Add-Member -NotePropertyName p1275_mode -NotePropertyValue 4
 $opts.MaxPages=16;$script:budget=16
 $batch=$false;try{Dump $meta}catch{if($_.Exception.Message -eq 'P1274_BATCH_LIMIT'){$batch=$true}else{throw}}
 Check ($batch -and (Test-Path (Join-Path $opts.RunDir 'p1275-focus.txt'))) 'P1275 focus checkpoint'
 Check ($script:mockOrder[2] -eq 'P8192') 'P1275 focus priority'
 Check (-not (Test-Path (Join-Path $opts.RunDir 'dump.complete'))) 'P1275 partial marked complete'
 $opts.RunDir=Join-Path $dir 'mock-p12751';[void](New-Item -ItemType Directory -Path $opts.RunDir)
 $script:mockOrder.Clear();$script:mockFair=$true;$meta|Add-Member -NotePropertyName p12751_latch -NotePropertyValue 7
 $script:budget=16;$opts.MaxPages=16
 $batch=$false;try{Dump $meta}catch{if($_.Exception.Message -eq 'P1274_BATCH_LIMIT'){$batch=$true}else{throw}}
 Check ($batch -and $script:mockOrder[0] -eq 'S0' -and $script:mockOrder[1] -eq 'S1') 'paired boot/tail and censored stage priority'
 Check ($script:mockOrder.Contains('T3080') -and $script:mockOrder.Contains('T8')) 'both cluster pages must be included in first batch'
 Check (-not (Test-Path (Join-Path $opts.RunDir 'dump.complete'))) 'P12751 partial marked complete'
 $opts.MaxPages=512;$script:budget=512;Dump $meta
 Check (Test-Path (Join-Path $opts.RunDir 'dump.complete')) 'P12751 resume failed'
 Check (@($script:mockOrder|Where-Object {$_ -eq 'S0'}).Count -eq 1) 'P12751 reread sealed stage'
}finally{Remove-Item -LiteralPath $dir -Recurse -Force}
Write-Host 'P1274.1 typed ABI, refusal, episode coverage, partial peak, QH/energy, priority dump/resume and qualification mocks PASS; no serial'
