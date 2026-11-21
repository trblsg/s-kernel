#requires -Version 5.1
# P6 unified passive offline report. Never modifies the original serial log.
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$Log)
$ErrorActionPreference='Stop'
$lines=[IO.File]::ReadAllLines((Resolve-Path -LiteralPath $Log).Path)
$ph=@();$wake=@();$shadow=@();$proc=@();$churn=@();$missing=@()
$ca=@();$threads=@();$g3d=@();$gpu=@();$crashes=@()
$off=0;$on=0;$skipped=0
$prideSegv=@()
function Token([string]$s,[string]$key,[string]$default='0'){
    $m=[regex]::Match($s,'(?<![A-Za-z0-9_])'+[regex]::Escape($key)+'=([^\s]+)')
    if($m.Success){return $m.Groups[1].Value}
    return $default
}
function IntToken([string]$s,[string]$key,[string]$default='0'){
    $v=Token $s $key $default
    if($v -notmatch '^-?[0-9]+$'){throw "interleaved/non-numeric $key=$v"}
    return [long]::Parse($v,[Globalization.CultureInfo]::InvariantCulture)
}
function Avg($array,[string]$key) {
    $items=@($array)
    if($items.Count -eq 0){return 'n/a'}
    return [math]::Round(($items|Measure-Object -Property $key -Average).Average,2)
}
function Sum($array,[string]$key) {
    $items=@($array)
    if($items.Count -eq 0){return 0}
    return ($items|Measure-Object -Property $key -Sum).Sum
}
foreach($s in $lines){
    try{
        if($s -match '^p6: PHASE=(\d+) up=(\d+)ms'){
            $ph+= [pscustomobject]@{p=[int]$Matches[1];up=[long]$Matches[2]}
        } elseif($s -match '^p6-wake: up=(\d+)ms phase=(\d+)'){
            $wake+= [pscustomobject]@{
                p=[int]$Matches[2];up=[long]$Matches[1]
                win=(IntToken $s 'gov_loops');boost=(IntToken $s 'boost_requests')
                touch=(IntToken $s 'touches');frames=(IntToken $s 'presents')
                lit=(IntToken $s 'idle_lit');big=(IntToken $s 'idle_big')
                miss=(IntToken $s 'missing')
            }
        } elseif($s -match '^p6-shadow: up=(\d+)ms phase=(\d+)'){
            $shadow+= [pscustomobject]@{
                p=[int]$Matches[2];up=[long]$Matches[1]
                bigmc=(IntToken $s 'big_mc');gpumc=(IntToken $s 'gpu_mc')
                load=(IntToken $s 'm3_load');park=(IntToken $s 'park_candidate')
                bigstep=(IntToken $s 'big_step');gpustep=(IntToken $s 'gpu_step')
            }
        } elseif($s -match '^p6-proc: up=(\d+)s phase=(\d+)'){
            $proc+= [pscustomobject]@{
                p=[int]$Matches[2];up=[long]$Matches[1]
                name=(Token $s 'name' 'unknown');pid=(IntToken $s 'pid')
                cpu=[double]::Parse((Token $s 'cpu_pct'),[Globalization.CultureInfo]::InvariantCulture)
                csw=(IntToken $s 'csw');valid=(IntToken $s 'valid_delta')
            }
        } elseif($s -match '^p6-churn: up=(\d+)s phase=(\d+)'){
            $churn+= [pscustomobject]@{p=[int]$Matches[2];up=[long]$Matches[1]
                name=(Token $s 'name');old=(IntToken $s 'prev_pid')
                newid=(IntToken $s 'new_pid')}
        } elseif($s -match '^p6-missing: up=(\d+)s phase=(\d+)'){
            $missing+= [pscustomobject]@{p=[int]$Matches[2];name=(Token $s 'name')}
        } elseif($s -match '^p6-ca-gl: up=([0-9.]+)s phase=(\d+)'){
            $ca+= [pscustomobject]@{
                p=[int]$Matches[2]
                avg=(IntToken $s 'avg_wall_us');draws=(IntToken $s 'draws')
                target=(IntToken $s 'target_draws');uploads=(IntToken $s 'uploads')
                changed=(IntToken $s 'changed');unchecked=(IntToken $s 'unchecked')
                frames=(IntToken $s 'n')
            }
        } elseif($s -match '^p6-ca-thread: up=(\d+)s phase=(\d+)'){
            $threads+= [pscustomobject]@{p=[int]$Matches[2];pid=(IntToken $s 'pid')
                cpu=[double]::Parse((Token $s 'cpu_ns'),[Globalization.CultureInfo]::InvariantCulture)}
        } elseif($s -match '^g3d-lab: up=(\d+)ms phase=(\d+) mode=(\d+) domain='){
            $g3d+= [pscustomobject]@{
                p=[int]$Matches[2];mode=[int]$Matches[3]
                up=[long]$Matches[1];domain=(IntToken $s 'domain')
                off=(IntToken $s 'off_total_ms')
                offcnt=(IntToken $s 'off_count');wakes=(IntToken $s 'wake_count')
                fail=(IntToken $s 'wake_fail');fault=(IntToken $s 'fault')
            }
        } elseif($s -match '^p5-gpu: up=(\d+)ms phase=(\d+) mode=(\d+)'){
            $gpu+= [pscustomobject]@{p=[int]$Matches[2];up=[long]$Matches[1]
                domain=(IntToken $s 'domain');jobs=(IntToken $s 'completed')
                peak=(IntToken $s 'peak_jobs_1s');error=(IntToken $s 'errors')
                dropped=(IntToken $s 'dropped')}
        } elseif($s -match 'GenericARMPE: the display off'){
            $off++
        } elseif($s -match 'GenericARMPE: the display on'){
            $on++
        }
        # This is confirmed launchd-exit evidence, unlike a PID transition.
        if($s -match 'com\.apple\.PridePoster\.PridePosterExtension \[(\d+)\].*exited due to SIGSEGV'){
            $prideSegv += [int]$Matches[1]
        }
        if($s -match '(?i)(SIGSEGV|EXC_BAD_ACCESS|fatal signal|crash report)' -and
            $s -match '(?i)(PridePoster|Poster|backboardd|SpringBoard|chronod)'){
            $crashes+=$s.Substring(0,[Math]::Min(240,$s.Length))
        }
    } catch { $skipped++ }
}
Write-Output ('P6 UNIFIED REPORT: '+$Log)
Write-Output ("Input lines=$($lines.Count) phase markers=$($ph.Count) skipped-interleaved=$skipped display_OFF=$off display_ON=$on")
if($ph.Count -eq 0){Write-Warning 'No exact P6 phase markers: verify matching P6 kernel / serial ACK. Do not compare phases.'}
foreach($phase in ($ph|Select-Object -ExpandProperty p -Unique)){
    $w=@($wake|Where-Object p -eq $phase)
    $s=@($shadow|Where-Object p -eq $phase)
    $p=@($proc|Where-Object p -eq $phase)
    $c=@($ca|Where-Object p -eq $phase)
    $t=@($threads|Where-Object p -eq $phase)
    $g=@($g3d|Where-Object p -eq $phase)
    $j=@($gpu|Where-Object p -eq $phase)
    $x=@($churn|Where-Object p -eq $phase)
    Write-Output ''
    Write-Output ("PHASE=${phase}: wakeSamples=$($w.Count) shadowSamples=$($s.Count) procSamples=$($p.Count) caGroups=$($c.Count) caThreads=$($t.Count) g3dSamples=$($g.Count) gpuSamples=$($j.Count) pidChanges=$($x.Count)")
    if($w.Count){
        Write-Output ("  WAKE-PROXY: governorLoops=$(Sum $w 'win') boostReq=$(Sum $w 'boost') touch=$(Sum $w 'touch') presented=$(Sum $w 'frames') avgIdleLIT=$(Avg @($w|Where-Object lit -ne 255) 'lit')% avgIdleBIG=$(Avg @($w|Where-Object big -ne 255) 'big')% (NOT GIC IRQ, CPD, WFI)")
    }
    if($s.Count){
        Write-Output ("  SHADOW: bigPeakMC=$(($s|Measure-Object bigmc -Maximum).Maximum) gpuPeakMC=$(($s|Measure-Object gpumc -Maximum).Maximum) parkCandidateSamples=$(Sum $s 'park') bigStepMax=$(($s|Measure-Object bigstep -Maximum).Maximum) gpuStepMax=$(($s|Measure-Object gpustep -Maximum).Maximum) writes=0")
    }
    if($p.Count){
        $top=@($p|Group-Object name|ForEach-Object{[pscustomobject]@{name=$_.Name;csw=($_.Group|Measure-Object csw -Sum).Sum;cpu=[math]::Round(($_.Group|Measure-Object cpu -Average).Average,1)}}|Sort-Object csw -Descending|Select-Object -First 5)
        foreach($h in $top){Write-Output ("  PROC-PROXY $($h.name): cswDelta=$($h.csw) avgCPU=$($h.cpu)%")}
    }
    if($x.Count){foreach($r in $x){Write-Output ("  PID-CHURN $($r.name): $($r.old) -> $($r.newid) (NOT proven crash)")}}
    if($c.Count){Write-Output ("  CA GL: frames=$(Sum $c 'frames') meanFrameWallUS=$(Avg $c 'avg') draws=$(Sum $c 'draws') targetDraws=$(Sum $c 'target') uncheckedGroups=$(Sum $c 'unchecked') (NOT CA callback source)")}
    if($t.Count){Write-Output ("  CA thread: avgCPUSampled=$(Avg $t 'cpu')% source=libproc")}
    if($g.Count){
        $offSamples=@($g|Where-Object domain -eq 0).Count
        $faultSamples=@($g|Where-Object fault -ne 0).Count
        Write-Output ("  G3D: PMU_OFF=$offSamples/$($g.Count) latestOffMS=$($g[-1].off) count=$($g[-1].offcnt) wakes=$($g[-1].wakes) wakeFails=$($g[-1].fail) faultSamples=$faultSamples")
    }
    if($j.Count){Write-Output ("  GPU: completedJobs=$(Sum $j 'jobs') maxPeakJobs1s=$(($j|Measure-Object peak -Maximum).Maximum) jobErrors=$(Sum $j 'error') dropped=$(Sum $j 'dropped')")}
}
Write-Output ''
Write-Output ("CONFIRMED PridePoster SIGSEGV exits in launchd log=$($prideSegv.Count); pids=$($prideSegv -join ',')")
Write-Output ("CRASH-RELATED RAW LINES=$($crashes.Count) (other signal strings do NOT automatically prove process identity)")
foreach($line in ($crashes|Select-Object -First 15)){Write-Output ('  '+$line)}
Write-Output 'NOT MEASURED: physical watts/voltage, GIC IRQ/callout origin, actual CPU deep-state residency, final framebuffer pixel identity.'
Write-Output 'Compare A1/B1/B2/B3/B4/B5/B6/A2 under equivalent initial temperatures. No gate OFF result unless PMU domain=0; a frame gap alone is insufficient.'
