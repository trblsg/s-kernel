#requires -version 5.1
# S9 P5 read-only PuTTY offline report. Input is never modified.
param([Parameter(Mandatory=$true)][string]$Log)
$ErrorActionPreference='Stop'
$lines=[IO.File]::ReadAllLines((Resolve-Path -LiteralPath $Log).Path)
$ph=[Collections.Generic.List[object]]::new()
$gpu=[Collections.Generic.List[object]]::new()
$owners=[Collections.Generic.List[object]]::new()
$gl=[Collections.Generic.List[object]]::new()
$burst=[Collections.Generic.List[object]]::new()
$display=[Collections.Generic.List[object]]::new()
$threads=[Collections.Generic.List[object]]::new()
$bb=[Collections.Generic.List[object]]::new()
$off=0;$on=0
function F([string]$s,[string]$key,[string]$default='0') {
 $m=[regex]::Match($s,'(?<![A-Za-z0-9_])'+[regex]::Escape($key)+'=([^\s]+)')
 if($m.Success){return $m.Groups[1].Value}
 return $default
}
function Sum($items,[string]$key) {
 $a=@($items)
 if(!$a.Count){return 0}
 return ($a|Measure-Object -Property $key -Sum).Sum
}
function Mean($items,[string]$key) {
 $a=@($items)
 if(!$a.Count){return 'n/a'}
 return [math]::Round(($a|Measure-Object -Property $key -Average).Average,2)
}
foreach($s in $lines){
 if($s -match '^p5: PHASE=(\d+) up=(\d+)ms'){
  $ph.Add([pscustomobject]@{phase=[int]$Matches[1];t=[double]$Matches[2]/1000})
 }
 elseif($s -match '^p5-gpu: up=(\d+)ms phase=(\d+) mode=(\d+)'){
  $gpu.Add([pscustomobject]@{
   t=[double]$Matches[1]/1000;ph=[int]$Matches[2];mode=[int]$Matches[3]
   domain=[int](F $s 'domain');awake=[int](F $s 'awake')
   clock=[int]((F $s 'clock_hint').Replace('MHz',''))
   frameAge=[long](F $s 'frame_age_ms');jobAge=[long](F $s 'job_age_ms')
   submitted=[long](F $s 'submitted');jobs=[long](F $s 'completed')
   frag=[long](F $s 'fragment');vertex=[long](F $s 'vertex')
   errors=[int](F $s 'errors');dropped=[int](F $s 'dropped')
   owners=[int](F $s 'owners');overflow=[int](F $s 'overflow')
   peak=[int](F $s 'peak_jobs_1s');maxQueue=[int](F $s 'queue_peak')
   wait=[double](F $s 'worker_wait_us_avg');run=[double](F $s 'worker_run_us_avg')
  })
 }
 elseif($s -match '^p5-owner: up=(\d+)ms phase=(\d+) owner=(\d+)'){
  $owners.Add([pscustomobject]@{
   t=[double]$Matches[1]/1000;ph=[int]$Matches[2];owner=[int]$Matches[3]
   jobs=[int](F $s 'jobs');submitted=[int](F $s 'submitted')
   frag=[int](F $s 'fragment');vertex=[int](F $s 'vertex')
   dropped=[int](F $s 'dropped');failed=[int](F $s 'failed')
   wait=[long](F $s 'wait_us_avg');run=[long](F $s 'run_us_avg')
  })
 }
 elseif($s -match '^p5-gl: up=([\d.]+)s phase=(\d+) mode=(\d+) frames='){
  $gl.Add([pscustomobject]@{
   t=[double]::Parse($Matches[1],[Globalization.CultureInfo]::InvariantCulture)
   ph=[int]$Matches[2];mode=[int]$Matches[3]
   frames=[int](F $s 'n');draws=[long](F $s 'draws')
   large=[int](F $s 'large_draws');other=[int](F $s 'other_draws')
   clears=[long](F $s 'clears');uploads=[long](F $s 'uploads')
   checked=[long](F $s 'checked');changed=[long](F $s 'changed')
   skipped=[long](F $s 'skipped');changedFrames=[int](F $s 'changed_frames')
   uncheckedFrames=[int](F $s 'unchecked_frames')
   fast=[int](F $s 'fast17');fast33=[int](F $s 'fast33')
   slow=[int](F $s 'slow120');cpu=[long](F $s 'cpu_avg_us')
  })
 }
 elseif($s -match '^p5-gl-burst: up=([\d.]+)s phase=(\d+)'){
  $burst.Add([pscustomobject]@{
   t=[double]::Parse($Matches[1],[Globalization.CultureInfo]::InvariantCulture)
   ph=[int]$Matches[2];maxDraw=[int](F $s 'max_draw')
   maxUpload=[int](F $s 'max_uploads');changed=[int](F $s 'max_changed')
   maxGapMs=[long](F $s 'max_gap_ms');minGapUs=[long](F $s 'min_gap_us')
  })
 }
 elseif($s -match '^p5-gl-resume: up=([\d.]+)s phase=(\d+)'){
  Write-Output ('GL_RESUME '+$s)
 }
 elseif($s -match '^p5-display-thread: up=(\d+)s phase=(\d+)'){
  $display.Add([pscustomobject]@{
   t=[int]$Matches[1];ph=[int]$Matches[2]
   cpu=[double]::Parse((F $s 'cpu_if_ns'),[Globalization.CultureInfo]::InvariantCulture)
   run=[int](F $s 'run');policy=[int](F $s 'policy')
   priority=[int](F $s 'curpri')
  })
 }
 elseif($s -match '^p5-thread: up=(\d+)s phase=(\d+) proc=([^\s]+)'){
  $threads.Add([pscustomobject]@{
   t=[int]$Matches[1];ph=[int]$Matches[2];proc=$Matches[3];name=(F $s 'thr')
   cpu=[double]::Parse((F $s 'cpu_if_ns'),[Globalization.CultureInfo]::InvariantCulture)
   prio=[int](F $s 'curpri')
  })
 }
 elseif($s -match '^poster_cost: \d+ s: backboardd\[\d+\] cpu=([\d.]+)% .*?csw=(\d+) .*?up=(\d+)s sample'){
  $bb.Add([pscustomobject]@{t=[int]$Matches[3];cpu=[double]::Parse($Matches[1],[Globalization.CultureInfo]::InvariantCulture);csw=[int]$Matches[2]})
 }
 elseif($s -match '^GenericARMPE: the display off'){$off++}
 elseif($s -match '^GenericARMPE: the display on'){$on++}
}
Write-Output ('S9 P5 report input='+$Log+' lines='+$lines.Count)
Write-Output ('counts phases='+$ph.Count+' gpu='+$gpu.Count+' clients='+$owners.Count+' gl='+$gl.Count+' gl_burst='+$burst.Count+' display_thread='+$display.Count+' thread_lines='+$threads.Count+' bb='+$bb.Count+' displayOFF='+$off+' displayON='+$on)
Write-Output 'WARN: GPU owner is a driver client number, not a confirmed PID. Worker-run includes GPU wait and kernel overhead, not pure shader cycles.'
Write-Output 'WARN: P5 GL frame-end does not reveal original CoreAnimation timer/VSYNC trigger; checked IOSurfaces are a sparse sample and unchanged input does not imply identical pixels.'
Write-Output 'WARN: libproc thread CPU percentage assumes nanosecond counters on this S9 build, calibrate; run state is snapshot NOT wakeup count. CMU/PMU observation is not watt measurement.'
if(!$ph.Count){Write-Warning 'No p5_phase markers. Set kern.hakios.p5_phase=N in your test.';return}
$phases=@($ph|Sort-Object t)
for($i=0;$i -lt $phases.Count;$i++){
 $p=$phases[$i]
 $begin=$p.t
 $end=if($i+1 -lt $phases.Count){$phases[$i+1].t}else{[double]::PositiveInfinity}
 $gn=@($gpu|Where-Object{$_.t -ge $begin -and $_.t -lt $end})
 $en=@($gl|Where-Object{$_.t -ge $begin -and $_.t -lt $end})
 $bn=@($bb|Where-Object{$_.t -ge $begin -and $_.t -lt $end})
 $dn=@($display|Where-Object{$_.t -ge $begin -and $_.t -lt $end})
 $on=@($owners|Where-Object{$_.t -ge $begin -and $_.t -lt $end})
 $br=@($burst|Where-Object{$_.t -ge $begin -and $_.t -lt $end})
 $f=Sum $en frames;$dr=Sum $en draws
 $df=if($f){[math]::Round($dr/$f,2)}else{'n/a'}
 $endtext=if([double]::IsInfinity($end)){'end'}else{[math]::Round($end,1)}
 Write-Output ('PHASE '+$p.phase+' up='+[math]::Round($begin,1)+'..'+$endtext+
  ' GPU_N='+$gn.Count+' domain_on='+@($gn|Where-Object{$_.domain -eq 1}).Count+
  ' domain_off='+@($gn|Where-Object{$_.domain -eq 0}).Count+
  ' GPU_jobs='+(Sum $gn jobs)+' ownerN='+$on.Count+' max_1s='+(($gn|Measure-Object peak -Maximum).Maximum)+
  ' GPU_errors='+(Sum $gn errors)+' overflow='+(Sum $gn overflow)+
  ' GLframes='+$f+' GLdraws/frame='+$df+
  ' GL_checked='+(Sum $en checked)+' changed='+(Sum $en changed)+
  ' unchecked_frames='+(Sum $en uncheckedFrames)+' fast17='+(Sum $en fast)+
  ' BB_CPU='+ (Mean $bn cpu)+'% display_thr_CPU='+ (Mean $dn cpu)+
  '% P5_burst_windows='+$br.Count)
 if($on.Count){
  $top=$on|Group-Object owner|ForEach-Object{[pscustomobject]@{owner=$_.Name;jobs=Sum $_.Group jobs;frag=Sum $_.Group frag;vert=Sum $_.Group vertex}}|Sort-Object jobs -Descending|Select-Object -First 6
  foreach($x in $top){Write-Output ('  CLIENT owner='+$x.owner+' jobs='+$x.jobs+' fragment='+$x.frag+' vertex='+$x.vert)}
 }
 if($br.Count){
  $best=$br|Sort-Object maxDraw -Descending|Select-Object -First 1
  Write-Output ('  WAKE_BURST max_draw/frame='+$best.maxDraw+' max_upload='+$best.maxUpload+' max_changed='+$best.changed+' min_gap_us='+$best.minGapUs)
 }
}
Write-Output 'SAFE EXIT: sysctl kern.hakios.p5_mode=0 ; sysctl kern.hakios.p5_threads=0'
