#requires -Version 5.1
<#
  P12 integrated S9 thermal A/B and natural deep-idle PMU/PLL observer.
  Requires s9-screen-lifecycle-serial.ps1 in same Windows directory.
  No forced screen ON; no CPU_OFF/C2/CPD/SICD, panel DCS, or GPU policy writes.
#>
[CmdletBinding()]
param(
    [ValidatePattern('^COM[0-9]+$')][string]$Port='COM125',
    [ValidateRange(12,90)][int]$ActiveWindowSeconds=25,
    [ValidateRange(50,900)][int]$IdleWindowSeconds=120,
    [ValidateRange(10,100)][int]$OffStableSeconds=25,
    [ValidateRange(75,85)][int]$BigStopC=82,
    [ValidateRange(70,80)][int]$LittleStopC=75,
    [ValidateRange(70,85)][int]$G3DStopC=77,
    [ValidateRange(43,55)][int]$BatteryStopC=47,
    [switch]$WfiProbe,
    [switch]$Execute,
    [switch]$LibraryOnly
)
Set-StrictMode -Version 2
$ErrorActionPreference='Stop'
$script:P12ExecuteRequested=$Execute
$script:P12LibraryRequested=$LibraryOnly
$lib=Join-Path $PSScriptRoot 's9-screen-lifecycle-serial.ps1'
if(-not(Test-Path -LiteralPath $lib)){throw 'Missing P11 companion: s9-screen-lifecycle-serial.ps1'}
. $lib -LibraryOnly -Port $Port -OffStableSeconds $OffStableSeconds -BigStopC $BigStopC -LittleStopC $LittleStopC -G3DStopC $G3DStopC -BatteryStopC $BatteryStopC
# Dot-sourced P11 declares its own -Execute and -LibraryOnly params.
# Preserve the user's P12 flags before importing that library.
$Execute=$script:P12ExecuteRequested
$LibraryOnly=$script:P12LibraryRequested
$script:P12Command=''
$script:P12Echo=$false
$script:P12Reply=$null
$script:P12PreflightVerified=$false
$script:P12Phase='PREFLIGHT'
$script:P12Results=New-Object 'System.Collections.Generic.List[string]'
$script:P12Phases=New-Object 'System.Collections.Generic.List[object]'
$script:P12CA=@{}
$script:LifeLineObserver={
    param([string]$Line,[datetime]$At)
    if($script:P12Command -and $Line.Contains('usb> '+$script:P12Command)){
        $script:P12Echo=$true
    }
    if($script:P12Command -and $script:P12Echo -and $Line -match '^usb: kern\.hakios\.([a-zA-Z_0-9]+) = (.*)$'){
        $name=$Matches[1];$value=$Matches[2].Trim()
        $expected=$script:P12Command.Substring(('sysctl kern.hakios.').Length).Split('=')[0]
        if($name -eq $expected){$script:P12Reply=$value}
    }
    if($script:LifeSynced -and $Line -match '^p12-ca: forwarded=(\d+) mean_render_us=(\d+) max_render_us=(\d+)'){
        if(-not $script:P12CA.ContainsKey($script:P12Phase)){
            $script:P12CA[$script:P12Phase]=[pscustomobject]@{Samples=0;MeanSum=0.0;Max=0.0}
        }
        $b=$script:P12CA[$script:P12Phase]
        $b.Samples++;$b.MeanSum+=[double][int]$Matches[2]
        $b.Max=[Math]::Max($b.Max,[double][int]$Matches[3])
    }
}
function P12-Query([string]$Name,[string]$Value=''){
    $allowed=@('p12_cpu_mode','p12_cpu_stats','p12_wfi_probe','p12_wfi_stats',
        'p12_gpu_boot_stats','p9_auto_gpu','p9_panel_stats','g3d_lab_stats',
        'p7_cpu_stats','p4_stats')
    if($Name -notin $allowed){throw "P12 sysctl disallowed $Name"}
    if($Value -ne '' -and (($Name -notin @('p12_cpu_mode','p12_wfi_probe')) -or $Value -notin @('0','1'))){
        throw 'Only guarded CPU A/B and passive WFI profiler may be written'
    }
    $cmd='sysctl kern.hakios.'+$Name
    if($Value -ne ''){$cmd+='='+$Value}
    $script:P12Command=$cmd
    $script:P12Reply=$null
    $script:P12Echo=$false
    $bytes=[Text.Encoding]::ASCII.GetBytes($cmd+[char]13)
    $script:LifeSerial.Write($bytes,0,$bytes.Length)
    Life-Say ('P12 '+$(if($Value -eq ''){'READ '}else{'WRITE '})+$cmd)
    $deadline=(Life-Now).AddSeconds(12)
    while((Life-Now) -lt $deadline -and $null -eq $script:P12Reply){
        Life-Pump
        Life-CheckSafety (Life-Now)
    }
    if($null -eq $script:P12Reply){throw "Missing echo and sysctl readback $cmd"}
    $reply=$script:P12Reply
    $script:P12Results.Add(('{0:HH:mm:ss.fff}Z {1} {2}={3}' -f (Life-Now),$script:P12Phase,$Name,$reply))
    $script:P12Command=''
    $script:P12Echo=$false
    if($Value -ne '' -and $reply -ne $Value){throw "P12 readback mismatch for $Name, got $reply"}
    return $reply
}
function P12-Observe([string]$Phase,[int]$Seconds,[string]$Instruction){
    $script:P12Phase=$Phase
    $start=Life-Now
    $sampleStart=$script:LifeSamples.Count
    Life-Say "P12 $Phase $Seconds seconds: $Instruction"
    $until=$start.AddSeconds($Seconds)
    while((Life-Now) -lt $until){
        Life-Pump
        $now=Life-Now
        Life-CheckStable $now
        Life-CheckSafety $now
    }
    $sample=@($script:LifeSamples | Select-Object -Skip $sampleStart | Where-Object {$_.display -eq 'ON'})
    $cpu=@($sample | Where-Object {$_.type -eq 'CPU'})
    $frames=@($sample | Where-Object {$_.type -eq 'FRAME'})
    $miss=@($sample | Where-Object {$_.type -eq 'FRAME_MISS'})
    $big='n/a';$peak='n/a'
    if($cpu.Count){
        $temps=@($cpu | ForEach-Object {[double]::Parse($_.big_c,[Globalization.CultureInfo]::InvariantCulture)})
        $big=[Math]::Round(($temps | Measure-Object -Average).Average,1)
        $peak=($temps | Measure-Object -Maximum).Maximum
    }
    $presented=0;$refresh=0;$missed=0
    foreach($s in $frames){$presented += [int]$s.fps_presented;$refresh += [int]$s.fps_refresh}
    foreach($s in $miss){$missed += [int]$s.fps_missed}
    $stat=[pscustomobject]@{Name=$Phase;Window=$Seconds;Cpu=$cpu.Count;Fps=$frames.Count;BigMean=$big;BigPeak=$peak;Presented=$presented;Refresh=$refresh;Missed=$missed}
    $script:P12Phases.Add($stat)
    Life-Say ("P12 $Phase ON cpu_n=$($stat.Cpu) fps_n=$($stat.Fps) BIG_mean=$big BIG_peak=$peak frames=$presented/$refresh missed=$missed")
}
function P12-ObserveIdle{
    $script:P12Phase='NATURAL_OFF'
    Life-Say "P12 idle $IdleWindowSeconds seconds: STOP interacting; allow iOS natural auto screen-OFF, no forced lock or wake."
    $beg=Life-Now
    $lastProbe=$beg
    while(((Life-Now)-$beg).TotalSeconds -lt $IdleWindowSeconds){
        Life-Pump
        $now=Life-Now
        Life-CheckStable $now
        Life-CheckSafety $now
        if($script:LifeState -eq 'OFF' -and ($now-$script:LifeStateSince).TotalSeconds -ge $OffStableSeconds -and
           ($now-$lastProbe).TotalSeconds -ge 20){
            $lastProbe=$now
            [void](P12-Query 'g3d_lab_stats')
            [void](P12-Query 'p9_auto_gpu')
            if($WfiProbe){[void](P12-Query 'p12_wfi_stats')}
        }
    }
}
function P12-Report([string]$Stem,[string]$Reason){
    Life-Report $Stem (Life-Now) $Reason
    $out=New-Object 'System.Collections.Generic.List[string]'
    $out.Add('P12 integrated thermal/idle hardware A/B - DO NOT CLAIM POWER OR FPS IMPROVEMENT WITHOUT SAME-GESTURE COMPARISON')
    $out.Add('A0/B/A1 apply only CPU no-touch stable-frame adaptive mode. Manual swipe input always uses stock target.')
    $out.Add('CPU frame mode0/1 needs verified sysctl readback; WFI observation is NOT actual C2/CPD/SICD or system suspend.')
    $out.Add('GPU boot gate uses P9 controls unchanged, verified via independent g3d_lab_stats PMU/PLL and off_total_ms.')
    $out.Add('No CPU_OFF, no forced Lock Screen, no panel DCS sleep, no voltage commands.')
    $out.Add('STOP='+$Reason)
    foreach($p in $script:P12Phases){
        $out.Add("PHASE $($p.Name) ON_cpu_n=$($p.Cpu) ON_fps_n=$($p.Fps) BIG_mean=$($p.BigMean) BIG_peak=$($p.BigPeak) presented=$($p.Presented) refresh=$($p.Refresh) missed=$($p.Missed)")
        if($p.Cpu -lt 5 -or $p.Fps -lt 5){$out.Add("PHASE $($p.Name) insufficient ON coverage: invalid A/B comparison")}
    }
    foreach($name in @('A0','B','A1')){
        if($script:P12CA.ContainsKey($name)){
            $v=$script:P12CA[$name]
            $out.Add("CA $name batches=$($v.Samples) mean_render_us=$([Math]::Round($v.MeanSum/$v.Samples)) max_render_us=$($v.Max)")
        }
    }
    $out.Add('SYSCTL ACKS AND PMU STATS')
    foreach($s in $script:P12Results){$out.Add($s)}
    $path=$Stem+'.p12-report.txt'
    [IO.File]::WriteAllLines($path,$out.ToArray(),[Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllLines($Stem+'.events.txt',$script:LifeEvents.ToArray(),[Text.UTF8Encoding]::new($false))
    Life-Say ('P12 REPORT: '+$path)
    foreach($line in $out){Write-Host $line}
}
function P12-Run {
    if(-not $Execute){
        Write-Host 'P12 PREVIEW: one matched P12 kernel+SYSTEM, same-gesture A0/B/A1 CPU efficiency, then natural OFF PMU/PLL idle.'
        Write-Host "Active windows $ActiveWindowSeconds s each; OFF observation $IdleWindowSeconds s. NO forced screen ON."
        Write-Host 'Keep PuTTY CLOSED for exclusive COM. Use same real touch/swipe sequence in A0 B A1; then leave device idle.'
        Write-Host 'P12 writes ONLY kern.hakios.p12_cpu_mode=0/1 and optionally p12_wfi_probe=0/1.'
        Write-Host 'P12 NEVER writes GPU/panel/CPU_OFF/voltage policies or forces a wake.'
        Write-Host 'Files required together: s9-p12-dual-serial.ps1 and s9-screen-lifecycle-serial.ps1.'
        return
    }
    if($env:OS -ne 'Windows_NT'){throw 'Windows only'}
    if([IO.Ports.SerialPort]::GetPortNames() -notcontains $Port){throw "Port $Port absent"}
    $stem=Join-Path (Get-Location).Path ('putty-S9-p12-dual-'+(Get-Date -Format 'yyyyMMdd-HHmmss'))
    $raw=$stem+'.log'
    if(Test-Path $raw){throw 'Existing raw log not overwritten'}
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
        if((P12-Query 'p12_cpu_stats') -notmatch '^mode=[01] '){throw 'Wrong kernel: P12 sysctl not recognized'}
        $script:P12PreflightVerified=$true
        [void](P12-Query 'p12_gpu_boot_stats')
        [void](P12-Query 'p9_auto_gpu')
        [void](P12-Query 'g3d_lab_stats')
        [void](P12-Query 'p9_panel_stats')
        [void](P12-Query 'p12_wfi_stats')
        [void](P12-Query 'p7_cpu_stats')
        [void](P12-Query 'p12_cpu_mode' '0')
        P12-Observe 'A0' $ActiveWindowSeconds 'STOCK CPU target; touch/swipe naturally'
        [void](P12-Query 'p12_cpu_mode' '1')
        P12-Observe 'B' $ActiveWindowSeconds 'P12 adaptive 70% target ONLY on no-touch stable frame streak; swipe normally'
        [void](P12-Query 'p12_cpu_stats')
        [void](P12-Query 'p12_cpu_mode' '0')
        P12-Observe 'A1' $ActiveWindowSeconds 'RESTORED stock; same gesture'
        if($WfiProbe){[void](P12-Query 'p12_wfi_probe' '1')}
        P12-ObserveIdle
        if($WfiProbe){[void](P12-Query 'p12_wfi_stats')}
        [void](P12-Query 'g3d_lab_stats')
        [void](P12-Query 'p12_gpu_boot_stats')
        Life-Say 'P12 capture complete; restoring all modified CPU observational policies'
    }catch{
        $errorText=$_.Exception.Message
        Life-Say ('P12 STOPPED: '+$errorText)
    }finally{
        if($script:P12PreflightVerified -and $null -ne $script:LifeSerial -and $script:LifeSerial.IsOpen){
            try{[void](P12-Query 'p12_cpu_mode' '0')}catch{Life-Say ('P12 CPU restore not confirmed: '+$_.Exception.Message)}
            try{[void](P12-Query 'p12_wfi_probe' '0')}catch{Life-Say ('P12 WFI restore not confirmed: '+$_.Exception.Message)}
        }
        if($null -ne $script:LifeRaw){try{$script:LifeRaw.Flush()}catch{};$script:LifeRaw.Dispose();$script:LifeRaw=$null}
        if($null -ne $script:LifeSerial){$script:LifeSerial.Dispose();$script:LifeSerial=$null}
        if(Test-Path -LiteralPath $raw){
            try{P12-Report $stem $(if($errorText){$errorText}else{'COMPLETE'})}
            catch{Life-Say ('P12 report failed; raw retained: '+$_.Exception.Message)}
            Write-Host ('RAW LOG: '+$raw)
        }
    }
    if($errorText){throw ('P12 capture incomplete: '+$errorText)}
}
if(-not $LibraryOnly){P12-Run}
