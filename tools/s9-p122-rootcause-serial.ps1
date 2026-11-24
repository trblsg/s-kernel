#requires -Version 5.1
<#
 P12.2 bounded root-cause profiler (paired new kernel+SYSTEM).
 No automatic touch, clock, PMIC, panel, CPU_OFF or FPS-policy changes.
 No forced screen OFF. Reuses P12.1 proven cool-start and hot cleanup.
#>
[CmdletBinding()]
param(
 [ValidatePattern('^COM[0-9]+$')][string]$Port='COM125',
 [ValidateRange(12,60)][int]$WindowSeconds=20,
 [ValidateRange(50,600)][int]$IdleWindowSeconds=120,
 [ValidateRange(10,100)][int]$OffStableSeconds=25,
 [switch]$MicroGesture,[switch]$IncludeIdle,[switch]$WfiProbe,[switch]$WakeCheck,
 [switch]$P123,[switch]$AdaptiveFloor,[switch]$ClockCrossover,[switch]$IdleOnly,[switch]$HashCrossover,
 [switch]$Execute,[switch]$LibraryOnly
)
Set-StrictMode -Version 2
$ErrorActionPreference='Stop'
$script:P122SavedExecute=$Execute
$script:P122SavedLibrary=$LibraryOnly
$script:P122SavedWfi=$WfiProbe
$script:P123SavedWake=$WakeCheck
$parent=Join-Path $PSScriptRoot 's9-p12-dual-serial.ps1'
if(-not(Test-Path -LiteralPath $parent)){throw 'Missing matched s9-p12-dual-serial.ps1'}
. $parent -LibraryOnly -Port $Port -ActiveWindowSeconds $WindowSeconds -IdleWindowSeconds $IdleWindowSeconds -OffStableSeconds $OffStableSeconds
$Execute=$script:P122SavedExecute
$LibraryOnly=$script:P122SavedLibrary
$WfiProbe=$script:P122SavedWfi
$WakeCheck=$script:P123SavedWake
$script:P122ModeDirty=$false
$script:P123HostEnabled=[bool]$P123
$script:P123ClockAllowed=[bool]($P123 -and $ClockCrossover)
$script:P123FloorDirty=$false
$script:P123SourceLines=New-Object 'System.Collections.Generic.List[string]'
$script:P123ClockInput=0L
$script:P123ClockAt=[datetime]::MinValue
$script:P123GlAt=[datetime]::MinValue
$script:P123CaAt=[datetime]::MinValue
$script:P123ExpectedInput=-1L
$script:P123AutoPhase=''
$script:P123AutoRearmed=$false
$script:P123AutoAborted=$false
$script:P123RearmUp=-1
$script:P123StartAt=[datetime]::MinValue
$script:P123HashDirty=$false
$script:P123HashApplied=-1
$script:P123HashFallback=$false
$script:P123HashWindows=0
$script:P123TouchDown=[datetime]::MinValue
$script:P123TouchUp=[datetime]::MinValue
$script:P123TouchCount=0
$script:P123FloorRows=New-Object 'System.Collections.Generic.List[string]'
$script:P123FloorBaseline=$null
if(($AdaptiveFloor -or $ClockCrossover -or $IdleOnly -or $HashCrossover) -and -not $P123){throw 'P12.3 options require -P123'}
if(@(@($AdaptiveFloor,$ClockCrossover,$IdleOnly,$HashCrossover) | Where-Object {$_}).Count -gt 1){throw 'Choose ONE of -AdaptiveFloor, -ClockCrossover, -HashCrossover or -IdleOnly'}
if(($AdaptiveFloor -or $ClockCrossover -or $IdleOnly -or $HashCrossover) -and $MicroGesture){throw 'Keep gesture separate from floor, Clock and idle tests'}
if($WakeCheck -and -not ($IncludeIdle -or $IdleOnly)){throw '-WakeCheck requires an idle scenario'}
if(($AdaptiveFloor -or $ClockCrossover -or $HashCrossover) -and ($IncludeIdle -or $WfiProbe)){throw 'Run floor/Clock A/B independently of WFI/idle measurements'}
$script:P122CurrentMode=0
$script:P122Await=-1
$script:P122Ack=$false
$script:P123Control0=$false
$script:P123DeferredMode0=$false
$script:P122HiOverlap=$false
$script:P122GlWindows=New-Object 'System.Collections.Generic.List[object]'
$script:P122Phases=New-Object 'System.Collections.Generic.List[object]'
$script:P122GpuSnapshots=New-Object 'System.Collections.Generic.List[string]'
$script:P122PreviousObserver=$script:LifeLineObserver
$script:LifeLineObserver={
 param([string]$Line,[datetime]$At)
 if($script:P122PreviousObserver){& $script:P122PreviousObserver $Line $At}
 if($script:P123HostEnabled -and $script:LifeSynced){
    if($Line -match '^p123-(clock-class|clock-images|clock-window|ca-window|gl-window|gl):|^p1231-(clock-meta|clock-view|hash|floor|control):|^hakios-gl-(path|cost-top|g21|p0):|^ufs: (nufs: summary|native: speed)|^cpufreq_gov: (stat|state ms)'){
        $script:P123SourceLines.Add(('{0:HH:mm:ss.fff}Z {1} {2}' -f $At,$script:P12Phase,$Line))
    }
    if($Line -match '^p123-gl-window: .*native_skipped=0 '){$script:P123GlAt=$At}
    if($Line -match '^p123-ca-window: '){$script:P123CaAt=$At}
    if($Line -match '^p123-clock-window: .*input_ticks=(\d+) home=([01])'){
        $script:P123ClockInput=[long]$Matches[1]
        $script:P123ClockAt=$At
    }
    if($Line -match '^p1231-control: requested=0 observed=1 ' -and $script:P122Await -eq 0){$script:P123Control0=$true}
    if($Line -match '^p1231-hash: applied=([012]) '){$script:P123HashApplied=[int]$Matches[1]}
    if($Line -match '^p1231-hash: window mode=([12]) fallback=([01])'){
        if([int]$Matches[1] -eq $script:P123HashApplied){$script:P123HashWindows++}
        if($Matches[2] -eq '1'){$script:P123HashFallback=$true}
    }
    if($Line -match '\[TouchEvents\] contact \d+ presence: (touching|none)'){
        if($Matches[1] -eq 'touching'){$script:P123TouchDown=$At;$script:P123TouchCount++}
        else{$script:P123TouchUp=$At}
        $script:P123SourceLines.Add(('{0:HH:mm:ss.fff}Z TOUCH {1}' -f $At,$Line))
    }
    if($Line -match '^hi: auto=(REARM|baseline|cut1|recover1|cut2|recover2|FINISHED|ABORT)(?: |$)'){
        $phase=$Matches[1]
        if($phase -eq 'REARM'){$script:P123AutoRearmed=$true;if($Line -match 'up=(\d+)s'){$script:P123RearmUp=[int]$Matches[1]}}
        if($phase -eq 'baseline' -and $script:P123AutoRearmed){$script:P123StartAt=$At}
        if($script:P123AutoRearmed){
            if($phase -eq 'ABORT'){$script:P123AutoAborted=$true}
            $script:P123AutoPhase=$phase
            if($ClockCrossover){$script:P12Phase='CLOCK_'+$phase.ToUpperInvariant()}
            $script:P123SourceLines.Add(('{0:HH:mm:ss.fff}Z CLOCK_CROSSOVER {1}' -f $At,$Line))
        }
    }
    if($Line -match '^panic\(|^Debugger called:|^Kernel panic|Mali.*(?:GPU fault|MMU fault)|^GPU fault'){
        $script:P123SourceLines.Add("FAULT $Line")
        throw "P12.3 kernel/GPU fault: $Line"
    }
 }
 if($Line -match '^p122-gl: mode=([012]) ack=kernel_sysctl'){
    $mode=[int]$Matches[1]
    if($mode -eq $script:P122Await){$script:P122Ack=$true}
    Life-Say "P12.2 backboardd GL PROFILE ACK=$mode"
 }
 if($script:P12HiStopConfirmed -and $script:P12Phase -match '^(ROOT_|MICRO_)' -and
    $Line -match '^hi: auto=(baseline|cut1|recover1|cut2|recover2)'){
    $script:P122HiOverlap=$true
 }
 if($script:LifeSynced -and $Line -match '^p122-gl-window: mode=(\d+) frames=(\d+) wall_mean_us=(\d+) wall_max_us=(\d+) p50bin=(\d+) p95bin=(\d+) p99bin=(\d+) cpu_samples=(\d+) cpu_sample_us=(\d+) wall_sample_us=(\d+) draws=(\d+) uploads=(\d+) calls=(\d+)'){
    $script:P122GlWindows.Add([pscustomobject]@{
      Phase=$script:P12Phase;Mode=[int]$Matches[1];Frames=[int]$Matches[2];
      MeanUs=[double]$Matches[3];MaxUs=[double]$Matches[4];
      P50Bin=[int]$Matches[5];P95Bin=[int]$Matches[6];P99Bin=[int]$Matches[7];
      CpuSamples=[int]$Matches[8];CpuUs=[double]$Matches[9];
      SampleWallUs=[double]$Matches[10];Draws=[double]$Matches[11];
      Uploads=[double]$Matches[12];Calls=[double]$Matches[13]
    })
 }
}
function P122-Switch([int]$Mode,[switch]$FirstMinimal){
 if($Mode -notin @(0,1,2)){throw 'P12.2 modes are 0/1/2 only'}
 $script:P12Phase="SWITCH_$Mode"
 $script:P122Await=$Mode
 $script:P122Ack=$false
 $script:P123Control0=$false
 if($Mode -ne 0){$script:P122ModeDirty=$true}
 $idleBefore=$null
 if($Mode -eq 0 -and $P123 -and $MicroGesture){$idleBefore=P123-FloorSnapshot 'MODE0_REQUEST'}
 [void](P12-Query 'p122_diag_mode' "$Mode")
 $script:P122CurrentMode=$Mode
 if($FirstMinimal -and $Mode -eq 0){return}
 $until=(Life-Now).AddSeconds(15)
 while((Life-Now) -lt $until -and -not $script:P122Ack){
    Life-Pump
    $now=Life-Now
    Life-CheckSafety $now
    P12-EarlyThermalGuard
    if($script:P123Strict){P123-Guard}
 }
 if(-not $script:P122Ack){
    if($null -ne $idleBefore -and $script:P123Control0){
        $idleAfter=P123-FloorSnapshot 'MODE0_DEFERRED_CHECK'
        if((P122-CountField $idleBefore 'presents') -eq (P122-CountField $idleAfter 'presents')){
            $script:P123DeferredMode0=$true
            Life-Say 'P12.3.1 MODE0_REQUESTED_RENDER_IDLE: kernel0 ACK, userland request observed, no new presents. Apply pending next frame, NOT applied ACK.'
            $script:P123SourceLines.Add('MODE0_REQUESTED_RENDER_IDLE: control ACK + no new presents, not applied ACK')
            return
        }
    }
    throw "P12.2 kernel mode $Mode accepted; no fresh userland applied ACK in 15s; comparison invalid (render idle/stall or mismatched SYSTEM)"
 }
 if($Mode -eq 0){$script:P122ModeDirty=$false}
}
function P122-Snapshot([string]$Label){
 $s=P12-Query 'p122_gpu_stats'
 if($s -notmatch '^version=[23] mode=[012] '){throw 'Wrong P12.2 kernel, missing GPU histogram'}
 $script:P122GpuSnapshots.Add("$Label $s")
 return $s
}
function P122-CountField([string]$Text,[string]$Name){
 $rx='(?:^|\s)'+[Regex]::Escape($Name)+'=(\d+)'
 $match=[Regex]::Match($Text,$rx)
 if(-not $match.Success){throw "P12.2 missing expected GPU field $Name"}
 return [long]$match.Groups[1].Value
}
function P122-Deltas([string]$Before,[string]$After){
 $result=@{}
 $fields=@('windows','busy_ns','elapsed_ns','frames','forced','level260_ns','level455_ns','level598_ns','other_ns','jobs','queue_us','run_us')
 if($Before -match '^version=3 ' -and $After -match '^version=3 '){$fields+='level572_ns'}
 elseif(($Before -match '^version=3 ') -ne ($After -match '^version=3 ')){throw 'GPU schema changed mid-phase'}
 foreach($key in $fields){
    $a=P122-CountField $Before $key
    $b=P122-CountField $After $key
    if($b -lt $a){throw "P12.2 GPU counter $key went backwards across one phase; reset/overflow invalid"}
    $result[$key]=$b-$a
 }
 $busyPct=0.0;$forcedPct=0.0;$level455Pct=0.0
 if($result['elapsed_ns'] -gt 0){
    $busyPct=[Math]::Round(100.0*$result['busy_ns']/$result['elapsed_ns'],1)
    $level455Pct=[Math]::Round(100.0*$result['level455_ns']/$result['elapsed_ns'],1)
 }
 if($result['windows'] -gt 0){
    $forcedPct=[Math]::Round(100.0*$result['forced']/$result['windows'],1)
 }
 $avgQueue=0.0;$avgRun=0.0
 if($result['jobs'] -gt 0){
    $avgQueue=[Math]::Round([double]$result['queue_us']/$result['jobs'],1)
    $avgRun=[Math]::Round([double]$result['run_us']/$result['jobs'],1)
 }
 return [pscustomobject]@{
    Windows=$result['windows'];FrameWindows=$result['frames'];Forced=$result['forced'];
    ForcedPct=$forcedPct;BusyPct=$busyPct;Residency455Pct=$level455Pct;
    Jobs=$result['jobs'];QueueAvgUs=$avgQueue;RunAvgUs=$avgRun
 }
}

