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
 [switch]$MicroGesture,[switch]$IncludeIdle,[switch]$WfiProbe,
 [switch]$Execute,[switch]$LibraryOnly
)
Set-StrictMode -Version 2
$ErrorActionPreference='Stop'
$script:P122SavedExecute=$Execute
$script:P122SavedLibrary=$LibraryOnly
$script:P122SavedWfi=$WfiProbe
$parent=Join-Path $PSScriptRoot 's9-p12-dual-serial.ps1'
if(-not(Test-Path -LiteralPath $parent)){throw 'Missing matched s9-p12-dual-serial.ps1'}
. $parent -LibraryOnly -Port $Port -ActiveWindowSeconds $WindowSeconds -IdleWindowSeconds $IdleWindowSeconds -OffStableSeconds $OffStableSeconds
$Execute=$script:P122SavedExecute
$LibraryOnly=$script:P122SavedLibrary
$WfiProbe=$script:P122SavedWfi
$script:P122ModeDirty=$false
$script:P122CurrentMode=0
$script:P122Await=-1
$script:P122Ack=$false
$script:P122HiOverlap=$false
$script:P122GlWindows=New-Object 'System.Collections.Generic.List[object]'
$script:P122Phases=New-Object 'System.Collections.Generic.List[object]'
$script:P122GpuSnapshots=New-Object 'System.Collections.Generic.List[string]'
$script:P122PreviousObserver=$script:LifeLineObserver
$script:LifeLineObserver={
 param([string]$Line,[datetime]$At)
 if($script:P122PreviousObserver){& $script:P122PreviousObserver $Line $At}
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
 if($Mode -ne 0){$script:P122ModeDirty=$true}
 [void](P12-Query 'p122_diag_mode' "$Mode")
 $script:P122CurrentMode=$Mode
 if($FirstMinimal -and $Mode -eq 0){return}
 $until=(Life-Now).AddSeconds(15)
 while((Life-Now) -lt $until -and -not $script:P122Ack){
    Life-Pump
    $now=Life-Now
    Life-CheckSafety $now
    P12-EarlyThermalGuard
 }
 if(-not $script:P122Ack){
    throw "P12.2 kernel mode $Mode acknowledged but backboardd has NO mode ACK within 15 seconds; wrong SYSTEM or missing p122=1, refuse comparison"
 }
 if($Mode -eq 0){$script:P122ModeDirty=$false}
}
function P122-Snapshot([string]$Label){
 $s=P12-Query 'p122_gpu_stats'
 if($s -notmatch '^version=2 mode=[012] '){throw 'Wrong P12.2 kernel, missing GPU histogram'}
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
 foreach($key in @('windows','busy_ns','elapsed_ns','frames','forced',
   'level260_ns','level455_ns','level598_ns','other_ns',
   'jobs','queue_us','run_us')){
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
 $lines.Add('GPU CUMULATIVE SNAPSHOTS (compute deltas; do not misread totals as per-window)')
 foreach($v in $script:P122GpuSnapshots){$lines.Add($v)}
 $lines.Add('No physical root cause declared without same workload/thermal and instrument overhead controls.')
 $path=$Stem+'.p122-report.txt'
 [IO.File]::WriteAllLines($path,$lines.ToArray(),[Text.UTF8Encoding]::new($false))
 Life-Say ('P12.2 REPORT '+$path)
 foreach($v in $lines){Write-Host $v}
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
    return
 }
 if($env:OS -ne 'Windows_NT'){throw 'Windows serial runner only'}
 if([IO.Ports.SerialPort]::GetPortNames() -notcontains $Port){throw "Serial port $Port unavailable"}
 $stem=Join-Path (Get-Location).Path ('putty-S9-p122-rootcause-'+(Get-Date -Format 'yyyyMMdd-HHmmss'))
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
    [void](P122-Snapshot 'PREFLIGHT')
    P12-CoolStart 12
    P12-QuiesceHI2
    P122-Profile 0 'ROOT_MIN_A'
    P122-Profile 1 'ROOT_STAGE'
    P122-Profile 2 'ROOT_DETAIL'
    P122-Profile 0 'ROOT_MIN_B'
    if($MicroGesture){
        P12-CoolStart 12
        P12-EarlyThermalGuard
        P122-Switch 2
        P12-Observe 'MICRO_TOUCH' 4 'ONE gentle swipe; no repeated gestures'
        [void](P122-Snapshot 'MICRO_TOUCH_END')
        P122-Switch 0
    }
    if($IncludeIdle){
        if($WfiProbe){[void](P12-Query 'p12_wfi_probe' '1')}
        P12-ObserveIdle
        if($WfiProbe){[void](P12-Query 'p12_wfi_stats');[void](P12-Query 'p12_idle_diag')}
        [void](P12-Query 'g3d_lab_stats')
    }
    Life-Say 'P12.2 acquisition complete; all diagnostics restored to minimal'
 }catch{
    $errorText=$_.Exception.Message
    Life-Say ('P12.2 STOPPED '+$errorText)
 }finally{
    if($script:P12PreflightVerified -and $null -ne $script:LifeSerial -and $script:LifeSerial.IsOpen){
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
        if($script:P12HiDirty){
            try{[void](P12-Query 'hi_cmd' '0' -Cleanup)}catch{Life-Say ('HI2 mailbox restore UNSURE '+$_.Exception.Message)}
        }
    }
    if($null -ne $script:LifeRaw){try{$script:LifeRaw.Flush()}catch{};$script:LifeRaw.Dispose();$script:LifeRaw=$null}
    if($null -ne $script:LifeSerial){$script:LifeSerial.Dispose();$script:LifeSerial=$null}
    if(Test-Path -LiteralPath $raw){
        try{
            P122-Report $stem $(if($errorText){$errorText}else{'COMPLETE'})
            P12-Report $stem $(if($errorText){$errorText}else{'COMPLETE'})
        }catch{Life-Say ('P12.2 report failed, raw retained: '+$_.Exception.Message)}
        Write-Host ('RAW LOG '+$raw)
    }
 }
 if($errorText){throw ('P12.2 incomplete '+$errorText)}
}
if(-not $LibraryOnly){P122-Run}
