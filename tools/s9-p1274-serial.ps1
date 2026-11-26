#requires -Version 5.1
[CmdletBinding()]
param(
 [ValidateSet('Capture','Dump','Analyze','WakeCheck','Compare')][string]$Action='Capture',
 [ValidateSet('BASE','LOG2','FAST','B1','B2','B3','R2','R1','R0')][string]$Profile='BASE',
 [ValidateSet('Connected','Offline')][string]$Power='Connected',
 [ValidateSet('Legacy','Observe','Early','Load','EarlyLoad','Coord','Lazy','Combined','CombinedLazy')][string]$Experiment='Legacy',
 [ValidatePattern('^COM[0-9]+$')][string]$Port='COM125',
 [string]$ReconnectPort='', [string]$RunDir='', [string]$PriorReport='',
 [string[]]$CompareDirs=@(), [ValidateRange(16,512)][int]$MaxPages=96,
 [switch]$AllowIntervention,[switch]$AllowOffline,[switch]$ConfirmForeground,[switch]$Execute,[switch]$LibraryOnly
)
Set-StrictMode -Version 2
$ErrorActionPreference='Stop'
$opts=@{Action=$Action;Profile=$Profile;Power=$Power;Experiment=$Experiment;Port=$Port;ReconnectPort=$ReconnectPort;RunDir=$RunDir;PriorReport=$PriorReport;CompareDirs=$CompareDirs;MaxPages=$MaxPages;AllowIntervention=[bool]$AllowIntervention;AllowOffline=[bool]$AllowOffline;ConfirmForeground=[bool]$ConfirmForeground;Execute=[bool]$Execute;LibraryOnly=[bool]$LibraryOnly}
. (Join-Path $PSScriptRoot 's9-p125-timer-serial.ps1') -LibraryOnly -Port $Port -ReconnectPort $ReconnectPort
$script:P1273HostEnabled=$true;$script:P1274HostEnabled=$true
$script:P1274ProfileAllowed=$false
$ids=@{BASE=1;LOG2=2;FAST=3;B1=4;B2=5;B3=6;R2=7;R1=8;R0=9}
$experimentIds=@{Legacy=0;Observe=1;Early=2;Load=3;EarlyLoad=4;Coord=5;Lazy=9;Combined=8;CombinedLazy=16}
$script:budget=$opts.MaxPages
$script:dirty=$false
$script:P12741Deadline=720
function Save-Json($obj,[string]$file){$obj|ConvertTo-Json -Depth 10|Set-Content -Encoding UTF8 -LiteralPath $file}
function Load-Json([string]$file){Get-Content -Raw -LiteralPath $file|ConvertFrom-Json}
function Range-N([int]$start,[int]$count){for($i=0;$i -lt $count;$i++){$start+$i}}
function Stable-Line([string]$file,[string]$line){
 if(Test-Path -LiteralPath $file){if((Get-Content -Raw -LiteralPath $file).Trim() -cne $line){throw "Frozen identity changed: $file"}}
 else{Set-Content -Encoding UTF8 -LiteralPath $file -Value $line}
}
function Page-Cache([string]$file,[string]$name,[string]$key){
 $h=@{}
 if(Test-Path -LiteralPath $file){
  foreach($line in Get-Content -LiteralPath $file){
   if(-not (P12-ReplyShape $name $line $false)){throw "Invalid checkpoint: $file"}
   $f=P125-Fields $line;$n=[int]$f[$key]
   if($h.ContainsKey($n)){throw "Duplicate checkpoint page $n"}
   $h[$n]=$line
  }
 }
 return $h
}
function Get-Page($cache,[string]$file,[string]$selector,[string]$reader,[string]$key,[int]$n){
 if($cache.ContainsKey($n)){return $cache[$n]}
 if($script:budget -le 0){throw 'P1274_BATCH_LIMIT'}
 P12-EarlyThermalGuard
 if((($opts.MaxPages-$script:budget)%16) -eq 0){[void](P12741-LiveGuard)}
 [void](P12-Query $selector "$n")
 $line=P12-Query $reader;$f=P125-Fields $line
 if([int]$f[$key] -ne $n){throw "Page identity mismatch $reader/$n"}
 # Validate before persisting; caller additionally checks generation/time/coverage.
 $script:budget--
 return $line
}
function Commit-Page($cache,[string]$file,[int]$n,[string]$line){
 if(-not $cache.ContainsKey($n)){Add-Content -Encoding UTF8 -LiteralPath $file -Value $line;$cache[$n]=$line}
}
function Check-Identity($meta){
 $nonce=P125-Fields (P12-Query 'p1273_boot_identity');$s=P125-Status
 if($nonce.nonce -cne $meta.nonce -or $s.hz -ne $meta.hz -or $s.anchor -cne $meta.anchor){throw 'Boot nonce / clock / anchor mismatch: never resume after reboot'}
 return $s
}
function P12741-LiveGuard{
 $g=P125-Fields (P12-Query 'p12741_guard')
 if($g.armed -ne '1' -or [int]$g.age -gt 2000 -or [int]$g.big -lt 0 -or [int]$g.lit -lt 0 -or [int]$g.gpu -lt 0 -or [int]$g.battery -le 0){throw 'P1274.1 live thermal proof missing; stop and cool'}
 if([int]$g.big -ge 76000 -or [int]$g.lit -ge 70000 -or [int]$g.gpu -ge 72000 -or [int]$g.battery -ge 47){throw "P1274.1 LIVE THERMAL STOP BIG=$($g.big) LITTLE=$($g.lit) GPU=$($g.gpu) battery=$($g.battery)"}
 return $g
}
function P12741-RequireRevision{
 if((P12-Query 'p12741_revision') -ne '2'){throw 'P1274.1 host/firmware mismatch'}
}
function Read-Frozen($meta){
 if($meta.version -ne 2){throw 'P1274.1 requires revision 2 capture; use original kit for older logs'}
 P12741-RequireRevision
 [void](Check-Identity $meta)
 if($meta.PSObject.Properties['p1275_mode'] -and [int]$meta.p1275_mode){
  if((P12-Query 'p1275_revision') -ne '1' -or [int](P12-Query 'p1275_selected') -ne [int]$meta.p1275_mode){throw 'P1275 firmware/mode mismatch'}
 }
 $line=P12-Query 'p1274_status';$s=P125-Fields $line
 if($s.state -ne '3' -or [int]$s.profile -ne $ids[$meta.profile]){throw 'P1274 not frozen or profile mismatch'}
 if([uint64]$s.start -ge [uint64]$s.end -or [uint64]$s.end -gt ([uint64]$s.planned+[uint64]$meta.hz*2) -or
    [uint64]$s.planned -ne ([uint64]$meta.hz*720) -or [int]$s.rows -gt 768 -or [int]$s.fg -gt 128 -or [int]$s.events -gt 4096 -or [int]$s.rate_events -gt 1024){throw 'Frozen coverage invalid'}
 Stable-Line (Join-Path $opts.RunDir 'status.txt') $line
 $info=P12-Query 'p12741_info';$v=P125-Fields $info
 if([int]$v.episodes -gt 32 -or ([int]$v.peak_select -ne 0 -and ([int]$v.peak_select -lt 6000 -or [int]$v.peak_select -ge (6000+[int]$s.rows)))){throw 'Episode/peak index invalid'}
 Stable-Line (Join-Path $opts.RunDir 'episode-info.txt') $info
 $trace=P12-Query 'p1273_trace_status'
 Stable-Line (Join-Path $opts.RunDir 'trace-status.txt') $trace
 return $s
}
function Restore-Sample {
 $r=P125-Fields (P12-Query 'p1274_restore')
 $guard=P12741-LiveGuard
 $ok=($r.state -eq '3' -and $r.profile -eq '0' -and $r.cores -eq '4' -and (P125-Hex $r.actual) -eq 255 -and (P125-Hex $r.requested) -eq 255 -and $r.fast -eq '0' -and $guard.lit_fast -eq '0' -and [int]$r.online -eq 68 -and (P125-Hex $r.exit) -eq 0)
 $loadState=$null
 $capturePath=Join-Path $opts.RunDir 'capture.json'
 if(Test-Path -LiteralPath $capturePath){
  $captureMeta=Load-Json $capturePath
  if($captureMeta.PSObject.Properties['p1275_mode'] -and [int]$captureMeta.p1275_mode){
   $loadState=[int](P12-Query 'p1275_load_state')
   $ok=$ok -and $loadState -eq 0
  }
 }
 Save-Json ([pscustomobject]@{ok=$ok;fields=$r;guard=$guard;load_state=$loadState;utc=[DateTime]::UtcNow.ToString('o')}) (Join-Path $opts.RunDir 'restore.json')
 return $ok
}
function Check-Prior{
 $p=$opts.Profile
 if($opts.Experiment -ne 'Legacy'){
  if($p -ne 'LOG2' -or $opts.Power -ne 'Connected'){throw 'P1275 experiments require LOG2 Connected'}
  if($opts.Experiment -ne 'Observe' -and -not $opts.AllowIntervention){throw 'P1275 experimental mode requires -AllowIntervention'}
 }
 if($ids[$p] -ge 3 -and -not $opts.AllowIntervention){throw 'Explicit -AllowIntervention required'}
 if($opts.Power -eq 'Offline' -and -not $opts.AllowOffline){throw 'Explicit -AllowOffline required'}
 if($p -in @('B2','B3','R2','R1','R0')){
  if(-not $opts.PriorReport){throw 'Prior reviewed summary.json required'}
  $prior=Load-Json $opts.PriorReport
  $allowed=switch($p){B2{'B1'} B3{'B2'} R2{'B1';'B2';'B3'} R1{'R2'} R0{'R1'}}
  if($prior.version -ne 2 -or $prior.profile -notin @($allowed) -or -not $prior.qualified){throw 'Previous step not qualified; review functional/thermal evidence first'}
 }
}
function Capture{
 Check-Prior
 P12741-RequireRevision
 if($opts.Experiment -ne 'Legacy' -and (P12-Query 'p1275_revision') -ne '1'){throw 'P1275 revision mismatch'}
 $s=P125-Status;$n=P125-Fields (P12-Query 'p1273_boot_identity')
 if($s.armed -ne '0' -or $s.active -ne '0'){throw 'P125 lease active'}
 $state=P125-Fields (P12-Query 'p1274_status')
 if($state.state -ne '0'){throw 'Fresh boot required; preserve previous frozen capture'}
 if($script:LifeState -ne 'OFF'){
  Life-Say 'Press POWER once to turn screen OFF. Do not swipe.'
  $until=(Life-Now).AddSeconds(25)
  while($script:LifeState -ne 'OFF' -and (Life-Now) -lt $until){Life-Pump;Life-CheckSafety (Life-Now);P12-EarlyThermalGuard}
 }
 P125-PumpOff 25
 $g=P125-Fields (P12-Query 'g3d_lab_stats')
 if($g.domain -ne '0' -or $g.pll -ne '0' -or $g.fault -ne '0'){throw 'GPU OFF proof missing'}
 $s=P125-Status
 if([double]$s.now/[double]$s.hz -ge 220){throw 'Arm too late; fresh boot required (arm before 225s)'}
 $meta=[pscustomobject]@{version=2;nonce=$n.nonce;hz=$s.hz;anchor=$s.anchor;profile=$opts.Profile;power=$opts.Power;experiment=$opts.Experiment;p1275_mode=$experimentIds[$opts.Experiment];utc=[DateTime]::UtcNow.ToString('o');arm_now=$s.now}
 Save-Json $meta (Join-Path $opts.RunDir 'capture.json')
 $script:dirty=$true;$script:P1274ProfileAllowed=$true
 try{
  if($opts.Experiment -eq 'Legacy'){[void](P12-Query 'p12741_profile' "$($ids[$opts.Profile])")}
  else{[void](P12-Query 'p1275_profile' "$($experimentIds[$opts.Experiment])")}
 }finally{$script:P1274ProfileAllowed=$false}
 if($opts.Experiment -ne 'Legacy' -and [int](P12-Query 'p1275_selected') -ne $experimentIds[$opts.Experiment]){throw 'P1275 mode not selected'}
 $active=P125-Fields (P12-Query 'p1274_status')
 if($active.state -ne '2' -or [int]$active.profile -ne $ids[$opts.Profile]){throw 'Profile not active'}
 [void](P12741-LiveGuard)
 if($opts.Power -eq 'Offline'){
  $wait=[int][Math]::Ceiling($script:P12741Deadline-[double]$active.now/[double]$s.hz)+5
  P125-Offline $wait
 }else{
  $until=(Life-Now).AddSeconds($script:P12741Deadline+10)
  while((Life-Now) -lt $until){
   $state=P125-Fields (P12-Query 'p1274_status')
   if($state.state -eq '3'){break}
   P125-PumpOff 2
  }
 }
 $f=Read-Frozen $meta;$script:dirty=$false
 Life-Say "FROZEN reason=$($f.reason). Keep same boot; run Dump when cool. No automatic policy progression."
}
function Dump($meta){
 $s=Read-Frozen $meta
 $info=P125-Fields (Get-Content -Raw -LiteralPath (Join-Path $opts.RunDir 'episode-info.txt'))
 [void](P12741-LiveGuard)
 $t=P125-Fields (Get-Content -Raw -LiteralPath (Join-Path $opts.RunDir 'trace-status.txt'))
 $file=Join-Path $opts.RunDir 'pages.txt';$cache=Page-Cache $file 'p1274_page' 'select'
 function Read-P4([int]$n,[int]$kind){
  $line=Get-Page $cache $file 'p1274_select' 'p1274_page' 'select' $n
  $v=P125-Fields $line
  if([int]$v.kind -ne $kind){throw 'P1274 kind mismatch'}
  if($kind -eq 2 -and [int]$v.seq -ne ($n-511)){throw 'FG seq mismatch'}
  if($kind -eq 7 -and ([int]$v.samples -gt 40 -or [uint64]$v.end -lt [uint64]$v.at -or [uint64]$v.end -gt [uint64]$s.end)){throw 'Episode coverage invalid'}
  if($kind -eq 8){
   if([int]$v.episode -ne [int][Math]::Floor(($n-13000)/40) -or [int]$v.index -ne (($n-13000)%40)){throw 'Episode sample identity mismatch'}
   $ep=P125-Fields $cache[(5100+[int]$v.episode)]
   if([int]$v.index -ge [int]$ep.samples -or [uint64]$v.at -lt [uint64]$ep.at -or [uint64]$v.at -gt [uint64]$ep.end){throw 'Episode sample outside its episode'}
  }
  if($kind -eq 4 -and $v.present -eq '1' -and ([uint64]$v.bin%128) -ne ($n-4096)){throw 'Detail ring mismatch'}
  if($kind -ne 5 -and -not ($kind -eq 4 -and $v.present -eq '0')){
   if([uint64]$v.at -lt [uint64]$s.start -or [uint64]$v.at -gt [uint64]$s.end){throw 'Page outside phase'}
  }
  Commit-Page $cache $file $n $line
 }
 [void](Restore-Sample)
 # First batch contains immutable peak, episode headers and high-value
 # pages. Do not spend the first several batches on exhaustive owner rows.
 foreach($i in (Range-N 5100 ([int]$info.episodes))){Read-P4 $i 7}
 if([int]$info.peak_select -ne 0){
  $center=[int]$info.peak_select
  $lo=[Math]::Max(6000,$center-8);$hi=[Math]::Min(6000+[int]$s.rows-1,$center+8)
  Read-P4 $center 1
  foreach($i in $lo..$hi){Read-P4 $i 1}
 }
 $p1275=$meta.PSObject.Properties['p1275_mode'] -and [int]$meta.p1275_mode
 if($p1275){
  $focus=P12-Query 'p1275_focus';Stable-Line (Join-Path $opts.RunDir 'p1275-focus.txt') $focus
  foreach($hit in [regex]::Matches($focus,' s=(\d+)')){
   $n=[int]$hit.Groups[1].Value
   if($n -lt 8192 -or $n -ge 8192+[int]$s.events){throw 'P1275 focus outside frozen events'}
   Read-P4 $n 3
  }
  # Latest DVFS decisions include the emergency tail and experimental load cap.
  $first=[Math]::Max(0,[int]$s.rate_events-16)
  foreach($i in (Range-N $first ([int]$s.rate_events-$first))){Read-P4 (3072+$i) 6}
 }
 $recent=[Math]::Min(24,[int]$s.events)
 for($i=[int]$s.events-1;$i -ge [int]$s.events-$recent;$i--){Read-P4 (8192+$i) 3}
 # Sample a bounded high-severity slice before the first major owner page.
 $episodeRows=@($cache.Values|ForEach-Object {P125-Fields $_}|Where-Object kind -eq '7'|Sort-Object @{Expression={[int]$_.severity};Descending=$true},@{Expression={[int]$_.peak};Descending=$true})
 if($episodeRows.Count){
  $ep=$episodeRows[0];$i=[int]$ep.select-5100
  foreach($j in (Range-N (13000+$i*40) ([Math]::Min(4,[int]$ep.samples)))){Read-P4 $j 8}
 }
 function Read-ScalarRemainder{
  foreach($ep in $episodeRows){
   $i=[int]$ep.select-5100
   foreach($j in (Range-N (13000+$i*40) ([int]$ep.samples))){Read-P4 $j 8}
  }
  foreach($i in (Range-N 512 ([int]$s.fg))){Read-P4 $i 2}
  foreach($i in (Range-N 3072 ([int]$s.rate_events))){Read-P4 $i 6}
  foreach($i in (Range-N 6000 ([int]$s.rows))){Read-P4 $i 1}
  for($i=[int]$s.events-1;$i -ge 0;$i--){Read-P4 (8192+$i) 3}
  foreach($i in (Range-N 4096 128)){Read-P4 $i 4}
  foreach($i in (Range-N 5000 8)){Read-P4 $i 5}
 }
 if($t.state -eq '4'){
  $gf=Join-Path $opts.RunDir 'gpu-status.txt';$gl=P12-Query 'p1273_gpu_status';$g=P125-Fields $gl
  if($g.generation -ne $t.generation -or [uint64]$g.completed -gt [uint64]$g.submitted){throw 'GPU generation/coverage mismatch'}
  Stable-Line $gf $gl
  $gp=Join-Path $opts.RunDir 'gpu-pages.txt';$gc=Page-Cache $gp 'p1273_gpu_page' 'slot'
  if([uint64]$g.first_wake){
   $line=Get-Page $gc $gp 'p1273_gpu_select' 'p1273_gpu_page' 'slot' 1024;$v=P125-Fields $line
   if($v.generation -ne $t.generation -or $v.seq -ne $g.first_wake -or $v.wake_ok -ne '1'){throw 'First wake mismatch'}
   Commit-Page $gc $gp 1024 $line
  }
  $tf=Join-Path $opts.RunDir 'trace-pages.txt';$tc=Page-Cache $tf 'p1273_trace_page' 'select'
  $cpuMasks=@{}
  for($cpu=0;$cpu -lt 8;$cpu++){
   $cl=P12-Query "p1273_trace_cpu$cpu";$c=P125-Fields $cl
   if($c.generation -ne $t.generation -or [int]$c.cpu -ne $cpu){throw 'CPU coverage mismatch'}
   Stable-Line (Join-Path $opts.RunDir "trace-cpu$cpu.txt") $cl
   $cpuMasks[$cpu]=@($c.mask.Split('/')|ForEach-Object {P125-Hex $_})
  }
  $center=if([uint64]$t.trigger -ge [uint64]$t.start){[int]([Math]::Floor(([double]$t.trigger-[double]$t.start)/[double]$t.span)%128)}else{0}
  $slots=@(0..127|Sort-Object { $d=[Math]::Abs($_-$center);[Math]::Min($d,128-$d) })
  $big=P125-Hex $s.bigmask
  $order=@(0..7|Sort-Object {if($big -band ([uint64]1 -shl $_)){0}else{1}})
  function Read-OwnerBand($bandSlots,$bandGroups){
  foreach($group in $bandGroups){
   foreach($slot in $bandSlots){
    foreach($cpu in $order){
     if((($big -band ([uint64]1 -shl $cpu)) -ne 0) -ne ($group -eq 'BIG')){continue}
     $m=$cpuMasks[$cpu]
     if(-not ($m[[int][Math]::Floor($slot/64)] -band ([uint64]1 -shl ($slot%64)))){continue}
     $mask=0;$bin=''
     for($p=0;$p -lt 8;$p++){
      if($p -gt 0 -and -not ($mask -band (3 -shl ($p*2)))){continue}
      $sel=$cpu*1024+$slot*8+$p
      $line=Get-Page $tc $tf 'p1273_trace_select' 'p1273_trace_page' 'select' $sel
      $v=P125-Fields $line
      if($v.generation -ne $t.generation -or $v.present -ne '1' -or ([uint64]$v.bin%128) -ne $slot){throw 'Trace page mismatch'}
      if($p -eq 0){$mask=P125-Hex $v.mask;$bin=$v.bin}
      if((P125-Hex $v.mask) -ne $mask -or $v.bin -ne $bin -or $mask -gt 65535){throw 'Trace bucket mismatch'}
      $expected=0;foreach($i in 0,1){if($mask -band (1 -shl ($p*2+$i))){$expected++}}
      if(([regex]::Matches($line,' r=')).Count -ne $expected){throw 'Trace owner coverage mismatch'}
      Commit-Page $tc $tf $sel $line
     }
    }
   }
  }
  }
  # Obtain a single closest BIG bin before the exhaustive scalar streams.
  if([uint64]$t.trigger){
   $near=@($slots|Where-Object {
    $slot=$_;$covered=$false
    foreach($cpu in $order){
     if(($big -band ([uint64]1 -shl $cpu)) -and ($cpuMasks[$cpu][[int][Math]::Floor($slot/64)] -band ([uint64]1 -shl ($slot%64)))){$covered=$true;break}
    }
    $covered
   }|Select-Object -First 1)
   if($near.Count){Read-OwnerBand $near @('BIG')}
  }
  if($p1275){
   # A few nearby bins on both clusters before long scalar remainder.
   Read-OwnerBand @($slots|Select-Object -First 4) @('BIG','LITTLE')
  }
  Read-ScalarRemainder
  Read-OwnerBand $slots @('BIG','LITTLE')
  $n=[uint64]$g.completed;$first=[Math]::Max(1,[double]$n-1023)
  for($i=[uint64]$first;$i -le $n;$i++){
   $slot=[int](($i-1)%1024)
   $line=Get-Page $gc $gp 'p1273_gpu_select' 'p1273_gpu_page' 'slot' $slot;$v=P125-Fields $line
   if([uint64]$v.seq -ne $i -or $v.generation -ne $t.generation -or [uint64]$v.queued -gt [uint64]$v.began -or [uint64]$v.began -gt [uint64]$v.ended){throw 'GPU page mismatch'}
   Commit-Page $gc $gp $slot $line
  }
 }else{Read-ScalarRemainder}
 [void](Read-Frozen $meta)
 [void](Restore-Sample)
 Set-Content -Encoding UTF8 -LiteralPath (Join-Path $opts.RunDir 'dump.complete') -Value $meta.nonce
 Life-Say 'DUMP COMPLETE; review raw coverage, then WakeCheck and Analyze.'
}
function Analyze-Run([string]$dir){
 $m=Load-Json (Join-Path $dir 'capture.json')
 if($m.version -ne 2){throw 'Use original P1274 kit for revision 1 logs'}
 $info=P125-Fields (Get-Content -Raw -LiteralPath (Join-Path $dir 'episode-info.txt'))
 $s=P125-Fields (Get-Content -Raw -LiteralPath (Join-Path $dir 'status.txt'))
 $cache=Page-Cache (Join-Path $dir 'pages.txt') 'p1274_page' 'select'
 $complete=(Test-Path -LiteralPath (Join-Path $dir 'dump.complete')) -and ((Get-Content -Raw -LiteralPath (Join-Path $dir 'dump.complete')).Trim() -eq $m.nonce)
 $rows=@($cache.Values|ForEach-Object {P125-Fields $_})
 $timeline=@($rows|Where-Object kind -eq '1'|Sort-Object {[uint64]$_.at})
 $fg=@($rows|Where-Object kind -eq '2'|Sort-Object {[uint64]$_.at})
 $events=@($rows|Where-Object kind -eq '3')
 $hz=[double]$m.hz
 $peak=$null;if([int]$info.peak -ge 0){$peak=[int]$info.peak/1000}
 $episodes=@($rows|Where-Object kind -eq '7');$epSamples=@($rows|Where-Object kind -eq '8')
 $dur=$null;if([uint64]$s.burst_start -gt 0 -and [uint64]$s.burst_end -gt [uint64]$s.burst_start){$dur=([double]$s.burst_end-[double]$s.burst_start)/$hz}
 $calls=$null;$api=@($events|Where-Object event -eq '2')
 if($api.Count){$calls=($api|Group-Object { $_.task }|ForEach-Object {($_.Group|ForEach-Object {[double]$_.a}|Measure-Object -Maximum).Maximum}|Measure-Object -Sum).Sum}
 $energy=$null;$qh=$null;$validPower=$false;$qhSeconds=$null
 # Fixed common window 250..710s; include bracketing samples, no extrapolation.
 $lo=250*$hz;$hi=710*$hz
 $pre=@($fg|Where-Object {[double]$_.at -le $lo}|Select-Object -Last 1)
 $post=@($fg|Where-Object {[double]$_.at -ge $hi}|Select-Object -First 1)
 if($pre.Count -eq 1 -and $post.Count -eq 1){
  $w=@($fg|Where-Object {[uint64]$_.at -ge [uint64]$pre[0].at -and [uint64]$_.at -le [uint64]$post[0].at})
  $validPower=($w.Count -ge 2);$sum=0.0;$dq=0
  $want=if($m.power -eq 'Offline'){'0'}else{'1'}
  foreach($v in $w){if($v.external -ne $want -or ($m.power -eq 'Offline' -and [int]$v.ua -ge 0) -or [double]$v.uv -lt 2500000 -or [double]$v.uv -gt 5000000 -or [int]$v.qh -gt 65535){$validPower=$false}}
  for($i=1;$i -lt $w.Count;$i++){
   $a=$w[$i-1];$b=$w[$i];$dt=([double]$b.at-[double]$a.at)/$hz
   if($dt -le 0 -or $dt -gt 15 -or ([uint64]$b.seq-[uint64]$a.seq) -ne 1){$validPower=$false;continue}
   $delta=(([int]$b.qh-[int]$a.qh+98304)%65536)-32768
   if([Math]::Abs($delta) -gt 4096){$validPower=$false}
   $dq+=$delta
   $p0=-[double]$a.ua*[double]$a.uv/1e12;$p1=-[double]$b.ua*[double]$b.uv/1e12
   $x=[Math]::Max($lo,[double]$a.at);$y=[Math]::Min($hi,[double]$b.at)
   if($y -gt $x){$span=[double]$b.at-[double]$a.at;$px=$p0+($p1-$p0)*($x-[double]$a.at)/$span;$py=$p0+($p1-$p0)*($y-[double]$a.at)/$span;$sum+=($px+$py)/2*($y-$x)/$hz/3600}
  }
  if($m.power -eq 'Offline'){
   if([uint64]$s.usb_down -eq 0 -or [double]$s.usb_down -gt $lo -or ([uint64]$s.usb_up -ne 0 -and [double]$s.usb_up -lt $hi)){$validPower=$false}
   if($validPower){$energy=$sum;$qh=$dq;$qhSeconds=([double]$w[-1].at-[double]$w[0].at)/$hz}
  }elseif([uint64]$s.usb_down -ne 0){$validPower=$false}
 }
 $rest=$false;$wake=$false;$guardPeakBig=$null;$guardPeakLit=$null
 if(Test-Path -LiteralPath (Join-Path $dir 'restore.json')){
  $restore=Load-Json (Join-Path $dir 'restore.json');$rest=$restore.ok
  if($restore.PSObject.Properties['guard']){
   if([int]$restore.guard.peak_big -ge 0){$guardPeakBig=[int]$restore.guard.peak_big/1000}
   if([int]$restore.guard.peak_lit -ge 0){$guardPeakLit=[int]$restore.guard.peak_lit/1000}
  }
 }
 if(Test-Path -LiteralPath (Join-Path $dir 'wake.json')){$wake=(Load-Json (Join-Path $dir 'wake.json')).ok}
 $capOK=$false
 if($m.profile -in @('B1','B2','B3')){$capOK=([uint64]$s.measure_ok -ge 5 -and [uint64]$s.measure_bad -eq 0)}
 elseif($m.profile -in @('R2','R1','R0')){
  $need=@{R2=2;R1=1;R0=0}[$m.profile];$late=@($timeline|Where-Object {[double]$_.at/$hz -ge 240 -and [double]$_.at/$hz -lt 710})
  $capOK=($late.Count -ge 100)
  foreach($v in $late){
   $actual=P125-Hex $v.recommended;$requested=P125-Hex $v.requested;$big=P125-Hex $s.bigmask;$lit=P125-Hex $s.litmask
   $n=0;for($k=0;$k -lt 8;$k++){if(($actual -band $big) -band ([uint64]1 -shl $k)){$n++}}
   if($actual -ne $requested -or ($actual -band $lit) -ne $lit -or $n -ne $need -or [int]$v.online -ne 68){$capOK=$false}
  }
 }
 # Export typed tables from retained pages, including partial captures.
 $stages=@(foreach($e in $events|Where-Object event -eq '3'|Where-Object {[int]$_.a -lt 100 -and [int]$_.a -notin @(20,21) -and [uint64]$_.e -gt 0}){
  [pscustomobject]@{pid=$e.pid;tid=$e.tid;stage=$e.a;start_s=[double]$e.e/$hz;end_s=[double]$e.at/$hz;wall_ms=1000*[double]$e.b/$hz;thread_cpu_ms=if([uint64]$e.c -eq [uint64]::MaxValue){$null}else{[double]$e.c/1000000};success=$e.d}
 })
 if($stages.Count){$stages|Sort-Object start_s|Export-Csv -NoTypeInformation -Encoding UTF8 -LiteralPath (Join-Path $dir 'metal-stages.csv')}
 $usage=@(foreach($e in $events|Where-Object event -eq '6'|Where-Object {[int]$_.a -lt 100}){
  [pscustomobject]@{pid=$e.pid;tid=$e.tid;stage=$e.a;start_s=[double]$e.f/$hz;end_s=[double]$e.at/$hz;process_user_us=$e.b;process_system_us=$e.c;process_minor_faults=$e.d;process_major_faults=$e.e}
 })
 if($usage.Count){$usage|Sort-Object start_s|Export-Csv -NoTypeInformation -Encoding UTF8 -LiteralPath (Join-Path $dir 'metal-process-usage.csv')}
 $tables=@{episodes=$episodes;'p1275-events'=@($events|Where-Object event -eq '6');'episode-samples'=$epSamples;timeline=$timeline;'fuel-gauge'=$fg;'user-events'=$events;detail=@($rows|Where-Object kind -eq '4');dvfs=@($rows|Where-Object kind -eq '6');wfi=@($rows|Where-Object kind -eq '5')}
 foreach($name in $tables.Keys){
  $data=@($tables[$name]);if($data.Count){$data|ForEach-Object {[pscustomobject]$_}|Export-Csv -NoTypeInformation -Encoding UTF8 -LiteralPath (Join-Path $dir ($name+'.csv'))}
 }
 $tracePath=Join-Path $dir 'trace-pages.txt'
 if(Test-Path -LiteralPath $tracePath){
  $owners=@(foreach($line in Get-Content -LiteralPath $tracePath){
   $f=P125-Fields $line
   $cpu=[int][Math]::Floor([int]$f.select/1024)
   foreach($hit in [regex]::Matches($line,' r=([0-9/]+)')){
    $a=$hit.Groups[1].Value.Split('/')
    [pscustomobject]@{cpu=$cpu;bin=[uint64]$f.bin;tid=$a[0];task=$a[1];pid=$a[2];kind=$a[3];user_ms=1000*[double]$a[4]/$hz;system_ms=1000*[double]$a[5]/$hz;fragments=$a[6];switches=$a[7]}
   }
  })
  if($owners.Count){
   $owners|Export-Csv -NoTypeInformation -Encoding UTF8 -LiteralPath (Join-Path $dir 'trace-owners.csv')
   # Observed CPU placement, not an exact chronological migration trace.
   $big=P125-Hex $s.bigmask
   $placement=@(foreach($group in $owners|Where-Object kind -eq '2'|Group-Object pid,task,tid,bin){
    $v=$group.Group[0];$cpus=@($group.Group.cpu|Sort-Object -Unique);$b=@($cpus|Where-Object {($big -band ([uint64]1 -shl [int]$_)) -ne 0});$l=@($cpus|Where-Object {($big -band ([uint64]1 -shl [int]$_)) -eq 0})
    [pscustomobject]@{pid=$v.pid;task=$v.task;tid=$v.tid;bin=$v.bin;cpus=($cpus -join '/');big=($b -join '/');little=($l -join '/');cross_cluster_observed=($b.Count -gt 0 -and $l.Count -gt 0)}
   })
   if($placement.Count){$placement|Export-Csv -NoTypeInformation -Encoding UTF8 -LiteralPath (Join-Path $dir 'thread-placement.csv')}
  }
 }
 $expectedEpisodeSamples=0;foreach($ep in $episodes){$expectedEpisodeSamples+=[int]$ep.samples}
 $coverage=($timeline.Count -eq [int]$s.rows -and $fg.Count -eq [int]$s.fg -and $events.Count -eq [int]$s.events -and $episodes.Count -eq [int]$info.episodes -and $epSamples.Count -eq $expectedEpisodeSamples -and [int]$info.episode_drops -eq 0 -and [int]$s.drops -eq 0 -and [int]$s.rate_drops -eq 0)
 $qualified=($complete -and $coverage -and $s.reason -eq '1' -and $rest -and $wake -and $capOK -and $null -ne $dur -and $null -ne $peak -and $peak -lt 76 -and $null -ne $guardPeakBig -and $guardPeakBig -lt 76 -and $null -ne $guardPeakLit -and $guardPeakLit -lt 70 -and $validPower)
 $experiment=if($m.PSObject.Properties['experiment']){$m.experiment}else{'Legacy'}
 $report=[pscustomobject]@{version=2;experiment=$experiment;episodes=[int]$info.episodes;episode_drops=[int]$info.episode_drops;profile=$m.profile;power=$m.power;nonce=$m.nonce;hz=$m.hz;reason=[int]$s.reason;dump_complete=$complete;coverage=$coverage;peak_big_c=$peak;guard_peak_big_c=$guardPeakBig;guard_peak_little_c=$guardPeakLit;burst_proxy_s=$dur;layout_calls_lower_bound=$calls;event_drops=[int]$s.drops;rate_drops=[int]$s.rate_drops;qh_raw_bracket_delta=$qh;qh_bracket_seconds=$qhSeconds;net_battery_wh_250_710=$energy;power_verified=$validPower;measure_ok=[uint64]$s.measure_ok;measure_bad=[uint64]$s.measure_bad;restore_ok=$rest;foreground_operator_ok=$wake;qualified=$qualified}
 Save-Json $report (Join-Path $dir 'summary.json')
 return $report
}
if($opts.LibraryOnly){return}
if(-not $opts.Execute){Write-Host "P1274 PREVIEW $($opts.Action) $($opts.Profile) $($opts.Power) experiment=$($opts.Experiment). No serial, no files. Add -Execute.";return}
if($opts.Action -eq 'Compare'){
 if($opts.CompareDirs.Count -ne 3){throw 'Specify -CompareDirs A0,B,A1'}
 $rs=@($opts.CompareDirs|ForEach-Object {Analyze-Run $_})
 if(@($rs.nonce|Select-Object -Unique).Count -ne 3 -or $rs[0].profile -ne $rs[2].profile -or $rs[0].experiment -ne $rs[2].experiment){throw 'A/B/A needs separate boots, matching A profiles'}
 $rs|Format-Table profile,power,reason,peak_big_c,burst_proxy_s,layout_calls_lower_bound,qh_raw_bracket_delta,net_battery_wh_250_710,power_verified,qualified -AutoSize
 Write-Host 'Descriptive only: check matched workloads, drops, censored phases, power and start temperature. No automatic causal claim.'
 return
}
if($opts.Action -eq 'Analyze'){if(-not $opts.RunDir){throw 'RunDir required'};Analyze-Run $opts.RunDir|Format-List;return}
if($env:OS -ne 'Windows_NT'){throw 'Windows serial only'}
if($opts.Action -eq 'Capture'){
 if(-not $opts.RunDir){$opts.RunDir=Join-Path (Get-Location).Path ("logs/P1274-"+$opts.Profile+'-'+$opts.Power+'-'+(Get-Date -Format yyyyMMdd-HHmmss))}
 if(Test-Path -LiteralPath $opts.RunDir){throw 'Capture RunDir already exists'}
 Check-Prior
 [void](New-Item -ItemType Directory -Path $opts.RunDir -Force)
}else{if(-not $opts.RunDir -or -not (Test-Path -LiteralPath (Join-Path $opts.RunDir 'capture.json'))){throw 'Existing capture RunDir required'}}
$opts.RunDir=(Resolve-Path -LiteralPath $opts.RunDir).Path
$stem=Join-Path $opts.RunDir ($opts.Action+'-'+(Get-Date -Format yyyyMMdd-HHmmss-fff))
$script:P12Journal="$stem.journal.txt";$script:P12Phase='P1274-'+$opts.Action
try{
 $script:LifeRaw=[IO.FileStream]::new("$stem.log",[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read)
 Life-Say 'SERIAL BACKLOG: raw lines before current clock/nonce ACK are historical or time-unclassified; not live temperature samples'
 P125-Open $opts.Port;Life-SyncReadOnly
 P12741-RequireRevision
 $sync=P125-Status;$nonce=P125-Fields (P12-Query 'p1273_boot_identity')
 Save-Json ([pscustomobject]@{version=2;host_utc=[DateTime]::UtcNow.ToString('o');device_now=$sync.now;hz=$sync.hz;nonce=$nonce.nonce;raw_before_sync='historical_or_unclassified'}) "$stem.sync.json"
 switch($opts.Action){
  Capture {Capture}
  Dump {Dump (Load-Json (Join-Path $opts.RunDir 'capture.json'))}
  WakeCheck {
   $m=Load-Json (Join-Path $opts.RunDir 'capture.json');[void](Read-Frozen $m)
   if(-not (Restore-Sample)){throw 'Policy not fully restored; do not force release; cool first'}
   if(-not (Test-Path -LiteralPath (Join-Path $opts.RunDir 'dump.complete')) -or -not $opts.ConfirmForeground -or $script:LifeState -ne 'ON'){throw 'After completed dump, test screen ON, touch, UI, radio/audio manually; rerun with -ConfirmForeground only if successful'}
   Save-Json ([pscustomobject]@{ok=$true;operator_attestation=$true;utc=[DateTime]::UtcNow.ToString('o')}) (Join-Path $opts.RunDir 'wake.json')
  }
 }
}catch{
 if($_.Exception.Message -eq 'P1274_BATCH_LIMIT'){Life-Say "BATCH COMPLETE. Resume -Action Dump -RunDir '$($opts.RunDir)' -Execute when cool."}
 else{throw}
}finally{
 if($script:dirty -and $null -ne $script:LifeSerial -and $script:LifeSerial.IsOpen){
  try{[void](P12-Query 'p12741_profile' '0' -Cleanup)}catch{Write-Warning 'Stop ACK unavailable; kernel lease expires autonomously; thermal guard remains armed.'}
 }
 if($null -ne $script:LifeSerial){$script:LifeSerial.Dispose()}
 if($null -ne $script:LifeRaw){$script:LifeRaw.Dispose();$script:LifeRaw=$null}
 $script:LifeSamples|Export-Csv -NoTypeInformation -Encoding UTF8 -LiteralPath "$stem.samples.csv"
 Write-Host "P1274 run directory: $($opts.RunDir)"
}
