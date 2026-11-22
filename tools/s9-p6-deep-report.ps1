#requires -Version 5.1
# P6.10--P6.12 offline attribution. Never touches a device or modifies raw log.
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$Log)
Set-StrictMode -Version 2
$ErrorActionPreference='Stop'
$lines=[IO.File]::ReadAllLines((Resolve-Path -LiteralPath $Log).Path)
$phase=-1;$ph=@();$gov=@();$gap=@();$hash=@();$surfaces=@();$eagl=@();$fps=@();$gpu=@();$thermal=@();$bb=@()
$bad=0;$lineNum=0
function Field([string]$s,[string]$k,[string]$fallback='0'){
    $m=[regex]::Match($s,'(?<![A-Za-z0-9_])'+[regex]::Escape($k)+'=([^\s]+)')
    if($m.Success){return $m.Groups[1].Value}
    return $fallback
}
function Number([string]$s,[string]$k){
    $v=Field $s $k
    if($v -notmatch '^\d+$'){throw "interleaved/missing $k : $v"}
    return [long]::Parse($v,[Globalization.CultureInfo]::InvariantCulture)
}
function Total($a,[string]$name){
    $v=@($a)
    if(!$v.Count){return [double]0}
    return [double](($v|Measure-Object -Property $name -Sum).Sum)
}
function Mean($a,[string]$name){
    $v=@($a)
    if(!$v.Count){return 'n/a'}
    return [math]::Round([double](($v|Measure-Object -Property $name -Average).Average),2)
}
function Rate([double]$num,[double]$den){
    if($den -le 0){return 'n/a'}
    return [math]::Round($num/$den,2)
}
foreach($s in $lines) {
    $lineNum++
    try {
        if($s -match '^p6: PHASE=(\d+) up=(\d+)ms'){
            $phase=[int]$Matches[1]
            $ph += [pscustomobject]@{p=$phase;up=[long]$Matches[2];line=$lineNum}
        }elseif($s -match '^p610-gov: up=(\d+)ms phase=(\d+)'){
            $gov += [pscustomobject]@{p=[int]$Matches[2];up=[long]$Matches[1]
                windows=(Number $s 'windows');span=(Number $s 'span_ms')
                sticky=(Number $s 'sticky');stickyMs=(Number $s 'sticky_ms')
                shadow=(Number $s 'shadow_static')
                moving=(Number $s 'moving');anim=(Number $s 'anim')
                stat=(Number $s 'static');fingers=(Number $s 'fingers')
                transitions=(Number $s 'transitions')
                gapMs=(Number $s 'last_gap_ms');ageMs=(Number $s 'last_age_ms')
                quietMs=(Number $s 'last_quiet_ms')
                invalid=(Number $s 'invalid_frame')
                ageMax=(Number $s 'max_age_ms')}
        }elseif($s -match '^p610-gap: up=(\d+)ms phase=(\d+)'){
            $gap += [pscustomobject]@{p=[int]$Matches[2];up=[long]$Matches[1]
                events=(Number $s 'frame_events');fast=(Number $s 'gap_le50')
                steady=(Number $s 'gap_51_150');slow=(Number $s 'gap_151_300')
                idle=(Number $s 'gap_gt300');ageFast=(Number $s 'age_le100')
                ageMid=(Number $s 'age_101_299');ageSlow=(Number $s 'age_ge300')}
        }elseif($s -match '^p611-hash: up=([\d.]+)s phase=(\d+)'){
            $hash += [pscustomobject]@{p=[int]$Matches[2];n=(Number $s 'frames')
                calls=(Number $s 'calls');checks=(Number $s 'checked')
                same=(Number $s 'checked_same');changed=(Number $s 'checked_changed')
                skipFrame=(Number $s 'skip_sameframe');skipBase=(Number $s 'skip_no_base')
                skipPlanar=(Number $s 'skip_planar');skipSeldom=(Number $s 'skip_seldom')
                samples=(Number $s 'samples');bytes=(Number $s 'sample_bytes')
                us=(Number $s 'sample_us');maxUs=(Number $s 'max_us')
                sampleChanged=(Number $s 'sampled_changed');overflow=(Number $s 'overflow')}
        }elseif($s -match '^p611-surface: up=([\d.]+)s phase=(\d+)'){
            $surfaces += [pscustomobject]@{p=[int]$Matches[2];id=(Number $s 'id')
                n=(Number $s 'n');bytes=(Number $s 'bytes');us=(Number $s 'us')
                maxUs=(Number $s 'max_us');same=(Number $s 'same')
                changed=(Number $s 'changed')}
        }elseif($s -match '^p612-eagl: up=([\d.]+)s phase=(\d+)'){
            $eagl += [pscustomobject]@{p=[int]$Matches[2]
                n=(Number $s 'frames');draws=(Number $s 'draws')
                single=(Number $s 'single');multi=(Number $s 'multi')
                unknown=(Number $s 'unknown');switched=(Number $s 'switched')
                same=(Number $s 'same');alternating=(Number $s 'alternating')
                a=(Number $s 'target_a');b=(Number $s 'target_b')
                third=(Number $s 'third_target');mirror=(Number $s 'mirrored')
                shared=(Number $s 'shared');copied=(Number $s 'copied')
                apiCalls=(Number $s 'api_calls');positive=(Number $s 'api_positive')
                zero=(Number $s 'api_zero');negative=(Number $s 'api_negative')}
        }elseif($s -match '^fps: (\d+) presented, (\d+) drawn, (\d+) flips, (\d+) lost, (\d+) held;'){
            $fps += [pscustomobject]@{p=$phase;present=[int]$Matches[1]
                drawn=[int]$Matches[2];flips=[int]$Matches[3]
                lost=[int]$Matches[4];held=[int]$Matches[5]}
        }elseif($s -match '^fps: .*gpu: (\d+)% busy,.* (\d+) MHz \(mali_gov\)'){
            $gpu += [pscustomobject]@{p=$phase;busy=[int]$Matches[1];mhz=[int]$Matches[2]}
        }elseif($s -match '^cpufreq_gov: \d+ s:.*?temp big ([\d.]+) lit ([\d.]+) C'){
            $thermal += [pscustomobject]@{p=$phase
                big=[double]::Parse($Matches[1],[Globalization.CultureInfo]::InvariantCulture)
                lit=[double]::Parse($Matches[2],[Globalization.CultureInfo]::InvariantCulture)}
        }elseif($s -match '^poster_cost: \d+ s: backboardd\[\d+\] cpu=([\d.]+)%'){
            $bb += [pscustomobject]@{p=$phase
                cpu=[double]::Parse($Matches[1],[Globalization.CultureInfo]::InvariantCulture)}
        }
    }catch{$bad++}
}
Write-Output ('P6.10-12 DEEP REPORT: '+$Log)
Write-Output ("lines=$($lines.Count) phase_markers=$($ph.Count) rejected_interleaved=$bad p610=$($gov.Count) p610gap=$($gap.Count) p611=$($hash.Count) p611surface=$($surfaces.Count) p612=$($eagl.Count)")
if(!$ph.Count){Write-Warning 'No exact P6 phase markers. Confirm new matching BOOT+SYSTEM and ACK.'}
foreach($p in @($ph|Select-Object -ExpandProperty p -Unique)){
    $g=@($gov|Where-Object p -eq $p)
    $b=@($gap|Where-Object p -eq $p)
    $h=@($hash|Where-Object p -eq $p)
    $s=@($surfaces|Where-Object p -eq $p)
    $e=@($eagl|Where-Object p -eq $p)
    $f=@($fps|Where-Object p -eq $p)
    $gpuP=@($gpu|Where-Object p -eq $p)
    $t=@($thermal|Where-Object p -eq $p)
    $bbP=@($bb|Where-Object p -eq $p)
    Write-Output ''
    Write-Output ("PHASE=$p gov=$($g.Count) gap=$($b.Count) hash=$($h.Count) surfaces=$($s.Count) eagl=$($e.Count) fps_seconds=$($f.Count)")
    if($f.Count){
        Write-Output ("  FB-FPS: presented=$(Total $f 'present') drawn=$(Total $f 'drawn') flips=$(Total $f 'flips') lost=$(Total $f 'lost') held=$(Total $f 'held') fps_mean=$(Mean $f 'present') flips_mean=$(Mean $f 'flips') driver_report_only_no_optical_panel_proof")
    }
    if($gpuP.Count){Write-Output ("  GPU: busy_avg=$(Mean $gpuP 'busy')% clock_hint_avg_mhz=$(Mean $gpuP 'mhz')")}
    if($t.Count){Write-Output ("  THERMAL: big_avg_C=$(Mean $t 'big') lit_avg_C=$(Mean $t 'lit') samples=$($t.Count)")}
    if($bbP.Count){Write-Output ("  USERLAND: backboardd_avg_pct_of_one_core=$(Mean $bbP 'cpu') samples=$($bbP.Count)")}
    if($g.Count){
        $win=Total $g 'windows'
        Write-Output ("  P6.10 GOVERNOR: windows=$win anim=$(Total $g 'anim') static=$(Total $g 'stat') moving=$(Total $g 'moving') fingers=$(Total $g 'fingers') sticky_windows=$(Total $g 'sticky') sticky_share_pct=$(Rate (100*(Total $g 'sticky')) $win) sticky_ms=$(Total $g 'stickyMs') hypothetical_static=$(Total $g 'shadow') transitions=$(Total $g 'transitions') SHADOW_ONLY")
        Write-Output ("  P6.10 AGES: last_gap_ms=$($g[-1].gapMs) last_age_ms=$($g[-1].ageMs) last_quiet_ms=$($g[-1].quietMs) invalid_frame_windows=$(Total $g 'invalid') max_age_ms=$(($g|Measure-Object ageMax -Maximum).Maximum)")
    }
    if($b.Count){
        Write-Output ("  P6.10 GAP: frames=$(Total $b 'events') le50ms=$(Total $b 'fast') 51to150ms=$(Total $b 'steady') 151to300ms=$(Total $b 'slow') gt300ms=$(Total $b 'idle') age_le100_windows=$(Total $b 'ageFast') age_101_299_windows=$(Total $b 'ageMid') age_ge300_windows=$(Total $b 'ageSlow')")
    }
    if($h.Count){
        $n=Total $h 'samples'
        Write-Output ("  P6.11 CHECKS: frames=$(Total $h 'n') calls=$(Total $h 'calls') checked=$(Total $h 'checks') same=$(Total $h 'same') changed=$(Total $h 'changed') skip_same_frame=$(Total $h 'skipFrame') skip_no_base=$(Total $h 'skipBase') skip_planar=$(Total $h 'skipPlanar') skip_seldom=$(Total $h 'skipSeldom')")
        Write-Output ("  P6.11 HASH: samples=$n sample_MB=$(Rate (Total $h 'bytes') 1048576) total_us=$(Total $h 'us') avg_us_per_sample=$(Rate (Total $h 'us') $n) max_us=$(($h|Measure-Object maxUs -Maximum).Maximum) sampled_changed=$(Total $h 'sampleChanged') overflow=$(Total $h 'overflow') no_extra_hashes=1")
        foreach($z in @($s|Group-Object id | Sort-Object {($_.Group|Measure-Object us -Sum).Sum} -Descending|Select-Object -First 6)){
            $arr=@($z.Group)
            Write-Output ("  P6.11 TOP-SURFACE: id=$($z.Name) samples=$(Total $arr 'n') sampled_MB=$(Rate (Total $arr 'bytes') 1048576) total_us=$(Total $arr 'us') max_us=$(($arr|Measure-Object maxUs -Maximum).Maximum) changed=$(Total $arr 'changed')")
        }
    }
    if($e.Count){
        Write-Output ("  P6.12 EAGL: frames=$(Total $e 'n') draws=$(Total $e 'draws') single_target=$(Total $e 'single') multi_target=$(Total $e 'multi') unknown_target=$(Total $e 'unknown') switches=$(Total $e 'switched') alternating=$(Total $e 'alternating') same=$(Total $e 'same') third_target=$(Total $e 'third')")
        $ids=@($e|ForEach-Object {@($_.a,$_.b)}|Where-Object {$_ -gt 0}|Select-Object -Unique)
        Write-Output ("  P6.12 EAGL-API: calls=$(Total $e 'apiCalls') positive=$(Total $e 'positive') zero=$(Total $e 'zero') negative=$(Total $e 'negative') mirror_frames=$(Total $e 'mirror') shared_frames=$(Total $e 'shared') copied_frames=$(Total $e 'copied') target_id_set=$($ids -join ',') EAGL_API_RETURN_NOT_PHYSICAL_PRESENT")
        if($f.Count){Write-Output ("  P6.12 CROSS-SOURCE: EAGL_frames=$(Total $e 'n') vs_FB_presented=$(Total $f 'present') FB_flips=$(Total $f 'flips') (phase_boundaries_may_lag_60_frames; not_1to1_API_equivalence)")}
    }
}
Write-Output ''
Write-Output 'PROOF LIMITS: P6.10 hypothetical governor state never applied. P6.11 reads existing hashes only with 1/16 timing. P6.12 FPS presented/flips are kernel-driver accounting, not optical panel verification. Temperature drift is not attributable to an observer without matched baselines.'
