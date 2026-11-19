#requires -version 5.1
# S9 fast offline A/B analyzer. Read-only; all times refer to kernel uptime.
# Example: powershell -File tools/s9-glgpu-compare.ps1 -A putty-glgpu-A598.log -B putty-glgpu-B455.log
param([Parameter(Mandatory=$true)][string]$A,[Parameter(Mandatory=$true)][string]$B)
$ErrorActionPreference='Stop'
function Read-Run($file) {
    $r=[ordered]@{file=$file;version='';floor='';brightness=-1;initialTemp=-1;initialBattery=-1;glReady=$false;gpuReady=$false;posterReady=$false;governor=@();cpu=@();gpu=@();fps=@();gl=@();mali=@();back=@()}
    $gov=[Collections.Generic.List[object]]::new()
    $cpu=[Collections.Generic.List[object]]::new()
    $gpu=[Collections.Generic.List[object]]::new()
    $fps=[Collections.Generic.List[object]]::new()
    $gl=[Collections.Generic.List[object]]::new()
    $mali=[Collections.Generic.List[object]]::new()
    $back=[Collections.Generic.List[object]]::new()
    $last=-1
    foreach($s in [IO.File]::ReadAllLines((Resolve-Path -LiteralPath $file).Path)){
        if(!$r.version -and $s -match 'boot-args = .*?hakios_kver=([^ ]+)'){$r.version=$Matches[1]}
        if(!$r.floor -and $s -match 'boot-args = .*?mali_gov_frame=(\d+)'){$r.floor=$Matches[1]}
        if($r.brightness -lt 0 -and $s -match '^s6e3ha8: brightness (\d+) sent'){$r.brightness=[int]$Matches[1]}
        if($r.initialTemp -lt 0 -and $s -match '^tmu: 8 sensors read; hottest \w+ at ([\d.]+) C'){$r.initialTemp=[double]::Parse($Matches[1],[Globalization.CultureInfo]::InvariantCulture)}
        if($r.initialBattery -lt 0 -and $s -match '^tmu: 8 sensors read;.*?battery=(\d+)'){$r.initialBattery=[int]$Matches[1]}
        if($s -like '*hakios-gl-stage: enabled*'){$r.glReady=$true}
        if($s -like '*mali-probe: enabled*'){$r.gpuReady=$true}
        if($s -like '*poster_cost: enabled*'){$r.posterReady=$true}
        if($s -match '^cpufreq_gov: (\d+) s: lit .*? \| big .*? \| temp big ([\d.]+) lit ([\d.]+) C'){
            $last=[int]$Matches[1];$big=[double]::Parse($Matches[2],[Globalization.CultureInfo]::InvariantCulture);$lit=[double]::Parse($Matches[3],[Globalization.CultureInfo]::InvariantCulture);$touch=0
            if($s -match 'boost: (\d+) touch'){$touch=[int]$Matches[1]}
            $gov.Add([pscustomobject]@{t=$last;big=$big;lit=$lit;touch=$touch});continue
        }
        if($s -match '^cputop: (\d+) s: (\d+)% in all:'){$cpu.Add([pscustomobject]@{t=[int]$Matches[1];val=[int]$Matches[2]});continue}
        if($s -match '^fps: (\d+) refreshes missed while moving,.*?gpu: (\d+)% busy, .*?, (\d+) MHz .*?G3D (\d+) C'){
            $gpu.Add([pscustomobject]@{t=$last;miss=[int]$Matches[1];busy=[int]$Matches[2];mhz=[int]$Matches[3];g3d=[int]$Matches[4]});continue
        }
        if($s -match '^fps: (\d+) presented, (\d+) drawn, (\d+) flips, (\d+) lost, (\d+) held'){
            $fps.Add([pscustomobject]@{t=$last;shown=[int]$Matches[1]});continue
        }
        if($s -match '^hakios-gl-stage: up=([\d.]+)s frames=\d+\.\.\d+ n=(\d+) draw=(\d+)us present=(\d+)us sync=(\d+)us copy=(\d+)us flush=(\d+)us total=(\d+)us p95bin=(\d+) slow16=(\d+) slow33=(\d+) max=(\d+)us shown=(\d+)'){
            $gl.Add([pscustomobject]@{t=[double]::Parse($Matches[1],[Globalization.CultureInfo]::InvariantCulture);n=[int]$Matches[2];draw=[int]$Matches[3];present=[int]$Matches[4];sync=[int]$Matches[5];copy=[int]$Matches[6];total=[int]$Matches[8];slow16=[int]$Matches[10]});continue
        }
        if($s -match '^mali-probe: up=(\d+)ms windows=(\d+) busy=(\d+)% frame_windows=(\d+) floor_forced=(\d+) thermal_windows=(\d+) framefloor=(\d+)MHz'){
            $mali.Add([pscustomobject]@{t=[double]([long]$Matches[1])/1000;windows=[int]$Matches[2];busy=[int]$Matches[3];forced=[int]$Matches[5];thermal=[int]$Matches[6];floor=[int]$Matches[7]});continue
        }
        if($s -match '^poster_cost: \d+ s: backboardd\[(\d+)\] cpu=([\d.]+)% rss=(\d+)KiB .*? up=(\d+)s sample'){
            $back.Add([pscustomobject]@{t=[int]$Matches[4];cpu=[double]::Parse($Matches[2],[Globalization.CultureInfo]::InvariantCulture);rss=[int]$Matches[3]});continue
        }
    }
    $r.governor=$gov.ToArray();$r.cpu=$cpu.ToArray();$r.gpu=$gpu.ToArray();$r.fps=$fps.ToArray();$r.gl=$gl.ToArray();$r.mali=$mali.ToArray();$r.back=$back.ToArray()
    return [pscustomobject]$r
}
function Avg($a,$key){if(@($a).Count -eq 0){return $null};return [math]::Round(($a|Measure-Object -Property $key -Average).Average,2)}
function Sum($a,$key){if(@($a).Count -eq 0){return 0};return ($a|Measure-Object -Property $key -Sum).Sum}
function Window($d,$begin,$end) {
    $g=@($d.governor|Where-Object{$_.t -ge $begin -and $_.t -lt $end})
    $c=@($d.cpu|Where-Object{$_.t -ge $begin -and $_.t -lt $end})
    $f=@($d.gpu|Where-Object{$_.t -ge $begin -and $_.t -lt $end})
    $sh=@($d.fps|Where-Object{$_.t -ge $begin -and $_.t -lt $end})
    $gl=@($d.gl|Where-Object{$_.t -ge $begin -and $_.t -lt $end})
    $m=@($d.mali|Where-Object{$_.t -ge $begin -and $_.t -lt $end})
    $b=@($d.back|Where-Object{$_.t -ge $begin -and $_.t -lt $end})
    $n=Sum $gl n; $nw=Sum $m windows; $shown=Sum $sh shown
    $frameMs=if($n){[math]::Round((($gl|ForEach-Object{$_.n*$_.total}|Measure-Object -Sum).Sum)/$n/1000,2)}else{$null}
    $syncMs=if($n){[math]::Round((($gl|ForEach-Object{$_.n*$_.sync}|Measure-Object -Sum).Sum)/$n/1000,2)}else{$null}
    $floorPct=if($nw){[math]::Round(100*(Sum $m forced)/$nw,1)}else{$null}
    $thermalPct=if($nw){[math]::Round(100*(Sum $m thermal)/$nw,1)}else{$null}
    $mhz598=if($f.Count){[math]::Round(100*@($f|Where-Object{$_.mhz -eq 598}).Count/$f.Count,1)}else{$null}
    $mhz455=if($f.Count){[math]::Round(100*@($f|Where-Object{$_.mhz -eq 455}).Count/$f.Count,1)}else{$null}
    $miss=if($shown){[math]::Round((Sum $f miss)/$shown,3)}else{$null}
    return [pscustomobject]@{run=$d.file;window="$begin-$end";CPUpct=(Avg $c val);backboardCPU=(Avg $b cpu);BIG=(Avg $g big);G3D=(Avg $f g3d);GPUbusy=(Avg $f busy);GPU598pct=$mhz598;GPU455pct=$mhz455;floorForcedPct=$floorPct;thermalGovPct=$thermalPct;GLtotalMs=$frameMs;GLsyncMs=$syncMs;GLslow16=(Sum $gl slow16);GLframes=$n;missedPerShown=$miss;touches=(Sum $g touch);govSamples=$g.Count}
}
$runA=Read-Run $A;$runB=Read-Run $B
foreach($d in @($runA,$runB)){
    Write-Output ("LOG {0}: kernel={1}, floor={2}, brightness={3}, SoCstart={4}C, batteryStart={5}C, GL={6}, Mali={7}, PID={8}" -f $d.file,$d.version,$d.floor,$d.brightness,$d.initialTemp,$d.initialBattery,$d.glReady,$d.gpuReady,$d.posterReady)
    if(!$d.glReady -or !$d.gpuReady -or !$d.posterReady){Write-Warning ("PROBE MISSING in "+$d.file+" (old logs cannot validate new instrumentation)")}
}
if($runA.version -ne $runB.version){Write-Warning 'Kernel SHA differs: invalid A/B'}
if($runA.brightness -ne $runB.brightness){Write-Warning 'Brightness differs: invalid A/B'}
if([math]::Abs($runA.initialTemp-$runB.initialTemp) -gt 2){Write-Warning 'Initial SoC temperatures differ by >2C: thermal comparison inconclusive'}
foreach($w in @(@(120,270),@(270,330))){
    Write-Output ("UPTIME WINDOW "+$w[0]+"-"+$w[1])
    $x=Window $runA $w[0] $w[1];$y=Window $runB $w[0] $w[1]
    @($x,$y)|Format-List|Out-String -Width 200|Write-Output
    $max=[math]::Max($x.touches,$y.touches)
    if($max -gt 0 -and [math]::Abs($x.touches-$y.touches)/$max -gt 0.15){Write-Warning 'Touch workload differs >15%; do not claim causal FPS or heat gain'}
}
Write-Output 'NOTE: times follow kernel monotonic uptime; FPS missed refreshes / presented is NOT unique lost frames.'
