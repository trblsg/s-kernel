# S9 P1/G1 read-only single-log report. PowerShell 5.1+.
# Run: powershell -File tools/s9-p1g1-report.ps1 -Log putty-P1G1-455.log -Baseline putty-B.log
param([Parameter(Mandatory=$true)][string]$Log,[string]$Baseline)
$ErrorActionPreference='Stop'
function ReadRun([string]$file) {
    $r=[ordered]@{file=$file;boot='';crash=@();p1=0;missing=0;layout=0;gl=@();slots=@();procs=@();cpu=@();g3d=@()}
    $gl=[Collections.Generic.List[object]]::new()
    $slots=[Collections.Generic.List[object]]::new()
    $procs=[Collections.Generic.List[object]]::new()
    $cpu=[Collections.Generic.List[object]]::new()
    $g3d=[Collections.Generic.List[object]]::new()
    $crash=[Collections.Generic.List[string]]::new()
    $last=0
    foreach($s in [IO.File]::ReadAllLines((Resolve-Path -LiteralPath $file).Path)){
        if(!$r.boot -and $s -match 'boot-args = (.*)'){$r.boot=$Matches[1]}
        if($s -match 'native-metal: P1 userDictionary SPI: created='){$r.p1++}
        if($s -match 'NBCommandBuffer userDictionary\]: unrecognized selector'){$r.missing++}
        if($s -match 'PosterBoard\[\d+:\d+\] native-metal: newTextureLayoutWithDescriptor:isHeapOrBufferBacked: is not implemented'){$r.layout++}
        if($s -match 'PridePoster\.PridePosterExtension \[(\d+)\].*exited due to SIGABRT'){$crash.Add($Matches[1])}
        if($s -match '^cpufreq_gov: (\d+) s:'){$last=[int]$Matches[1]+15}
        if($s -match '^cputop: (\d+) s: (\d+)% in all:'){$cpu.Add([pscustomobject]@{t=[int]$Matches[1]+15;val=[int]$Matches[2]})}
        if($s -match '^fps: \d+ refreshes missed while moving,.*gpu: (\d+)% busy, .*?, (\d+) MHz .*?G3D (\d+) C'){$g3d.Add([pscustomobject]@{t=$last;busy=[int]$Matches[1];mhz=[int]$Matches[2];temp=[int]$Matches[3]})}
        if($s -match '^poster_cost: \d+ s: ([A-Za-z0-9_-]+)\[(\d+)\] cpu=([\d.]+)% rss=(\d+)KiB .*? up=(\d+)s sample'){
            $procs.Add([pscustomobject]@{t=[int]$Matches[5];name=$Matches[1];cpu=[double]::Parse($Matches[3],[Globalization.CultureInfo]::InvariantCulture)});continue
        }
        if($s -match '^hakios-gl-cost: up=([\d.]+)s frames=\d+\.\.\d+ calls=(\d+) measured=(\d+)us (.*)$'){
            $d=[ordered]@{t=[double]::Parse($Matches[1],[Globalization.CultureInfo]::InvariantCulture);calls=[int]$Matches[2];us=[long]$Matches[3]}
            $detail=$Matches[4]
            foreach($k in @('draw','texture','uniform','shader','state','vertex','target','other')){
                $m=[regex]::Match($detail,($k+'=(\d+)/(\d+)'))
                $d[$k+'Us']=if($m.Success){[long]$m.Groups[1].Value}else{0}
                $d[$k+'Count']=if($m.Success){[int]$m.Groups[2].Value}else{0}
            }
            $gl.Add([pscustomobject]$d);continue
        }
        if($s -match '^hakios-gl-cost-top: up=([\d.]+)s rank=(\d+) slot=(\S+) name=(.+?) calls=(\d+) us=(\d+)'){
            $slots.Add([pscustomobject]@{t=[double]::Parse($Matches[1],[Globalization.CultureInfo]::InvariantCulture);name=$Matches[4];calls=[int]$Matches[5];us=[long]$Matches[6]})
        }
    }
    $r.crash=$crash.ToArray();$r.gl=$gl.ToArray();$r.slots=$slots.ToArray();$r.procs=$procs.ToArray();$r.cpu=$cpu.ToArray();$r.g3d=$g3d.ToArray()
    return [pscustomobject]$r
}
function avg($v,[string]$key){$a=@($v);if(!$a.Count){return 'NA'};return [math]::Round(($a|Measure-Object -Property $key -Average).Average,2)}
function sm($v,[string]$key){$a=@($v);if(!$a.Count){return 0};return ($a|Measure-Object -Property $key -Sum).Sum}
function Window($r,[int]$lo,[int]$hi){
    $g=@($r.gl|Where-Object{$_.t -ge $lo -and $_.t -lt $hi})
    $p=@($r.procs|Where-Object{$_.t -ge $lo -and $_.t -lt $hi -and $_.name -eq 'PosterBoard'})
    $b=@($r.procs|Where-Object{$_.t -ge $lo -and $_.t -lt $hi -and $_.name -eq 'backboardd'})
    $c=@($r.cpu|Where-Object{$_.t -ge $lo -and $_.t -lt $hi})
    $gpu=@($r.g3d|Where-Object{$_.t -ge $lo -and $_.t -lt $hi})
    $peak=if($c.Count){($c|Measure-Object val -Maximum).Maximum}else{'NA'}
    Write-Output ("   uptime={0}-{1}s cpuPeak={2}% PosterCPUavg={3}% backboardCPUavg={4}% GPUtemp={5}C busy={6}% G1batches={7}" -f $lo,$hi,$peak,(avg $p cpu),(avg $b cpu),(avg $gpu temp),(avg $gpu busy),$g.Count)
    if($g.Count){
        $parts=@()
        foreach($k in @('draw','texture','uniform','shader','state','vertex','target','other')){
            $parts+=("{0}={1}us/{2}" -f $k,(sm $g ($k+'Us')),(sm $g ($k+'Count')))
        }
        Write-Output ("      calls={0}, GLduration={1}us; {2}" -f (sm $g calls),(sm $g us),($parts -join ' '))
    }
}
$runs=@(ReadRun $Log)
if($Baseline){$runs+=ReadRun $Baseline}
foreach($r in $runs){
    $kver=if($r.boot -match 'hakios_kver=([^ ]+)'){$Matches[1]}else{'UNKNOWN'}
    $floor=if($r.boot -match 'mali_gov_frame=(\d+)'){$Matches[1]}else{'UNKNOWN'}
    Write-Output ("RUN {0}: kernel={1} floor={2}MHz P1markers={3} PrideSIGABRT={4} missingUserDictionary={5} missingTextureLayout={6} G1batches={7}" -f $r.file,$kver,$floor,$r.p1,$r.crash.Count,$r.missing,$r.layout,$r.gl.Count)
    if($r.file -eq $Log -and !$r.p1){Write-Warning 'No positive P1 marker: selector fix not proven exercised'}
    if($r.file -eq $Log -and !$r.gl.Count){Write-Warning 'No G1 data: check glcost=1 and GLES image'}
    foreach($w in @(@(120,270),@(295,340),@(360,600),@(600,900),@(960,1200),@(1200,1500))){Window $r $w[0] $w[1]}
    if($r.slots.Count){
        Write-Output '   HEAVIEST SLOTS (only those reaching top 3 of a 60-frame batch):'
        $r.slots|Group-Object name|ForEach-Object{[pscustomobject]@{name=$_.Name;us=($_.Group|Measure-Object us -Sum).Sum;calls=($_.Group|Measure-Object calls -Sum).Sum}}|Sort-Object us -Descending|Select-Object -First 8|Format-Table -AutoSize|Out-String -Width 150|Write-Output
    }
}
Write-Output 'Times: G1 and poster_cost absolute mach uptime; cputop/fps approximated governor t+15s. Baseline G1 absence is expected.'
