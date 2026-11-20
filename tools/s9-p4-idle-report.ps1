#requires -version 5.1
# S9 P4: one-log, phase-aware read-only report. Does not edit the input.
param([Parameter(Mandatory=$true)][string]$Log)
$ErrorActionPreference='Stop'
$f=(Resolve-Path -LiteralPath $Log).Path
$lines=[IO.File]::ReadAllLines($f)
$ph=[Collections.Generic.List[object]]::new()
$cpu=[Collections.Generic.List[object]]::new()
$gl=[Collections.Generic.List[object]]::new()
$bb=[Collections.Generic.List[object]]::new()
$pt=[Collections.Generic.List[object]]::new()
$pc=[Collections.Generic.List[object]]::new()
$gf=[Collections.Generic.List[object]]::new()
$threadErrors=[Collections.Generic.List[object]]::new()
function K([string]$s,[string]$key,[string]$default='0') {
    $m=[regex]::Match($s,'(?<![A-Za-z0-9_])'+[regex]::Escape($key)+'=([^\s]+)')
    if($m.Success){return $m.Groups[1].Value}
    return $default
}
function A($rows,[string]$col) {
    $a=@($rows)
    if(!$a.Count){return 'n/a'}
    return [math]::Round(($a|Measure-Object -Property $col -Average).Average,2)
}
function S($rows,[string]$col) {
    $a=@($rows)
    if(!$a.Count){return 0}
    return ($a|Measure-Object -Property $col -Sum).Sum
}
foreach($line in $lines) {
    if($line -match '^p4: PHASE=(\d+) up=(\d+)ms') {
        $ph.Add([pscustomobject]@{ph=[int]$Matches[1];t=[double]$Matches[2]/1000})
        continue
    }
    if($line -match '^p4-cpu: up=(\d+)ms phase=(\d+) mode=(\d+)'){
        $cpu.Add([pscustomobject]@{
            t=[double]$Matches[1]/1000;ph=[int]$Matches[2];mode=[int]$Matches[3]
            lit=[int]((K $line 'idle_lit').TrimEnd('%'));big=[int]((K $line 'idle_big').TrimEnd('%'))
            val=(K $line 'valid');onlineL=[int](K $line 'lit_online');onlineB=[int](K $line 'big_online')
            frames=[int](K $line 'frames');touches=[int](K $line 'touches')
            gap0=[int](K $line 'gap_lt17');gap1=[int](K $line 'gap_17_50')
            gap2=[int](K $line 'gap_50_120');gap3=[int](K $line 'gap_ge120')
            mhz=(K $line 'mhz')})
        continue
    }
    if($line -match '^p4-gl: up=([\d.]+)s phase=(\d+) mode=(\d+) frames='){
        $gl.Add([pscustomobject]@{
            t=[double]::Parse($Matches[1],[Globalization.CultureInfo]::InvariantCulture)
            ph=[int]$Matches[2];mode=[int]$Matches[3]
            n=[int](K $line 'n');draws=[long](K $line 'draws')
            calls=[long](K $line 'gl_calls');checks=[long](K $line 'checked')
            same=[long](K $line 'same');changed=[long](K $line 'changed')
            skipped=[long](K $line 'skipped');candidate=[int](K $line 'candidate')
            checkedFrames=[int](K $line 'checked_frames');unchecked=[int](K $line 'unchecked')
            uploads=[long](K $line 'uploads')})
        continue
    }
    if($line -match '^poster_cost: \d+ s: backboardd\[\d+\] cpu=([\d.]+)% .*?csw=(\d+) .*?up=(\d+)s sample') {
        $bb.Add([pscustomobject]@{t=[double]$Matches[3];cpu=[double]::Parse($Matches[1],[Globalization.CultureInfo]::InvariantCulture);csw=[int]$Matches[2]})
        continue
    }
    if($line -match '^p4-thread: up=(\d+)s phase=(\d+) name=([^\s]+).*?cpu_if_mach=([\d.]+) cpu_if_ns=([\d.]+)'){
        $pt.Add([pscustomobject]@{
            t=[double]$Matches[1];ph=[int]$Matches[2];proc=$Matches[3]
            cpuMach=[double]::Parse($Matches[4],[Globalization.CultureInfo]::InvariantCulture)
            cpuNs=[double]::Parse($Matches[5],[Globalization.CultureInfo]::InvariantCulture)
            thread=(K $line 'thr')})
        continue
    }
    if($line -match '^p4-threads: up=(\d+)s phase=(\d+)'){
        $pc.Add([pscustomobject]@{t=[double]$Matches[1];ph=[int]$Matches[2]
            listed=[int](K $line 'listed');read=[int](K $line 'read')
            missing=[int](K $line 'missing');truncated=[int](K $line 'truncated')})
        continue
    }
    if($line -match '^p4-thread: .*unavailable='){$threadErrors.Add($line);continue}
    if($line -match '^g3d-lab: up=(\d+)ms phase=') {
        $gf.Add([pscustomobject]@{t=[double]$Matches[1]/1000;target=[int](K $line 'target_mv');domain=[int](K $line 'domain');fault=[int](K $line 'fault')})
        continue
    }
}
Write-Output ('S9 P4 one-log report: '+$f)
Write-Output ('lines='+$lines.Count+' markers='+$ph.Count+' cpu_samples='+$cpu.Count+' gl_bins='+$gl.Count+
    ' backboard_samples='+$bb.Count+' thread_bins='+$pc.Count+' thread_top_lines='+$pt.Count+
    ' thread_errors='+$threadErrors.Count)
