#requires -version 5.1
# Read-only phase-aware analyzer for one PuTTY G3D Power Lab A/B log.
param([Parameter(Mandatory=$true)][string]$Log)
$ErrorActionPreference='Stop'
$f=(Resolve-Path -LiteralPath $Log).Path
$lines=[IO.File]::ReadAllLines($f)
$ph=[Collections.Generic.List[object]]::new()
$pw=[Collections.Generic.List[object]]::new()
$stage=[Collections.Generic.List[object]]::new()
$cpu=[Collections.Generic.List[object]]::new()
$fps=[Collections.Generic.List[object]]::new()
$poster=[Collections.Generic.List[object]]::new()
$p0=[Collections.Generic.List[object]]::new()
$govAt=0
function KV([string]$s,[string]$key,[string]$default='') {
  if($s -match ('(?<!\w)'+[regex]::Escape($key)+'=([^\s]+)')){return $Matches[1]}
  return $default
}
foreach($s in $lines){
 if($s -match '^g3d-lab: PHASE=(\d+) up=(\d+)ms'){
   $ph.Add([pscustomobject]@{phase=[int]$Matches[1];t=[double]$Matches[2]/1000})
   continue
 }
 if($s -match '^g3d-lab: up=(\d+)ms phase=(\d+) mode='){
   $pw.Add([pscustomobject]@{
      t=[double]$Matches[1]/1000;phase=[int]$Matches[2]
      mode=[int](KV $s 'mode' '0');domain=[int](KV $s 'domain' '-1')
      safe=[int](KV $s 'gpu_safe' '0');tz=[int](KV $s 'tz_ok' '0');offMs=[double](KV $s 'off_total_ms' '0')
      target=[int](KV $s 'target_mv' '0');rail=[int](KV $s 'rail_mv' '0')
      railValid=[int](KV $s 'rail_valid' '0')
      off=[int](KV $s 'off_count' '0');wake=[int](KV $s 'wake_count' '0')
      fail=[int](KV $s 'wake_fail' '0');fault=[int](KV $s 'fault' '0')
      wakeUs=[double](KV $s 'wake_last_us' '0');temp=[double](KV $s 'g3d_mc' '0')/1000
   })
   continue
 }
 if($s -match '^cpufreq_gov: (\d+) s:'){
   $govAt=[double]$Matches[1]+15
   if($s -match 'temp big ([\d.]+) lit ([\d.]+) C'){
      $cpu.Add([pscustomobject]@{t=$govAt;big=[double]$Matches[1]})
   }
   continue
 }
 if($s -match '^fps: (\d+) refreshes missed.*?gpu: (\d+)% busy, .*?, (\d+) MHz .*?G3D (\d+) C'){
   $fps.Add([pscustomobject]@{t=$govAt;miss=[int]$Matches[1];busy=[int]$Matches[2];mhz=[int]$Matches[3];g3d=[int]$Matches[4]})
   continue
 }
 if($s -match '^hakios-gl-stage: up=([\d.]+)s frames=.*? n=(\d+) draw=(\d+)us .*? total=(\d+)us'){
   $stage.Add([pscustomobject]@{t=[double]::Parse($Matches[1],[Globalization.CultureInfo]::InvariantCulture);n=[int]$Matches[2];draw=[long]$Matches[3];total=[long]$Matches[4]})
   continue
 }
 if($s -match '^hakios-gl-p0: up=([\d.]+)s frames=.*? fast=(\d+) scalar=(\d+) verified=(\d+) mismatch=(\d+) fallback=(\d+)'){
   $p0.Add([pscustomobject]@{t=[double]::Parse($Matches[1],[Globalization.CultureInfo]::InvariantCulture);verify=[int]$Matches[4];bad=[int]$Matches[5]})
   continue
 }
 if($s -match '^poster_cost: \d+ s: PosterBoard\[\d+\] cpu=([\d.]+)% .*? up=(\d+)s sample'){
   $poster.Add([pscustomobject]@{t=[double]$Matches[2];cpu=[double]$Matches[1]})
   continue
 }
}
function Sm($data,[string]$field){
 $v=@($data);if(!$v.Count){return 0};return ($v|Measure-Object -Property $field -Sum).Sum
}
function Av($data,[string]$field){
 $v=@($data);if(!$v.Count){return 'N/A'};return [math]::Round(($v|Measure-Object -Property $field -Average).Average,2)
}
Write-Output ('G3D Power Lab | file='+$f+' | lines='+$lines.Count+' phase_marks='+$ph.Count+' power_samples='+$pw.Count)
if(!$ph.Count){Write-Warning 'No g3d_lab_phase sysctl markers were found';exit}
$timeline=@($ph|Sort-Object t)
for($i=0;$i -lt $timeline.Count;$i++){
 $x=$timeline[$i];$next=if($i+1 -lt $timeline.Count){$timeline[$i+1].t}else{[double]::PositiveInfinity}
 $q=@($pw|Where-Object{$_.t -ge $x.t -and $_.t -lt $next})
 $c=@($cpu|Where-Object{$_.t -ge $x.t -and $_.t -lt $next})
 $g=@($fps|Where-Object{$_.t -ge $x.t -and $_.t -lt $next})
 $r=@($stage|Where-Object{$_.t -ge $x.t -and $_.t -lt $next})
 $p=@($p0|Where-Object{$_.t -ge $x.t -and $_.t -lt $next})
 $po=@($poster|Where-Object{$_.t -ge $x.t -and $_.t -lt $next})
 $frames=Sm $r n;$frameUs=0
 foreach($one in $r){$frameUs+=$one.total*$one.n}
 $gl=if($frames){[math]::Round($frameUs/$frames/1000,3)}else{'N/A'}
 $startW=$q|Select-Object -First 1;$endW=$q|Select-Object -Last 1
 $wakeDelta=if($q.Count){$endW.wake-$startW.wake}else{0}
 $offDelta=if($q.Count){$endW.off-$startW.off}else{0}
 $offMsDelta=if($q.Count){$endW.offMs-$startW.offMs}else{0}
 Write-Output ('PHASE '+$x.phase+' t='+[math]::Round($x.t,1)+'..'+[math]::Round($next,1)+
   's n='+$q.Count+' domainOff='+@($q|Where-Object{$_.domain -eq 0}).Count+
   ' mvTarget='+(Av $q target)+' railReadAvg='+(Av @($q|Where-Object{$_.railValid}) rail)+
   ' GPUbusy='+(Av $g busy)+' G3D='+ (Av $g g3d)+'C BIG='+(Av $c big)+
   'C GLstage='+$gl+'ms/frame missed='+(Sm $g miss)+
   ' gpuClocks='+(@($g|Group-Object mhz|ForEach-Object{$_.Name+'MHz:'+ $_.Count}) -join ',')+
   ' offCountEnd='+$(if($q.Count){$endW.off}else{'N/A'})+' offDelta='+$offDelta+' offMs='+$offMsDelta+
   ' unsafeWhileOn='+@($q|Where-Object{$_.domain -eq 1 -and ($_.safe -eq 0 -or $_.tz -eq 0)}).Count+' wakes='+$wakeDelta+
   ' wakeLastUs='+(Av $q wakeUs)+' posterCpu='+(Av $po cpu)+
   ' P0bad='+(Sm $p bad)+' faults='+@($q|Where-Object{$_.fault -ne 0}).Count)
}
Write-Output 'IMPORTANT: PMU residency and PMIC voltage are NOT measured electrical power (watts).'
Write-Output 'CPU, frame timings and temperatures must be compared across repeated A/B phases and similar initial conditions.'