function P122-Profile([int]$Mode,[string]$Name){
 P122-Switch $Mode -FirstMinimal:($Name -eq 'ROOT_MIN_A')
 $before=$script:P122GlWindows.Count
 $pre=P122-Snapshot ($Name+'_BEGIN')
 P12-Observe $Name $WindowSeconds "NO TOUCH, fixed HI2, profiler level $Mode"
 $post=P122-Snapshot ($Name+'_END')
 $gpu=P122-Deltas $pre $post
 if($script:P122HiOverlap){throw "HI2 rearmed during $Name, comparison invalid"}
 $windows=@($script:P122GlWindows | Select-Object -Skip $before | Where-Object {$_.Phase -eq $Name -and $_.Mode -eq $Mode})
 if($windows.Count -lt 1){throw "No complete 60-frame GL histogram in $Name; cannot infer latency"}
 $mean=($windows | Measure-Object -Property MeanUs -Average).Average
 $peak=($windows | Measure-Object -Property MaxUs -Maximum).Maximum
 $script:P122Phases.Add([pscustomobject]@{Name=$Name;Mode=$Mode;N=$windows.Count;Mean=[Math]::Round($mean);Max=$peak;Gpu=$gpu})
 Life-Say "P12.2 $Name full60=$($windows.Count) GL_wall_mean_us=$([Math]::Round($mean)) GPU_busy_pct=$($gpu.BusyPct) floor_forced_pct=$($gpu.ForcedPct) GPU_455_residency_pct=$($gpu.Residency455Pct) jobs=$($gpu.Jobs) queue_avg_us=$($gpu.QueueAvgUs) run_avg_us=$($gpu.RunAvgUs)"
}
function P122-Report([string]$Stem,[string]$Stop){
 $lines=New-Object 'System.Collections.Generic.List[string]'
 $lines.Add('S9 P12.2 ROOT CAUSE: CPU wall vs sampled thread CPU vs GPU job queue and DVFS (NOT GPU energy).')
 $lines.Add('Profiles 0=MINIMAL 1=GL frame stage 2=per-slot wall; no source fps/clock/voltage changes.')
 $lines.Add('Histogram bins: <=5,8,12,16.667,20,33.333,50ms, >50ms. p95bin is not exact p95.')
 $lines.Add("STOP=$Stop hi2_stop_ACK=$($script:P12HiStopConfirmed) hi2_overlap=$($script:P122HiOverlap)")
 foreach($p in $script:P122Phases){
    $g=$p.Gpu
    $lines.Add("PHASE $($p.Name) mode=$($p.Mode) full60_windows=$($p.N) mean_GL_wall_us=$($p.Mean) max_GL_wall_us=$($p.Max) GPU_windows=$($g.Windows) GPU_frame_windows=$($g.FrameWindows) floor_forced_windows=$($g.Forced) floor_forced_pct=$($g.ForcedPct) GPU_busy_pct=$($g.BusyPct) GPU_455_residency_pct=$($g.Residency455Pct) GPU_jobs=$($g.Jobs) GPU_queue_avg_us=$($g.QueueAvgUs) GPU_run_avg_us=$($g.RunAvgUs)")
 }
 $lines.Add('FULL 60-FRAME GL WINDOWS (thread CPU samples are best-effort)')
 foreach($w in $script:P122GlWindows){
    $lines.Add("GL phase=$($w.Phase) mode=$($w.Mode) n=$($w.Frames) mean_us=$($w.MeanUs) max_us=$($w.MaxUs) p50bin=$($w.P50Bin) p95bin=$($w.P95Bin) p99bin=$($w.P99Bin) cpu_n=$($w.CpuSamples) cpu_us=$($w.CpuUs) matched_wall_us=$($w.SampleWallUs) draws=$($w.Draws) uploads=$($w.Uploads) calls=$($w.Calls)")
 }
 $lines.Add('GPU CUMULATIVE SNAPSHOTS (compute deltas; schema2 mislabeled index2=572 as 598; schema3 uses exact MHz)')
 foreach($v in $script:P122GpuSnapshots){$lines.Add($v)}
 $lines.Add('No physical root cause declared without same workload/thermal and instrument overhead controls.')
 $path=$Stem+'.p122-report.txt'
 [IO.File]::WriteAllLines($path,$lines.ToArray(),[Text.UTF8Encoding]::new($false))
 Life-Say ('P12.2 REPORT '+$path)
 foreach($v in $lines){Write-Host $v}
}
# P12.3 uses the same live serial fence and thermal limits as P12.2.
# Do not compare modes when the scene changes or telemetry is stale.
function P123-Guard {
 $now=Life-Now
 Life-CheckSafety $now
 P12-EarlyThermalGuard
 if($script:LifeState -ne 'ON' -or $script:LifeScene -ne 'HOME'){
    throw 'P12.3 scene changed or is unverified; stop active Home experiment'
 }
 if(($now-$script:LifeLastCpuAt).TotalSeconds -gt 4 -or
    ($now-$script:LifeLastGpuAt).TotalSeconds -gt 8){
    throw 'P12.3 thermal telemetry stale; stop active experiment'
 }
 if($script:P123ExpectedInput -ge 0){
    if(($now-$script:P123ClockAt).TotalSeconds -gt 12){throw 'P12.3 Clock/source window stale'}
    if($script:P123ClockInput -ne $script:P123ExpectedInput){throw 'P12.3 user input during no-touch experiment'}
 }
}
function P123-FloorSnapshot([string]$Label){
 $s=P12-Query 'p123_floor_stats'
 if($s -notmatch '^version=1 active=[01] configured_floor=450 quiet=[01] display=[01] '){
    throw 'P12.3 paired kernel or baseline floor450 missing'
 }
 $script:P123FloorRows.Add("$Label $s")
 return $s
}
function P123-FloorProfile([string]$Name,[int]$Mode){
 $script:P12Phase=$Name
 P123-Guard
 $before=P123-FloorSnapshot ($Name+'_BEGIN')
 if($before -notmatch 'active=0 ' -or $before -notmatch 'quiet=1 display=1 '){
    throw 'P12.3 floor baseline or input/display proof missing (unlock, then no touch)'
 }
 try{
    if($Mode -eq 1){
        if((P122-CountField $before 'streak') -lt 6){throw 'P12.3 stable 80-150ms frame cadence not proven'}
        $script:P123FloorDirty=$true
        [void](P12-Query 'p123_floor_mode' '1')
    }
    $gpuA=P122-Snapshot ($Name+'_GPU_BEGIN')
    $glStart=$script:P122GlWindows.Count
    P12-Observe $Name ([Math]::Min($WindowSeconds,20)) 'NO TOUCH; adaptive GPU frame floor only'
    P123-Guard
    $after=P123-FloorSnapshot ($Name+'_END')
    if($Mode -eq 0 -and $after -notmatch 'active=0 '){throw 'P12.3 unexpected adaptive policy during baseline'}
    if($Mode -eq 1 -and ($after -notmatch 'active=1 ' -or
       (P122-CountField $after 'eligible') -le (P122-CountField $before 'eligible'))){
        throw 'P12.3 no actual eligible adaptive floor windows or trial expired; comparison invalid'
    }
    $gpuB=P122-Snapshot ($Name+'_GPU_END')
    $g=P122-Deltas $gpuA $gpuB
    $obs=$script:P12Phases[-1]
    if($obs.Name -ne $Name -or $obs.Fps -lt 5 -or $obs.Refresh -le 0){
        throw 'P12.3 floor comparison missing live presentation coverage'
    }
    $ratio=60.0*$obs.Presented/$obs.Refresh
    $gl=@($script:P122GlWindows | Select-Object -Skip $glStart |
        Where-Object {$_.Phase -eq $Name -and $_.Mode -eq 0})
    if($gl.Count -lt 1 -or $ratio -lt 7 -or $ratio -gt 13){
        throw 'P12.3 no stable Home 10fps baseline or GL windows; comparison invalid'
    }
    # First completed GL window may straddle a host phase boundary.
    # Retain the raw data; only compare p95 if a subsequent full window exists.
    $whole=@($gl | Select-Object -Skip 1)
    $p95=-1
    if($whole.Count){$p95=($whole | Measure-Object -Property P95Bin -Maximum).Maximum}
    if($Name -eq 'FLOOR_A0'){
        $script:P123FloorBaseline=[pscustomobject]@{Ratio=$ratio;Queue=$g.QueueAvgUs;P95=$p95}
    }else{
        $b=$script:P123FloorBaseline
        if($null -eq $b -or [Math]::Abs($ratio-$b.Ratio) -gt 0.2*$b.Ratio -or
           $g.QueueAvgUs -gt [Math]::Max(250,2*$b.Queue) -or
           ($p95 -ge 0 -and $b.P95 -ge 0 -and $p95 -gt $b.P95+1)){
            throw 'P12.3 floor A/B quality gate failed: cadence, GPU queue or GL p95 bucket changed'
        }
    }
    $script:P123FloorRows.Add("QUALITY $Name normalized_60Hz_present_rate=$([Math]::Round($ratio,2)) full60_windows=$($gl.Count) p95bin_excluding_first=$p95 (minus1=insufficient)")
    $script:P123FloorRows.Add("RESULT $Name mode=$Mode presents_delta=$((P122-CountField $after 'presents')-(P122-CountField $before 'presents')) eligible_delta=$((P122-CountField $after 'eligible')-(P122-CountField $before 'eligible')) bypass_delta=$((P122-CountField $after 'bypass')-(P122-CountField $before 'bypass')) busy_pct=$($g.BusyPct) residency455_pct=$($g.Residency455Pct) jobs=$($g.Jobs) queue_us=$($g.QueueAvgUs) run_us=$($g.RunAvgUs)")
 }finally{
    if($script:P123FloorDirty){
        [void](P12-Query 'p123_floor_mode' '0' -Cleanup)
        $script:P123FloorDirty=$false
        Life-Say 'P12.3 adaptive floor RESTORE0 ACK'
    }
 }
}
function P123-ClockTrial {
 $script:P123AutoRearmed=$false;$script:P123AutoAborted=$false;$script:P123AutoPhase='';$script:P123StartAt=[datetime]::MinValue;$script:P123RearmUp=-1
 $script:P12HiStopConfirmed=$false
 $script:P12Phase='CLOCK_CROSSOVER'
 $commandAt=Life-Now
 $readyUntil=$commandAt.AddSeconds(200)
 try{
    [void](P12-Query 'hi_cmd' '252')
    while($script:P123AutoPhase -ne 'FINISHED'){
        Life-Pump
        P123-Guard
        if($script:P123AutoAborted){throw 'P12.3 Clock crossover aborted; no retry'}
        $now=Life-Now
        if($script:P123StartAt -ne [datetime]::MinValue){
            if($now -ge $script:P123StartAt.AddSeconds(115)){throw 'Clock active phase timeout; incomplete protocol'}
        }else{
            if($script:P123RearmUp -ge 0){$readyUntil=$commandAt.AddSeconds([Math]::Min(200,[Math]::Max(20,180-$script:P123RearmUp+25)))}
            if($now -ge $readyUntil){throw 'Clock readiness timeout; no START'}
        }
        if($now -ge $commandAt.AddSeconds(315)){throw 'Clock overall deadline'}
    }
    if(-not $script:P123AutoRearmed -or $script:P123StartAt -eq [datetime]::MinValue){throw 'Clock completion without START'}
 }finally{
    $script:P12HiGateStopSeen=$false
    [void](P12-Query 'hi_cmd' '201' -Cleanup)
    $stopUntil=(Life-Now).AddSeconds(7)
    while((Life-Now) -lt $stopUntil -and -not $script:P12HiGateStopSeen){Life-Pump}
    if(-not $script:P12HiGateStopSeen){throw 'Clock STOP userland ACK missing'}
    $script:P12HiStopConfirmed=$true
 }
 Life-Say 'P12.3 Clock crossover FINISHED with original timer restored'
}
function P1231-ValidateIdle([string]$Before,[string]$After){
    $a=P122-CountField $Before 'unmapped';$b=P122-CountField $After 'unmapped'
    $rx=' c([0-7]):(\d+)/'
    $old=@([regex]::Matches($Before,$rx));$new=@([regex]::Matches($After,$rx))
    if($old.Count -ne 8 -or $new.Count -ne 8 -or $a -ne $b){throw "P12.3.1 WFI mapping invalid: unmapped $a -> $b"}
    $sum=0L
    for($i=0;$i -lt 8;$i++){
        if([int]$old[$i].Groups[1].Value -ne $i -or [int]$new[$i].Groups[1].Value -ne $i){throw 'WFI logical index order invalid'}
        $delta=[long]$new[$i].Groups[2].Value-[long]$old[$i].Groups[2].Value
        if($delta -lt 0){throw 'WFI idle counter went backwards'}
        $sum+=$delta
    }
    if($sum -le 0){throw 'WFI probe has no mapped idle entries; invalid measurement'}
    Life-Say "P12.3.1 WFI_MAPPING_VALID mapped_entry_delta=$sum unmapped_delta=0 (not physical residency)"
}
function P1231-MicroTrial {
    $script:P123TouchCount=0;$script:P123TouchDown=[datetime]::MinValue;$script:P123TouchUp=[datetime]::MinValue
    $script:P12Phase='MICRO_WAIT_TOUCH'
    [void](P122-Snapshot 'MICRO_BEFORE_WAIT')
    Life-Say 'ONE gentle swipe now; waiting for actual touch event (12s maximum)'
    $until=(Life-Now).AddSeconds(12)
    while($script:P123TouchCount -eq 0 -and (Life-Now) -lt $until){Life-Pump;P123-Guard}
    if($script:P123TouchCount -ne 1){throw 'MicroGesture: expected one actual contact'}
    $script:P12Phase='MICRO_TOUCH'
    $until=$script:P123TouchDown.AddSeconds(3)
    while($script:P123TouchUp -lt $script:P123TouchDown -and (Life-Now) -lt $until){Life-Pump;P123-Guard}
    if($script:P123TouchUp -lt $script:P123TouchDown -or $script:P123TouchCount -ne 1){throw 'MicroGesture: contact release not proven'}
    Life-Say "MicroGesture actual contact host_ms=$(($script:P123TouchUp-$script:P123TouchDown).TotalMilliseconds); snapshots include serial latency"
    [void](P122-Snapshot 'MICRO_RELEASE')
    P12-Observe 'MICRO_RECOVERY' 8 'STOP touching; recovery measured after actual contact release'
    if($script:P123TouchCount -ne 1){throw 'MicroGesture repeated contact; invalid comparison'}
    [void](P122-Snapshot 'MICRO_RECOVERY_END')
}
function P1231-HashTrial {
    P122-Switch 2
    foreach($item in @(@{Name='HASH_A0';Mode=1},@{Name='HASH_B';Mode=2},@{Name='HASH_A1';Mode=1})){
        $script:P12Phase=$item.Name
        P123-Guard
        $script:P123HashApplied=-1;$script:P123HashWindows=0;$script:P123HashFallback=$false
        $script:P123HashDirty=$true
        try{
            [void](P12-Query 'p1231_hash_mode' "$($item.Mode)")
            $until=(Life-Now).AddSeconds(5)
            while($script:P123HashApplied -ne $item.Mode -and (Life-Now) -lt $until){Life-Pump;P123-Guard}
            if($script:P123HashApplied -ne $item.Mode){throw 'Hash trial: no applied ACK'}
            [void](P122-Snapshot ($item.Name+'_BEGIN'))
            P12-Observe $item.Name ([Math]::Min($WindowSeconds,20)) 'No touch; reference vs fast same digest, first window may include migration'
            [void](P122-Snapshot ($item.Name+'_END'))
            if((P12-Query 'p1231_hash_mode') -ne "$($item.Mode)" -or $script:P123HashApplied -ne $item.Mode -or $script:P123HashWindows -lt 2 -or $script:P123HashFallback){throw 'Hash trial invalid: expired, fallback or insufficient complete windows'}
            Life-Say "Hash trial $($item.Name) complete windows=$script:P123HashWindows; compare hash bytes/time excluding initial migration"
        }finally{
            [void](P12-Query 'p1231_hash_mode' '0' -Cleanup)
            $script:P123HashDirty=$false
        }
    }
    P122-Switch 0
}
function P123-Report([string]$Stem,[string]$Stop){
 $lines=New-Object 'System.Collections.Generic.List[string]'
 $lines.Add("S9 P12.3.1 SOURCE AND ADAPTIVE FLOOR STOP=$Stop mode0_deferred=$($script:P123DeferredMode0)")
 $lines.Add('Same mach ticks across processes; windows only, NOT matched frame IDs. Shadow bind candidates do not authorize skipping.')
 $lines.Add('Adaptive trial is 30s max, lower-only frame floor; no GPU voltage, panel or CPU_OFF changes.')
 foreach($r in $script:P123FloorRows){$lines.Add($r)}
 foreach($r in $script:P123SourceLines){$lines.Add($r)}
 [IO.File]::WriteAllLines($Stem+'.p123-report.txt',$lines.ToArray(),[Text.UTF8Encoding]::new($false))
 Life-Say ('P12.3 REPORT '+$Stem+'.p123-report.txt')
}
function P122-Run {
 if(-not $Execute){
    Write-Host 'P12.2 PREVIEW: SAFE no-swipe Minimal -> Stage -> GL Detailed -> Minimal.'
    Write-Host 'Matched P12.2 BOOT+SYSTEM and p122=1 bootarg REQUIRED. No -LegacyFirmware.'
    Write-Host 'P12.1 cool-start, HI2 stop with SpringBoard gate ACK, early and hard thermal stops.'
    Write-Host 'Only writes bounded profiler sysctl p122_diag_mode=0/1/2, plus HI2 manual STOP, restore all to zero.'
    Write-Host 'Disable independent glcost/glstage/p0hash/p0verify glprof bootargs for causal comparison.'
    Write-Host 'Optional -MicroGesture: after fresh cool start ONE gentle swipe only for 4 seconds.'
    Write-Host 'Optional -IncludeIdle -WfiProbe: manual physical POWER OFF; CPU idle diagnostics.'
    if($P123){
        Write-Host 'P12.3 PREVIEW: matched BOOT+SYSTEM; source timestamps and shadow GL; no automatic suppression.'
        Write-Host 'Choose separate -AdaptiveFloor (A/B/A, 30s kernel expiry), -ClockCrossover, -HashCrossover (reference/fast/reference), or -IdleOnly -WfiProbe.'
        Write-Host 'Without those options, collect P12.3 profile data. -MicroGesture is a separate ONE-swipe scenario; optional -WakeCheck only with idle.'
    }
    return
 }
 if($env:OS -ne 'Windows_NT'){throw 'Windows serial runner only'}
 if([IO.Ports.SerialPort]::GetPortNames() -notcontains $Port){throw "Serial port $Port unavailable"}
 $prefix=if($P123){'putty-S9-p123-source-'}else{'putty-S9-p122-rootcause-'}
 $stem=Join-Path (Get-Location).Path ($prefix+(Get-Date -Format 'yyyyMMdd-HHmmss'))
 $raw=$stem+'.log'
 if(Test-Path -LiteralPath $raw){throw 'Refuse to overwrite existing raw log'}
 $errorText=$null
 try{
    $script:LifeSerial=[IO.Ports.SerialPort]::new($Port,115200,[IO.Ports.Parity]::None,8,[IO.Ports.StopBits]::One)
    $script:LifeSerial.DtrEnable=$true
    $script:LifeSerial.ReadTimeout=160
    $script:LifeSerial.WriteTimeout=4000
    $script:LifeSerial.ReadBufferSize=131072
    $script:LifeSerial.Open()
    $script:LifeRaw=[IO.FileStream]::new($raw,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read)
    Life-SyncReadOnly
    $cpu=P12-Query 'p12_cpu_stats'
    if($cpu -notmatch '^mode=[01] '){throw 'P12 CPU readback missing'}
    $script:P12PreflightVerified=$true
    if($cpu -match '^mode=1 '){$script:P12CpuDirty=$true;[void](P12-Query 'p12_cpu_mode' '0' -Cleanup)}
    $wfi=P12-Query 'p12_wfi_stats'
    if($wfi -match '^enabled=1 '){$script:P12WfiDirty=$true;[void](P12-Query 'p12_wfi_probe' '0' -Cleanup)}
    [void](P12-Query 'p12_idle_diag')
    [void](P12-Query 'p12_gpu_boot_stats')
    $old=P12-Query 'p122_diag_mode'
    if($old -notin @('0','1','2')){throw 'P12.2 mode value invalid'}
    $script:P122ModeDirty=$true
    [void](P12-Query 'p122_diag_mode' '0' -Cleanup)
    $schema=P122-Snapshot 'PREFLIGHT'
    if($P123){
        if($schema -notmatch '^version=3 '){throw 'P12.3.1 matched kernel required: GPU schema3 missing'}
        $hash=P12-Query 'p1231_hash_mode'
        if($hash -ne '0'){$script:P123HashDirty=$true;[void](P12-Query 'p1231_hash_mode' '0' -Cleanup);$script:P123HashDirty=$false}
        [void](P12-Query 'p1231_ufs_stats')
    }
    P12-CoolStart 12
    P12-QuiesceHI2
    if($P123){
        $floor=P123-FloorSnapshot 'PREFLIGHT'
        if($floor -notmatch 'active=0 '){
            $script:P123FloorDirty=$true
            [void](P12-Query 'p123_floor_mode' '0' -Cleanup)
            $script:P123FloorDirty=$false
        }
        if(-not $IdleOnly){
            P123-Guard
            if($script:P123ClockAt -eq [datetime]::MinValue){throw 'P12.3 SpringBoard source window missing; wrong SYSTEM'}
            if(-not $MicroGesture){$script:P123ExpectedInput=$script:P123ClockInput}
            $script:P123Strict=$true
        }
    }
    if($AdaptiveFloor){
        P123-FloorProfile 'FLOOR_A0' 0
        P123-FloorProfile 'FLOOR_B' 1
        P123-FloorProfile 'FLOOR_A1' 0
    }elseif($ClockCrossover){
        P123-ClockTrial
    }elseif($HashCrossover){
        P1231-HashTrial
    }elseif(-not $IdleOnly -and -not ($P123 -and $MicroGesture)){
        P122-Profile 0 'ROOT_MIN_A'
        P122-Profile 1 'ROOT_STAGE'
        P122-Profile 2 'ROOT_DETAIL'
        P122-Profile 0 'ROOT_MIN_B'
    }
    $script:P123ExpectedInput=-1L
    if($MicroGesture){
        P12-CoolStart 12
        P12-EarlyThermalGuard
        P122-Switch 2
        if($P123){P1231-MicroTrial}else{
            P12-Observe 'MICRO_TOUCH' 4 'ONE gentle swipe; no repeated gestures'
            [void](P122-Snapshot 'MICRO_TOUCH_END')
        }
        P122-Switch 0
    }
    $script:P123Strict=$false
    if($IncludeIdle -or $IdleOnly){
        if($WfiProbe){
            [void](P12-Query 'p12_wfi_probe' '1')
            if($P123){
                [void](P12-Query 'p12_wfi_stats')
                $idleBefore=P12-Query 'p12_idle_diag'
                P12-Observe 'WFI_ON' 12 'No touch; compare measured WFI before physical screen OFF'
                [void](P12-Query 'p12_wfi_stats')
                $idleOn=P12-Query 'p12_idle_diag'
                P1231-ValidateIdle $idleBefore $idleOn
            }
        }
        if($P123){
            $baseline=P12-Query 'g3d_lab_stats'
            $script:P12GpuOffBaseline=P122-CountField $baseline 'off_count'
            $script:P12GpuWakeBaseline=P122-CountField $baseline 'wake_count'
        }
        P12-ObserveIdle
        if($WfiProbe){[void](P12-Query 'p12_wfi_stats');$idleEnd=P12-Query 'p12_idle_diag';if($P123){P1231-ValidateIdle $idleOn $idleEnd}}
        [void](P12-Query 'g3d_lab_stats')
    }
    if($P123){[void](P12-Query 'p1231_ufs_stats')}
    if($P123 -and -not $IdleOnly -and -not $MicroGesture -and
        ($script:P123GlAt -eq [datetime]::MinValue -or $script:P123CaAt -eq [datetime]::MinValue)){
        throw 'P12.3 paired backboardd CA/GL evidence missing or native-bind skip was active; comparison invalid'
    }
    Life-Say 'P12.3.1 acquisition complete; kernel restore requested; any deferred render ACK explicitly reported'
 }catch{
    $errorText=$_.Exception.Message
    Life-Say ('P12.2 STOPPED '+$errorText)
 }finally{
    if($script:P12PreflightVerified -and $null -ne $script:LifeSerial -and $script:LifeSerial.IsOpen){
        if($script:P123HashDirty){
            try{[void](P12-Query 'p1231_hash_mode' '0' -Cleanup);$script:P123HashDirty=$false}catch{Life-Say ('Hash restore UNSURE; 30s deadline '+$_.Exception.Message)}
        }
        if($script:P123FloorDirty){
            try{[void](P12-Query 'p123_floor_mode' '0' -Cleanup);$script:P123FloorDirty=$false;Life-Say 'P12.3 floor RESTORE0 ACK'}
            catch{Life-Say ('P12.3 floor restore UNSURE; kernel deadline <=30s '+$_.Exception.Message)}
        }
        if($script:P122ModeDirty -or $script:P122CurrentMode -ne 0){
            try{[void](P12-Query 'p122_diag_mode' '0' -Cleanup);Life-Say 'P12.2 profiler mode0 RESTORE ACK'}
            catch{Life-Say ('P12.2 profiler RESTORE UNSURE '+$_.Exception.Message)}
        }
        if($script:P12CpuDirty){
            try{[void](P12-Query 'p12_cpu_mode' '0' -Cleanup)}catch{Life-Say ('CPU restore UNSURE '+$_.Exception.Message)}
        }
        if($script:P12WfiDirty){
            try{[void](P12-Query 'p12_wfi_probe' '0' -Cleanup)}catch{Life-Say ('WFI restore UNSURE '+$_.Exception.Message)}
        }
        if($script:P12HiDirty -and (-not $ClockCrossover -or $script:P12HiStopConfirmed)){
            try{[void](P12-Query 'hi_cmd' '0' -Cleanup)}catch{Life-Say ('HI2 mailbox restore UNSURE '+$_.Exception.Message)}
        }
    }
    if($null -ne $script:LifeRaw){try{$script:LifeRaw.Flush()}catch{};$script:LifeRaw.Dispose();$script:LifeRaw=$null}
    if($null -ne $script:LifeSerial){$script:LifeSerial.Dispose();$script:LifeSerial=$null}
    if(Test-Path -LiteralPath $raw){
        try{
            if($P123){P123-Report $stem $(if($errorText){$errorText}else{'COMPLETE'})}
            P122-Report $stem $(if($errorText){$errorText}else{'COMPLETE'})
            P12-Report $stem $(if($errorText){$errorText}else{'COMPLETE'})
        }catch{Life-Say ('P12.2 report failed, raw retained: '+$_.Exception.Message)}
        Write-Host ('RAW LOG '+$raw)
    }
 }
 if($errorText){throw ('P12.2 incomplete '+$errorText)}
}
if(-not $LibraryOnly){P122-Run}