Write-Output 'IMPORTANT: idle percentages are XNU scheduler idle, NOT WFI/CPD/SICD residency. GL shadow candidates are NOT safe-to-skip frames.'
Write-Output 'IMPORTANT: per-thread CPU values have two UNCALIBRATED time-unit interpretations; do not use as confirmed CPU percentages.'
if(!$ph.Count){Write-Warning 'No P4 phase markers found. Enable kern.hakios.p4_phase=N in the new kernel.';return}
$phases=@($ph|Sort-Object t)
for($i=0;$i -lt $phases.Count;$i++){
    $a=$phases[$i];$lo=$a.t;$hi=if($i+1 -lt $phases.Count){$phases[$i+1].t}else{[double]::PositiveInfinity}
    $c=@($cpu|Where-Object{$_.t -ge $lo -and $_.t -lt $hi})
    $g=@($gl|Where-Object{$_.t -ge $lo -and $_.t -lt $hi})
    $b=@($bb|Where-Object{$_.t -ge $lo -and $_.t -lt $hi})
    $q=@($pc|Where-Object{$_.t -ge $lo -and $_.t -lt $hi})
    $t=@($pt|Where-Object{$_.t -ge $lo -and $_.t -lt $hi})
    $v=@($gf|Where-Object{$_.t -ge $lo -and $_.t -lt $hi})
    $frameCount=S $g n;$candidate=S $g candidate;$seen=S $g checkedFrames
    $candidateRate=if($seen){[math]::Round($candidate*100/$seen,2)}else{'n/a'}
    $idleL=A @($c|Where-Object{$_.lit -ne 255}) lit
    $idleB=A @($c|Where-Object{$_.big -ne 255}) big
    $span=if([double]::IsInfinity($hi)){ 'end' }else{[math]::Round($hi,1)}
    Write-Output ('PHASE '+$a.ph+' t='+[math]::Round($lo,1)+'..'+$span+
        ' cpuN='+$c.Count+' glN='+$g.Count+' bN='+$b.Count+
        ' idleL='+$idleL+'% idleB='+$idleB+'% frame_events='+(S $c frames)+
        ' touches='+(S $c touches)+' BBcpu='+(A $b cpu)+'% BBcsw5s='+(A $b csw)+
        ' GLframes='+$frameCount+' GLdraws='+(S $g draws)+
        ' check_same='+(S $g same)+' checked='+(S $g checks)+
        ' check_changed='+(S $g changed)+' skipped='+(S $g skipped)+
        ' shadow_candidates='+$candidate+'/'+$seen+'='+$candidateRate+'%'+
        ' thread_reads='+$q.Count+' thread_missing='+(S $q missing)+
        ' g3d_voltage='+(A $v target)+'mV g3d_fault_samples='+@($v|Where-Object{$_.fault -ne 0}).Count)
    if($t.Count){
        $tops=$t|Group-Object proc,thread|Sort-Object Count -Descending|Select-Object -First 6
        foreach($x in $tops){
            Write-Output ('  THREAD '+$x.Name+' sample_count='+$x.Count+
                ' cpu_if_mach_avg='+(A $x.Group cpuMach)+
                '% cpu_if_ns_avg='+(A $x.Group cpuNs)+'% [UNCALIBRATED]')
        }
    }
}
Write-Output 'STOP RULES: frame glitches, dropped UI input, high CPU overhead or failures => sysctl p4_mode=0 and p4_threads=0.'
