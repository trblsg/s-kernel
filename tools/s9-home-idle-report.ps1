# S9 Home Idle single-boot PuTTY log summarizer.
# Usage: powershell -NoProfile -File tools/s9-home-idle-report.ps1 -Log putty-hi.log
# Counts are serial-window correlations, not panel watts or proof of pixel identity.
param([Parameter(Mandatory=$true)][string]$Log)
$ErrorActionPreference='Stop'
if(!(Test-Path -LiteralPath $Log)){throw "Missing log: $Log"}
$runs=New-Object System.Collections.Generic.List[object]
$active=$null
$groupNames=@{}
$crashes=0
$gateArms=0
$hasConfirmedMarkers=[bool](Select-String -LiteralPath $Log -Pattern '^hi: mark=(main|secondary|settings)' -Quiet)
$hasUsbDenied=[bool](Select-String -LiteralPath $Log -Pattern '^usb: sysctl kern.hakios.hi_cmd=\d+: EPERM' -Quiet)
$unconfirmed=$hasUsbDenied -and -not $hasConfirmedMarkers
$lastRequestedStage=0
function New-Run([string]$stage) {
    return [pscustomobject]@{
        Phase=$stage; Hi=@(); Backboard=@(); SpringBoard=@(); GpuClock=@(); GpuBusy=@();
        Frames=0; SecondsWithFps=0; Snapshots=0; Commits=0; LastUp=0; FirstUp=0;
        LinkTicks=@{}; LinkCut=@{}; BatteryCurrent=@(); BatteryAvg=@()
    }
}
foreach($line in [IO.File]::ReadLines((Resolve-Path -LiteralPath $Log).Path)){
    if($line -match '^hi: link group=(\d+) target=(\S+) action=(\S+)'){
        $groupNames[[int]$Matches[1]]="$($Matches[2]).$($Matches[3])"
    }
    if($line -match '^hi: mark=(main|secondary|settings)'){
        $active=New-Run $Matches[1]
        $runs.Add($active)
        continue
    }
    # The first S9 run had a real USB EPERM because hi_cmd lacked CTLFLAG_KERN.
    # Recover REQUESTED scene boundaries as explicitly unverified intent; never
    # claim that the corresponding UI scene was actually visible.
    if($unconfirmed -and $line -match '^usb> sysctl kern.hakios.hi_cmd=([123])$'){
        $requested=[int]$Matches[1]
        if($requested -ne $lastRequestedStage){
            $lastRequestedStage=$requested
            $name=@('unused','main','secondary','settings')[$requested]
            $active=New-Run $name
            $runs.Add($active)
        }
        continue
    }
    if($unconfirmed -and $active -and $lastRequestedStage -eq 1 -and
       $line -match '^usb: sysctl kern.hakios.mali_gov_frame=(0|450): set'){
        $label=if($Matches[1] -eq '0'){'main-floor0'}else{'main-floor450'}
        if($active.Phase -ne $label){
            $active=New-Run $label
            $runs.Add($active)
        }
        continue
    }
    if($line -match 'PridePosterExtension .*exited due to SIGSEGV'){$crashes++}
    if($line -match '^hi: gate=armed'){$gateArms++}
    if($null -eq $active){continue}
    if($line -match '^hi: phase=\w+ up=(\d+)s .+commits_5s=(\d+)'){
        $up=[int]$Matches[1];$active.Commits += [int]$Matches[2]
        $active.Snapshots++
        if(!$active.FirstUp){$active.FirstUp=$up}
        $active.LastUp=$up
    }
    if($line -match '^hi: link=(\d+) target=\S+ action=\S+ ticks_5s=(\d+) forwarded=\d+ cut=(\d+)'){
        $id=[int]$Matches[1];$ticks=[int]$Matches[2];$cut=[int]$Matches[3]
        if(!$active.LinkTicks.ContainsKey($id)){$active.LinkTicks[$id]=0;$active.LinkCut[$id]=0}
        $active.LinkTicks[$id]+=$ticks
        $active.LinkCut[$id]+=$cut
    }
    if($line -match '^poster_cost: \d+ s: (backboardd|SpringBoard)\[\d+\] cpu=([\d.]+)%'){
        $v=[double]::Parse($Matches[2],[Globalization.CultureInfo]::InvariantCulture)
        if($Matches[1] -eq 'backboardd'){$active.Backboard+= $v}else{$active.SpringBoard += $v}
    }
    if($line -match '^mali-probe: up=\d+ms .+busy=(\d+)% .+clock=(\d+)MHz'){
        $active.GpuBusy+= [int]$Matches[1];$active.GpuClock+= [int]$Matches[2]
    }
    if($line -match '^fps: (\d+) presented'){
        $active.Frames+=[int]$Matches[1];$active.SecondsWithFps++
    } elseif($line -match '^fps: idle'){
        $active.SecondsWithFps++
    }
    if($line -match '^max77705: fg: t \d+ s, .+ current (-?\d+) mA, avg (-?\d+) mA'){
        $active.BatteryCurrent += [int]$Matches[1];$active.BatteryAvg += [int]$Matches[2]
    }
}
if(!$runs.Count){
    Write-Output "home-idle-report: NO hi: mark lines. Confirm new BOOT+SYSTEM, hi=1, and kern.hakios.hi_cmd=1/2/3."
    exit 2
}
function Avg($a){
    if(!$a.Count){return 'n/a'}
    return ('{0:0.0}' -f (($a|Measure-Object -Average).Average))
}
Write-Output "S9 HomeIdle A/B: $($runs.Count) serial windows; $crashes confirmed PridePoster SIGSEGV; $gateArms source-cut attempts"
if($unconfirmed){
    Write-Output 'WARNING: hi_cmd=EPERM; stages are USB-REQUESTED scenes only, NOT confirmed hi: mark. No source cut succeeded.'
}
Write-Output 'fps_window = frames reported / sampled window uptime; navigation transitions are included; idle FPS reports are sparse, not OLED scanout.'
$i=0
foreach($r in $runs){
    $i++
    $seconds=$r.SecondsWithFps
    $duration=$r.LastUp-$r.FirstUp
    $fps=if($duration -ge 10){'{0:0.00}' -f ($r.Frames/$duration)}
         elseif($seconds){'{0:0.00}' -f ($r.Frames/$seconds)}else{'n/a'}
    $commits=if($r.Snapshots){'{0:0.0}' -f ($r.Commits/($r.Snapshots*5.0))}else{'n/a'}
    Write-Output ('{0,2}. {1,-9} fps={2,-6} backboardd={3,-5}% SB={4,-5}% GPU={5,-6}MHz busy={6,-5}% CA commits/s={7,-6} fg_current={8}/{9}mA (device charging state NOT inferred)' -f
        $i,$r.Phase,$fps,(Avg $r.Backboard),(Avg $r.SpringBoard),(Avg $r.GpuClock),(Avg $r.GpuBusy),$commits,
        (Avg $r.BatteryCurrent),(Avg $r.BatteryAvg))
    $top=@($r.LinkTicks.GetEnumerator()|Sort-Object Value -Descending|Select-Object -First 5)
    foreach($g in $top){
        if($g.Value -le 0){continue}
        $who=if($groupNames.ContainsKey([int]$g.Key)){$groupNames[[int]$g.Key]}else{'unknown'}
        Write-Output ("     link {0}: {1} tick_count={2} cut={3}" -f $g.Key,$who,$g.Value,$r.LinkCut[$g.Key])
    }
}
Write-Output 'Reminder: primary/secondary only prove a page-scoped workload; a manual 30s cut is causal evidence, not a validated permanent fix.'
