#requires -Version 5.1
<#
.SYNOPSIS
  S9 HI2: one command, one Windows COM capture, fully automated A/B + scenes.
.DESCRIPTION
  REPLACES PuTTY; close PuTTY first (Windows COM ports are exclusive).
  -Execute owns USB COM, writes every received byte to an unmodified .log,
  validates the kernel USB echo AND set (0) response AND SpringBoard
  acknowledgement, re-arms hi=2 if we connected after its boot-time A/B,
  waits for a complete 20/20/20/20/20 second Clock-only causal crossover,
  swipes to secondary Home and opens Settings via the already packaged
  entitled /redwood/bin/battery_test helper (hi_cmd=300), confirming the
  reported foreground app before marking Settings.
  On every failure, preserve the full log, print the failing condition and
  try to restore the callback/phase. Never report a timed-out stage as valid.
  Does NOT change voltage, governor, CPU idle, framebuffer, brightness,
  screen power state or unrelated boot arguments.

  Without -Execute it previews the sequence without opening any COM port.
  -ReplayLog checks the parser/state machine against a prerecorded fixture.
.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File .\s9-home-idle-serial-auto.ps1
.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File .\s9-home-idle-serial-auto.ps1 -Port COM125 -Execute
.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File .\s9-home-idle-serial-auto.ps1 -ReplayLog .\tests\s9-home-idle-auto-fixture.log
#>
[CmdletBinding()]
param(
    [ValidatePattern('^COM[0-9]+$')][string]$Port='COM125',
    [ValidateRange(1200,3000000)][int]$BaudRate=115200,
    [ValidateRange(3,45)][int]$AckTimeoutSeconds=24,
    [ValidateRange(30,600)][int]$ConnectTimeoutSeconds=180,
    [ValidateRange(90,900)][int]$AutoFinishTimeoutSeconds=480,
    [ValidateRange(20,300)][int]$SecondarySeconds=65,
    [ValidateRange(20,300)][int]$SettingsSeconds=65,
    [ValidateRange(43,55)][int]$BatteryStopC=47,
    [string]$LogPath='',
    [string]$ReplayLog='',
    [switch]$SkipSettings,
    [switch]$P7Unified,
    [switch]$ActiveG3DGate,
    [switch]$ActiveCpuPark,
    [switch]$DumpP7History,
    [switch]$Execute
)
Set-StrictMode -Version 2
$ErrorActionPreference='Stop'
$script:Serial=$null
$script:Raw=$null
$script:Buf=New-Object byte[] 16384
$script:Recent=''
$script:Partial=''
$script:LastFlush=[datetime]::UtcNow
$script:AutoStages=New-Object 'System.Collections.Generic.List[string]'
$script:AutoFinished=$false
$script:AutoStarted=$false
$script:AutoAbortCount=0
$script:BootProven=$false
$script:SceneSettings=$false
$script:SceneHome=$false
$script:DisplayOff=$false
$script:DisplayOn=$false
$script:ParkedCount=0
$script:UnparkedCount=0
$script:LatestClockTicks=0
$script:BatteryC=0.0
$script:BatteryStop=$false
$script:Events=New-Object 'System.Collections.Generic.List[string]'
$script:LogPathFinal=''
$script:Started=$false
$script:PreflightPassed=$false
$script:SafeStopNeeded=$false
$script:Completed=$false
$script:Allowed='^(?:sysctl kern\.hakios\.(?:hi_cmd(?:=(?:1|2|3|201|250|252|300|301))?|mali_gov_frame|hi_policy(?:=[01])?|p7_(?:cpu|gpu)_(?:mode=[01]|phase=\d{1,2}|slot=\d{1,2}|stats|record|park=[01])|g3d_lab_(?:gate=[013]|idle_ms=10000|rail_probe=[01]|stats))|button (?:home|lock)|swipe l 1 350 1000)$'
function Say([string]$Message){
    $record=('{0:yyyy-MM-ddTHH:mm:ss.fffZ} {1}' -f [datetime]::UtcNow,$Message)
    $script:Events.Add($record)
    Write-Host ('[{0:HH:mm:ss}] {1}' -f (Get-Date),$Message)
}
function Scan-Line([string]$Line){
    if($Line -match '^hi: enabled .+policy=autonomous_cross_over'){$script:BootProven=$true}
    if($Line -match '^hi: auto=REARM '){
        $script:AutoStages.Clear()
        $script:AutoFinished=$false
        $script:AutoStarted=$false
        $script:AutoAbortCount=0
        Say 'DEVICE confirmed clean auto REARM'
    }
    if($Line -match '^hi: auto=START '){
        $script:AutoStarted=$true
    }
    if($Line -match '^hi: auto=(baseline|cut1|recover1|cut2|recover2) '){
        $stage=$Matches[1]
        $script:AutoStages.Add($stage)
        Say ('DEVICE auto phase '+$stage)
    }
    if($Line -match '^hi: auto=FINISHED'){
        $script:AutoFinished=$true
        Say 'DEVICE auto A/B/A/B FINISHED, callback restored'
    }
    if($Line -match '^hi: auto=ABORT'){
        $script:AutoAbortCount++
        $script:AutoFinished=$false
        $script:AutoStarted=$false
        $script:AutoStages.Clear()
        Say ('DEVICE '+$Line.Substring(0,[Math]::Min(250,$Line.Length)))
    }
    if($Line -match '^hi: link=\d+ target=SBHClockApplicationIconImageView action=_timerFired: ticks_5s=(\d+)'){
        $script:LatestClockTicks=[int]$Matches[1]
    }
    if($Line -match '^(p7-cpu:|p7-gpu:|g3d-lab: FAULT|g3d-lab: gate timeout|hi: verified_policy=|hi: clock_proof)'){
        Say ('DEVICE '+$Line.Substring(0,[Math]::Min($Line.Length,250)))
    }
    if($Line -match '^GenericARMPE: the display off'){
        $script:DisplayOff=$true;$script:DisplayOn=$false
        Say 'DEVICE display request=OFF'
    }
    if($Line -match '^GenericARMPE: the display on'){
        $script:DisplayOn=$true;$script:DisplayOff=$false
        Say 'DEVICE display request=ON'
    }
    if($Line -match '^GenericARMPE: sleep_cores: cpu \d+ taken off'){
        $script:ParkedCount++
        Say ('DEVICE '+$Line.Substring(0,[Math]::Min(240,$Line.Length)))
    }
    if($Line -match '^GenericARMPE: sleep_cores: cpu \d+ started again'){
        $script:UnparkedCount++
        Say ('DEVICE '+$Line.Substring(0,[Math]::Min(240,$Line.Length)))
    }
    if($Line -match '^hi: scene home=(\d+) .+settings_confirmed=(\d+)'){
        $script:SceneHome=($Matches[1] -eq '1')
        $script:SceneSettings=($Matches[2] -eq '1')
    }
    if($Line -match '^tmu: .+\bbattery=(\d+)(?:\s|$)'){
        $script:BatteryC=[int]$Matches[1]
        if($script:BatteryC -ge $BatteryStopC){$script:BatteryStop=$true}
    }
    if($Line -match '^(usb>|usb: sysctl kern\.hakios\.hi_cmd|usb: swipe: done|usb: button: pressed|hi: ctl_selftest|hi: ctl=RECEIVED|hi: ping|hi: settings_open|hi: scene|hi: auto=WAIT|hi: auto=START)'){
        Say ('DEVICE '+$Line.Substring(0,[Math]::Min($Line.Length,275)))
    }
}
function Pump{
    if($null -eq $script:Serial){return}
    $n=0
    try{$n=$script:Serial.Read($script:Buf,0,$script:Buf.Length)}
    catch [System.TimeoutException]{return}
    if($n -le 0){return}
    $script:Raw.Write($script:Buf,0,$n)
    if(([datetime]::UtcNow-$script:LastFlush).TotalSeconds -ge 2){
        $script:Raw.Flush()
        $script:LastFlush=[datetime]::UtcNow
    }
    $chunk=[Text.Encoding]::ASCII.GetString($script:Buf,0,$n)
    $script:Recent+=$chunk
    if($script:Recent.Length -gt 110000){
        $script:Recent=$script:Recent.Substring($script:Recent.Length-85000)
    }
    $script:Partial+=$chunk
    $pos=$script:Partial.IndexOf([char]10)
    while($pos -ge 0){
        $line=$script:Partial.Substring(0,$pos).TrimEnd([char]13)
        $script:Partial=$script:Partial.Substring($pos+1)
        Scan-Line $line
        $pos=$script:Partial.IndexOf([char]10)
    }
    if($script:Partial.Length -gt 32768){$script:Partial=$script:Partial.Substring($script:Partial.Length-8192)}
}
function Check-Thermal{
    if($script:BatteryStop){
        throw "Battery reached $($script:BatteryC)C >= safety limit $BatteryStopC C; callback will be restored"
    }
}
function Wait-For([string]$Label,[int]$TimeoutSeconds,[scriptblock]$When,[switch]$Thermal){
    if(-not $Execute){Say "PREVIEW WAIT $Label (<=$TimeoutSeconds seconds)";return}
    $deadline=[datetime]::UtcNow.AddSeconds($TimeoutSeconds)
    while([datetime]::UtcNow -lt $deadline){
        Pump
        if(& $When){Say ('CONFIRMED '+$Label);return}
        if($Thermal){Check-Thermal}
    }
    throw "Timeout waiting for $Label ($TimeoutSeconds seconds). Last serial tail: "+$script:Recent.Substring([Math]::Max(0,$script:Recent.Length-420))
}
function Rest([string]$Label,[int]$Seconds){
    Say ("$Label : $Seconds seconds")
    if(-not $Execute){return}
    $deadline=[datetime]::UtcNow.AddSeconds($Seconds)
    while([datetime]::UtcNow -lt $deadline){Pump;Check-Thermal}
}
function Send-Checked([string]$Cmd,[string]$Additional='',[int]$ExtraAppWaitSeconds=0){
    if($Cmd -cnotmatch $script:Allowed){throw "Command outside conservative HI2 allowlist: $Cmd"}
    if(-not $Execute){Say "PREVIEW SEND $Cmd";return}
    # Never treat stale ACKs from preceding phases as proof of this command.
    $drain=[datetime]::UtcNow.AddMilliseconds(210)
    while([datetime]::UtcNow -lt $drain){Pump}
    $script:Recent=''
    $payload=[Text.Encoding]::ASCII.GetBytes($Cmd+"`r")
    $script:Serial.Write($payload,0,$payload.Length)
    Say ("TX $Cmd")
    $echo='usb> '+$Cmd
    if($Cmd -match '^sysctl (kern\.hakios\.[A-Za-z0-9_]+)=(\d+)$'){
        $ack='usb: '+$Cmd+': set (0)'
    }elseif($Cmd -match '^sysctl (kern\.hakios\.[A-Za-z0-9_]+)$'){
        $ack='usb: '+$Matches[1]+' = '
    }elseif($Cmd -eq 'button home' -or $Cmd -eq 'button lock'){
        $ack='usb: button: pressed and let go'
    }elseif($Cmd -eq 'swipe l 1 350 1000'){
        $ack='usb: swipe: done'
    }else{throw "Missing ACK parser for $Cmd"}
    $kernelDeadline=[datetime]::UtcNow.AddSeconds($AckTimeoutSeconds)
    $until=$kernelDeadline.AddSeconds([Math]::Max(0,$ExtraAppWaitSeconds))
    $gotEcho=$false;$gotAck=$false;$gotExtra=[string]::IsNullOrEmpty($Additional)
    while([datetime]::UtcNow -lt $until){
        Pump
        if($script:Recent.Contains($echo)){$gotEcho=$true}
        if($script:Recent.Contains($ack)){$gotAck=$true}
        if($Additional -and $script:Recent.Contains($Additional)){$gotExtra=$true}
        if($script:Recent -match 'not a command \(help\)|EPERM:|ENOENT:|EINVAL:|EBUSY:|usb: swipe: one is going on|usb: button: no thread'){
            throw ("USB rejected $Cmd; "+$script:Recent.Substring([Math]::Max(0,$script:Recent.Length-430)))
        }
        if([datetime]::UtcNow -ge $kernelDeadline -and (-not $gotEcho -or -not $gotAck)){
            throw "No kernel echo+ACK for $Cmd within $AckTimeoutSeconds seconds"
        }
        if($gotEcho -and $gotAck -and $gotExtra){
            Say ("ACK echo+kernel"+$(if($Additional){"+SpringBoard"}else{""})+": $Cmd")
            return
        }
    }
    throw "No exact echo/ACK for $Cmd (echo=$gotEcho ack=$gotAck SB=$gotExtra) within $AckTimeoutSeconds seconds"
}
function Check-Auto-Crossover{
    $want=@('baseline','cut1','recover1','cut2','recover2')
    if(-not $script:AutoFinished){throw 'Autonomous test never reached FINISHED'}
    $got=@($script:AutoStages.ToArray())
    if(($got -join ',') -ne ($want -join ',')){
        throw ('Incomplete auto phase order; expected '+($want -join ',')+' got '+($got -join ','))
    }
}
function Dump-P7-Rings{
    Say 'P7 OFFLINE HISTORY: retrieve up to 96 samples per ring, no CPU/GPU power writes'
    Send-Checked 'sysctl kern.hakios.p7_cpu_stats'
    Send-Checked 'sysctl kern.hakios.p7_gpu_stats'
    for($slot=0;$slot -lt 96;$slot++){
        Send-Checked ("sysctl kern.hakios.p7_cpu_slot="+$slot)
        Send-Checked 'sysctl kern.hakios.p7_cpu_record'
        Send-Checked ("sysctl kern.hakios.p7_gpu_slot="+$slot)
        Send-Checked 'sysctl kern.hakios.p7_gpu_record'
    }
    Say 'P7 all retained kernel RAM samples captured. Reconnect after battery-only idle.'
}
function Run-Scenario{
    Say 'S9 HI2 + P7: direct USB COM logger; no PuTTY, no GUI SendKeys'
    if($DumpP7History){
        Dump-P7-Rings
        $script:Completed=$true
        return
    }
    if(($ActiveG3DGate -or $ActiveCpuPark) -and -not $P7Unified){throw 'ActiveG3DGate and ActiveCpuPark require P7Unified'}
    if($P7Unified){
        Send-Checked 'sysctl kern.hakios.p7_cpu_mode=1' 'p7-cpu: mode=1'
        Send-Checked 'sysctl kern.hakios.p7_gpu_mode=1' 'p7-gpu: mode=1'
        Send-Checked 'sysctl kern.hakios.g3d_lab_rail_probe=1' 'g3d-lab: 30-second read-only PMIC sampler=1'
        Send-Checked 'sysctl kern.hakios.p7_cpu_phase=10'
        Send-Checked 'sysctl kern.hakios.p7_gpu_phase=10'
        Send-Checked 'sysctl kern.hakios.p7_cpu_stats'
        Send-Checked 'sysctl kern.hakios.p7_gpu_stats'
        Say 'P7 baseline CPU/GPU PMU history recording (kernel-ring; also captures USB disconnected epochs)'
    }
    Send-Checked 'sysctl kern.hakios.hi_cmd'
    Send-Checked 'sysctl kern.hakios.mali_gov_frame'
    # A previous PuTTY/manual run can leave the mailbox at 250. Force a
    # *different* acknowledged value first; also safely stops any old cut.
    Send-Checked 'sysctl kern.hakios.hi_cmd=201'
    # The USB gadget may enumerate before SpringBoard starts. A kernel ACK
    # must still happen promptly; only the userland ping can wait for boot.
    Send-Checked 'sysctl kern.hakios.hi_cmd=250' 'hi: ping mode=2' 180
    $script:PreflightPassed=$true
    Say 'PREFLIGHT PASS: USB sysctl kernel+SpringBoard mailbox roundtrip'
    # A Home press is used only to bring SpringBoard to the foreground.
    Send-Checked 'button home'
    Rest 'Home settle after injected HOME key' 5
    Send-Checked 'sysctl kern.hakios.hi_cmd=252' 'hi: auto=REARM'
    Say 'Waiting for the whole hardware-timed baseline/cut/recover/cut/recover crossover'
    Wait-For 'autonomous START (first confirmed 10Hz Clock on settled Home)' $AutoFinishTimeoutSeconds {
        $script:AutoStarted
    } -Thermal
    Wait-For 'autonomous FINISHED' 190 {
        $script:AutoFinished
    } -Thermal
    Check-Auto-Crossover
    Say 'COMPLETE: Clock causal A/B/A/B; all 5 phase markers confirmed'
    if($P7Unified){
        Send-Checked 'sysctl kern.hakios.p7_cpu_phase=11'
        Send-Checked 'sysctl kern.hakios.p7_gpu_phase=11'
        Send-Checked 'sysctl kern.hakios.hi_cmd=1' 'hi: mark=main'
        Send-Checked 'sysctl kern.hakios.hi_policy=1' 'hi: verified_policy=1'
        Rest 'Verified-offscreen-only Clock policy ON; UNKNOWN must fail open' 85
        Send-Checked 'sysctl kern.hakios.hi_policy=0' 'hi: verified_policy=0'
        Rest 'Clock policy restored: baseline contrast on main Home' 45
        Send-Checked 'sysctl kern.hakios.p7_cpu_phase=12'
        Send-Checked 'sysctl kern.hakios.p7_gpu_phase=12'
    }
    # Verify a physical one-stroke Home navigation completed before
    # assigning the secondary-stage label to any later serial metrics.
    Send-Checked 'swipe l 1 350 1000'
    Rest 'Secondary settle' 4
    Send-Checked 'sysctl kern.hakios.hi_cmd=2' 'hi: mark=secondary'
    Rest 'Secondary Home low-load comparison' $SecondarySeconds
    if($P7Unified){
        Send-Checked 'sysctl kern.hakios.p7_cpu_phase=20'
        Send-Checked 'sysctl kern.hakios.p7_gpu_phase=20'
        Send-Checked 'sysctl kern.hakios.g3d_lab_gate=1' 'g3d-lab: gate mode=1'
        Rest 'P7 G3D quiet shadow (NO power writes)' 65
        if($ActiveG3DGate){
            # Existing driver demands safe GPU/TZ, default 950mV, no DVS,
            # and makes a validated domain power cycle. Kernel P7 watchdog
            # restores gate=0 after 120s even if the host disappears.
            Send-Checked 'sysctl kern.hakios.g3d_lab_idle_ms=10000'
            Send-Checked 'sysctl kern.hakios.p7_gpu_phase=21'
            Send-Checked 'sysctl kern.hakios.p7_cpu_phase=21'
            Send-Checked 'sysctl kern.hakios.g3d_lab_gate=3' 'g3d-lab: gate mode=3'
            Rest 'P7 real G3D quiet power gating (bounded; no voltage changes)' 80
            Send-Checked 'sysctl kern.hakios.g3d_lab_gate=0' 'g3d-lab: gate mode=0'
            Send-Checked 'sysctl kern.hakios.g3d_lab_stats'
            Say 'G3D gate restored, read back off/wake/fault counters'
        }else{
            Send-Checked 'sysctl kern.hakios.g3d_lab_gate=0' 'g3d-lab: gate mode=0'
            Say 'P7 real G3D OFF skipped (safe default); opt-in with -ActiveG3DGate'
        }
        Send-Checked 'sysctl kern.hakios.p7_cpu_phase=22'
        Send-Checked 'sysctl kern.hakios.p7_gpu_phase=22'
    }
    if(-not $SkipSettings){
        # Existing entitled battery_test open URL implementation, invoked by
        # a separate subprocess from SpringBoard only after auto FINISHED.
        $script:SceneSettings=$false
        Send-Checked 'sysctl kern.hakios.hi_cmd=300' 'hi: settings_open request=1 route=service_then_helper'
        try{
            Wait-For 'Settings foreground via SpringBoardServices' 32 {
                $script:SceneSettings
            } -Thermal
        }catch{
            Check-Thermal
            Say ('Settings service did not confirm in 32s; automatic helper fallback: '+$_.Exception.Message)
            Send-Checked 'sysctl kern.hakios.hi_cmd=301' 'hi: settings_open request=1 route=helper'
            Wait-For 'Settings foreground via battery_test helper' 32 {
                $script:SceneSettings
            } -Thermal
        }
        Send-Checked 'sysctl kern.hakios.hi_cmd=3' 'hi: mark=settings'
        Rest 'Settings stable comparison' $SettingsSeconds
    }else{Say 'Settings intentionally skipped by -SkipSettings'}
    if($P7Unified -and $ActiveCpuPark){
        Say 'P7 CPU OFF baseline vs opt-in CPU_OFF; requires boot sleep_cores=1'
        Send-Checked 'sysctl kern.hakios.p7_cpu_phase=30'
        Send-Checked 'sysctl kern.hakios.p7_gpu_phase=30'
        $script:DisplayOff=$false
        Send-Checked 'button lock'
        Wait-For 'display OFF baseline request' 30 {$script:DisplayOff} -Thermal
        Rest 'P7 screen-OFF baseline, BIG still online (park=0)' 75
        $script:DisplayOn=$false
        Send-Checked 'button lock'
        Wait-For 'display ON from baseline' 45 {$script:DisplayOn} -Thermal
        Rest 'Screen wake stabilization' 12
        Send-Checked 'sysctl kern.hakios.p7_cpu_park=1' 'p7-cpu: park_requested=1'
        Send-Checked 'sysctl kern.hakios.p7_cpu_phase=31'
        Send-Checked 'sysctl kern.hakios.p7_gpu_phase=31'
        $script:DisplayOff=$false
        $script:ParkedCount=0
        Send-Checked 'button lock'
        Wait-For 'display OFF before CPU power down' 30 {$script:DisplayOff} -Thermal
        Wait-For 'BIG CPU_OFF confirmed (not scheduler-idle)' 45 {$script:ParkedCount -ge 1} -Thermal
        Rest 'P7 opt-in BIG CPU_OFF residency measurement' 75
        $script:DisplayOn=$false
        Send-Checked 'button lock'
        Wait-For 'display ON after BIG CPU_OFF' 50 {$script:DisplayOn} -Thermal
        Send-Checked 'sysctl kern.hakios.p7_cpu_park=0' 'p7-cpu: park_requested=0'
        Rest 'P7 BIG cores wake/restore and recovery window' 35
        Send-Checked 'sysctl kern.hakios.p7_cpu_phase=32'
        Send-Checked 'sysctl kern.hakios.p7_gpu_phase=32'
    }
    if($P7Unified){
        Send-Checked 'sysctl kern.hakios.p7_cpu_stats'
        Send-Checked 'sysctl kern.hakios.p7_gpu_stats'
        Send-Checked 'sysctl kern.hakios.p7_cpu_record'
        Send-Checked 'sysctl kern.hakios.p7_gpu_record'
    }
    $script:Completed=$true
}
function Invoke-Report{
    if(-not $script:LogPathFinal -or -not(Test-Path -LiteralPath $script:LogPathFinal)){return}
    $stem=([IO.Path]::ChangeExtension($script:LogPathFinal,$null)).TrimEnd('.')
    $events=$stem+'.events.txt'
    [IO.File]::WriteAllLines($events,$script:Events.ToArray(),[Text.UTF8Encoding]::new($false))
    Say ('HOST EVENTS: '+$events)
    $report=$stem+'.report.txt'
    $parser=Join-Path $PSScriptRoot 's9-home-idle-report.ps1'
    if(Test-Path $parser){
        try{
            $result=& $parser -Log $script:LogPathFinal 2>&1
            $result|Out-File -LiteralPath $report -Encoding UTF8
            Say ('AUTO REPORT: '+$report)
            foreach($line in @($result)){Write-Host $line}
        }catch{Say ('REPORT FAILED but RAW LOG preserved: '+$_.Exception.Message)}
    }
    if($P7Unified -or $DumpP7History){
        $p7report=$stem+'.p7-report.txt'
        $p7parser=Join-Path $PSScriptRoot 's9-p7-basepower-report.ps1'
        if(Test-Path $p7parser){
            try{
                $lines=& $p7parser -Log $script:LogPathFinal 2>&1
                $lines|Out-File -LiteralPath $p7report -Encoding UTF8
                Say ('P7 BASE POWER REPORT: '+$p7report)
                foreach($line in @($lines)){Write-Host $line}
            }catch{Say ('P7 report failed but RAW LOG preserved: '+$_.Exception.Message)}
        }
    }
    Say ('RAW LOG: '+$script:LogPathFinal)
}
if($ReplayLog){
    if(-not(Test-Path -LiteralPath $ReplayLog)){throw "No fixture: $ReplayLog"}
    foreach($line in [IO.File]::ReadLines((Resolve-Path -LiteralPath $ReplayLog).Path)){
        Scan-Line $line
    }
    Check-Auto-Crossover
    Say 'REPLAY SELFTEST PASS: correctly saw A/B/A/B and FINISHED, no COM port opened'
    return
}
Say ('S9 HI2 SERIAL AUTO: '+$(if($Execute){'EXECUTE'}else{'PREVIEW'}))
if(-not $Execute){
    Say 'Preview: close PuTTY then re-run with -Port COMxxx -Execute; it will log DIRECTLY over USB.'
    Say 'Preflight read sysctl+ping(250), Home, rearm(252), Clock A/B/A/B, verified Clock policy and P7 CPU/GPU sampled ring, secondary, G3D shadow, Settings.'
    if($ActiveCpuPark){Say 'EXPLICIT CPU_OFF A/B requested: requires sleep_cores=1 in verified bootargs, button lock, positive OFF/ON/CPU_OFF proof'}
    if($DumpP7History){Say 'OFFLINE DUMP mode: 96 CPU+GPU ring rows collected after USB reconnect (no device state changes)'}
    Say ('Safety: battery-stop '+$BatteryStopC+'C; P7Unified read-only by default; -ActiveG3DGate and -ActiveCpuPark are EXPLICIT optional power transitions; CPU park requires boot sleep_cores=1.')
    return
}
if($env:OS -ne 'Windows_NT'){throw 'The direct COM harness requires Windows PowerShell'}
$deadline=[datetime]::UtcNow.AddSeconds($ConnectTimeoutSeconds)
while([IO.Ports.SerialPort]::GetPortNames() -notcontains $Port){
    if([datetime]::UtcNow -ge $deadline){throw "COM device $Port not found within $ConnectTimeoutSeconds sec"}
    Start-Sleep -Milliseconds 700
}
# -Execute is the explicit opt-in. No extra prompt or manual typing.
# If PuTTY still owns the port, SerialPort.Open throws before changing anything.
Say ('Opening exclusive '+$Port+'. CLOSE PuTTY first; no manual commands will be required.')
if([string]::IsNullOrWhiteSpace($LogPath)){
    $LogPath='putty-S9-hi2-auto-'+(Get-Date -Format 'yyyyMMdd-HHmmss')+'.log'
}
$script:LogPathFinal=[IO.Path]::GetFullPath($LogPath)
if(Test-Path -LiteralPath $script:LogPathFinal){
    throw ('Refusing to overwrite existing raw log: '+$script:LogPathFinal)
}
$failed=$null
try{
    $script:Serial=[IO.Ports.SerialPort]::new($Port,$BaudRate,[IO.Ports.Parity]::None,8,[IO.Ports.StopBits]::One)
    $script:Serial.Handshake=[IO.Ports.Handshake]::None
    $script:Serial.DtrEnable=$true
    $script:Serial.RtsEnable=$false
    $script:Serial.ReadBufferSize=131072
    $script:Serial.ReadTimeout=180
    $script:Serial.WriteTimeout=4000
    $script:Serial.Open()
    $script:Raw=[IO.FileStream]::new($script:LogPathFinal,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read)
    $script:Started=$true
    Say ('COM OPENED '+$Port+' '+$BaudRate+'; raw log streaming')
    Run-Scenario
}catch{
    $failed=$_.Exception.Message
    Say ('STOPPED: '+$failed)
}finally{
    if($script:Started){
        try{
            if($P7Unified){
                Send-Checked 'sysctl kern.hakios.hi_policy=0'
                Send-Checked 'sysctl kern.hakios.p7_cpu_park=0'
                Send-Checked 'sysctl kern.hakios.g3d_lab_gate=0'
                Send-Checked 'sysctl kern.hakios.g3d_lab_rail_probe=0'
                Send-Checked 'sysctl kern.hakios.p7_cpu_mode=0'
                Send-Checked 'sysctl kern.hakios.p7_gpu_mode=0'
            }
            Send-Checked 'sysctl kern.hakios.hi_cmd=201' 'hi: gate=usb-stop'
        }catch{Say ('CLEANUP UNCONFIRMED; safe Clock forward + optional G3D watchdog still apply: '+$_.Exception.Message)}
    }
    if($null -ne $script:Raw){
        try{$script:Raw.Flush()}catch{}
        $script:Raw.Dispose();$script:Raw=$null
    }
    if($null -ne $script:Serial){$script:Serial.Dispose();$script:Serial=$null}
    Invoke-Report
}
if($null -ne $failed){throw "HI2 scenario incomplete. $failed . Raw log and report retained."}
if(-not $script:Completed){throw 'HI2 scenario did not complete all confirmed phases'}
Say 'HI2 FULL AUTOMATIC SESSION COMPLETE; send the .log and .report.txt for source attribution.'
