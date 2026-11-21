#requires -Version 5.1
<#
.SYNOPSIS
  S9 P5 read-only diagnostic run over exclusive direct USB CDC-ACM (COM125).
.DESCRIPTION
  Use on the Windows PC connected to the S9. CLOSE PuTTY FIRST: Windows COM
  handles are exclusive. This program replaces PuTTY as the logger during
  the experiment, writing all received bytes directly to a unique log file.
  Default mode is a no-port dry-run. -Execute opens the serial device.
  It validates the exact loader echo AND "set (0)" before changing phase.
  The USB loader consumes CR/LF itself; no GUI SendKeys or double Enter.
.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File .\s9-p5-serial-auto.ps1
.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File .\s9-p5-serial-auto.ps1 -Port COM125 -Execute -PreflightOnly
.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File .\s9-p5-serial-auto.ps1 -Port COM125 -Execute
#>
[CmdletBinding()]
param(
    [ValidatePattern('^COM[0-9]+$')][string]$Port = 'COM125',
    [ValidateRange(1200,3000000)][int]$BaudRate = 115200,
    [ValidateRange(3,45)][int]$AckTimeoutSeconds = 18,
    [ValidateRange(1,3600)][int]$BaselineSeconds = 120,
    [ValidateRange(1,3600)][int]$PassiveSeconds = 180,
    [ValidateRange(1,3600)][int]$ReversalSeconds = 120,
    [ValidateRange(1,3600)][int]$BurstSeconds = 120,
    [ValidateRange(30,3600)][int]$ScreenOffSeconds = 110,
    [ValidateRange(1,3600)][int]$AfterWakeSeconds = 120,
    [ValidateRange(10,180)][int]$DisplayTimeoutSeconds = 60,
    [string]$LogPath = '',
    [switch]$SkipScreenCycle,
    [switch]$PreflightOnly,
    [switch]$Execute
)

Set-StrictMode -Version 2
$ErrorActionPreference = 'Stop'

# Deliberately no voltage, clock, gating, temperature or boot commands.
$script:Allowed = '^sysctl kern\.hakios\.(?:(?:p4_mode|p4_threads|p5_mode|p5_threads|p5_phase)=[0-9]{1,2}|p5_stats)$'
$script:Serial = $null
$script:Log = $null
$script:Buf = New-Object byte[] 16384
$script:Recent = ''
$script:PartialLine = ''
$script:CommandNumber = 0
$script:LastFlushUtc = [DateTime]::UtcNow

function Say([string]$Message) {
    Write-Host ('[{0:HH:mm:ss}] {1}' -f (Get-Date),$Message)
}

# The full binary stream is logged; decoded ASCII is only used to recognize
# short, known kernel responses. Limit live terminal output to useful lines.
function Pump-Serial {
    if ($null -eq $script:Serial) { return }
    try {
        $count = $script:Serial.Read($script:Buf,0,$script:Buf.Length)
    } catch [System.TimeoutException] {
        return
    }
    if ($count -le 0) { return }
    $script:Log.Write($script:Buf,0,$count)
    if (([DateTime]::UtcNow - $script:LastFlushUtc).TotalSeconds -ge 1) {
        $script:Log.Flush()
        $script:LastFlushUtc = [DateTime]::UtcNow
    }
    $chunk = [Text.Encoding]::ASCII.GetString($script:Buf,0,$count)
    $script:Recent += $chunk
    if ($script:Recent.Length -gt 32768) {
        $script:Recent = $script:Recent.Substring($script:Recent.Length - 32768)
    }
    $script:PartialLine += $chunk
    # Print only confirmed loader command/telemetry lines: the raw logfile
    # contains EVERYTHING, including FPS and all unrelated system output.
    $lf = $script:PartialLine.IndexOf([char]10)
    while ($lf -ge 0) {
        $line = $script:PartialLine.Substring(0,$lf).TrimEnd([char]13)
        $script:PartialLine = $script:PartialLine.Substring($lf+1)
        if ($line -match '^(?:usb>|usb: sysctl|usb: kern\.hakios\.p5_|p5: PHASE|p5: mode|p5-gpu:|p5-owner:|p5-gl:|p5-display-thread:|GenericARMPE: the display)') {
            Say ("DEVICE " + $line.Substring(0,[Math]::Min(320,$line.Length)))
        }
        $lf = $script:PartialLine.IndexOf([char]10)
    }
    if ($script:PartialLine.Length -gt 16384) {
        # Do not retain a malformed/no-newline infinite status stream.
        $script:PartialLine = $script:PartialLine.Substring($script:PartialLine.Length-4096)
    }
}

function Drain-For([int]$Milliseconds) {
    if (-not $Execute) { return }
    $stop = [DateTime]::UtcNow.AddMilliseconds($Milliseconds)
    while ([DateTime]::UtcNow -lt $stop) { Pump-Serial }
}

function Send-Checked([string]$Command) {
    if ($Command -cnotmatch $script:Allowed) {
        throw "Command is outside the safe P5 allowlist: $Command"
    }
    if (-not $Execute) {
        Say ("PREVIEW SEND/VERIFY: " + $Command)
        return
    }
    # Drain earlier data and clear the search window BEFORE the command, so
    # confirmations cannot match something from a prior phase.
    Drain-For 250
    $script:Recent = ''
    $script:CommandNumber++
    $bytes = [Text.Encoding]::ASCII.GetBytes($Command + "`r")
    $script:Serial.Write($bytes,0,$bytes.Length)
    Say ("TX #{0}: {1} [one USB bulk write, CR termination]" -f $script:CommandNumber,$Command)

    $wantedEcho = 'usb> ' + $Command
    $wantedAck = if ($Command -eq 'sysctl kern.hakios.p5_stats') {
        'usb: kern.hakios.p5_stats = '
    } else {
        'usb: ' + $Command + ': set (0)'
    }
    $seenEcho = $false
    $seenAck = $false
    $deadline = [DateTime]::UtcNow.AddSeconds($AckTimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        Pump-Serial
        if ($script:Recent.Contains($wantedEcho)) { $seenEcho = $true }
        if ($script:Recent.Contains($wantedAck)) { $seenAck = $true }
        if ($seenEcho -and $seenAck) {
            Say ("ACK #{0}: exact loader echo AND expected response" -f $script:CommandNumber)
            return
        }
        if ($script:Recent.Contains('not a command (help)')) {
            throw "USB loader rejected a command during TX #$($script:CommandNumber). Do not advance phases."
        }
        if ($script:Recent.Contains('ENOENT: no such name') -or
            $script:Recent.Contains('EINVAL: the value was refused') -or
            $script:Recent.Contains('EPERM: not marked CTLFLAG_KERN')) {
            throw "USB loader rejected the sysctl during TX #$($script:CommandNumber)."
        }
    }
    $tail = $script:Recent.Substring([Math]::Max(0,$script:Recent.Length-800))
    throw ("TX #{0} NO VERIFIED ACK after {1}s: echo={2}, response={3}. Tail: {4}" -f $script:CommandNumber,$AckTimeoutSeconds,$seenEcho,$seenAck,$tail)
}

function Wait-Logged([string]$Label,[int]$Seconds) {
    Say ("{0}: {1}s" -f $Label,$Seconds)
    if (-not $Execute) { return }
    $stop = [DateTime]::UtcNow.AddSeconds($Seconds)
    while ([DateTime]::UtcNow -lt $stop) { Pump-Serial }
}

function Wait-Display([string]$State) {
    if (-not $Execute) {
        Say ("PLEASE physically turn the S9 screen $State NOW (do not type into a terminal).")
        return
    }
    # Discard PREVIOUS events before instructing the operator, not afterward:
    # otherwise a quick power-button press may be discarded as stale input.
    Drain-For 300
    $script:Recent = ''
    Say ("PLEASE physically turn the S9 screen $State NOW (do not type into a terminal).")
    try { [Console]::Beep(920,250) } catch {}
    $expected = 'GenericARMPE: the display ' + $State
    $deadline = [DateTime]::UtcNow.AddSeconds($DisplayTimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        Pump-Serial
        if ($script:Recent.Contains($expected)) {
            Say ("CONFIRMED physical screen $State in USB log")
            return
        }
    }
    throw "Physical screen $State was NOT confirmed in USB log; experiment aborted."
}

function Do-SafeExit {
    Say 'SAFE EXIT: attempt to restore P5/P4 observers OFF (verify each acknowledgment)'
    if (-not $Execute -or $null -eq $script:Serial) { return }
    foreach ($cmd in @(
        'sysctl kern.hakios.p5_mode=0',
        'sysctl kern.hakios.p5_threads=0',
        'sysctl kern.hakios.p4_mode=0',
        'sysctl kern.hakios.p5_phase=7',
        'sysctl kern.hakios.p5_stats'
    )) {
        try { Send-Checked $cmd }
        catch { Write-Warning ('SAFE EXIT unconfirmed: '+$cmd+': '+$_.Exception.Message) }
    }
}

Say ("S9 P5 direct serial for $Port (mode="+$(if($Execute){'EXECUTE'}else{'PREVIEW ONLY'})+")")
Say 'Does NOT use PuTTY or the GUI keyboard. Only ONE process may own COM125.'
Say 'The USB gadget line parser accepts a single CR. Device acknowledgments are mandatory.'
Say 'Read-only lab; no voltage, clocks, GPU gating or frame skip.'

if (-not $Execute) {
    Say 'PREVIEW: CLOSE PuTTY before EXECUTE; this script will capture ALL serial output instead.'
    Send-Checked 'sysctl kern.hakios.p5_stats'
    Send-Checked 'sysctl kern.hakios.p5_phase=0'
    if (-not $PreflightOnly) {
        Send-Checked 'sysctl kern.hakios.p4_mode=0'
        Send-Checked 'sysctl kern.hakios.p4_threads=0'
        Send-Checked 'sysctl kern.hakios.p5_threads=0'
        Send-Checked 'sysctl kern.hakios.p5_mode=0'
        Send-Checked 'sysctl kern.hakios.p5_phase=1'
        Wait-Logged 'A1 screen ON baseline' $BaselineSeconds
        Send-Checked 'sysctl kern.hakios.p5_threads=1'
        Send-Checked 'sysctl kern.hakios.p5_mode=1'
        Send-Checked 'sysctl kern.hakios.p4_mode=1'
        Send-Checked 'sysctl kern.hakios.p5_phase=2'
        Send-Checked 'sysctl kern.hakios.p5_stats'
        Wait-Logged 'B1 passive P5' $PassiveSeconds
        Send-Checked 'sysctl kern.hakios.p5_mode=0'
        Send-Checked 'sysctl kern.hakios.p5_threads=0'
        Send-Checked 'sysctl kern.hakios.p4_mode=0'
        Send-Checked 'sysctl kern.hakios.p5_phase=3'
        Wait-Logged 'A2 reversal' $ReversalSeconds
        Send-Checked 'sysctl kern.hakios.p5_threads=1'
        Send-Checked 'sysctl kern.hakios.p5_mode=2'
        Send-Checked 'sysctl kern.hakios.p4_mode=1'
        Send-Checked 'sysctl kern.hakios.p5_phase=4'
        Wait-Logged 'B2 screen ON burst detail' $BurstSeconds
        if (-not $SkipScreenCycle) {
            Send-Checked 'sysctl kern.hakios.p5_phase=5'
            Wait-Display 'off'
            Wait-Logged 'C1 real physical screen OFF' $ScreenOffSeconds
            Wait-Display 'on'
            Send-Checked 'sysctl kern.hakios.p5_phase=6'
            Send-Checked 'sysctl kern.hakios.p5_stats'
            Wait-Logged 'C2 wake and settle' $AfterWakeSeconds
        }
        Say 'PREVIEW SAFE EXIT: p5_mode=0 p5_threads=0 p4_mode=0 p5_phase=7 p5_stats'
    }
    Say 'PREVIEW COMPLETE. No COM port opened; no data sent.'
    return
}

if ($env:OS -ne 'Windows_NT') { throw 'Run -Execute on Windows, with the S9 CDC-ACM device attached.' }
$installed = [IO.Ports.SerialPort]::GetPortNames()
if ($installed -notcontains $Port) {
    throw "$Port is not enumerated in Windows: $($installed -join ', '). Check the S9 cable and USB CDC-ACM driver."
}
$answer = Read-Host "CLOSE PuTTY, confirm Windows $Port is the S9 device, then type $Port"
if ($answer -cne $Port) { throw 'COM confirmation failed; no port was opened.' }

if ([string]::IsNullOrWhiteSpace($LogPath)) {
    $LogPath = 'putty-P5-display-wake-gpu-serial-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.log'
}
$LogPath = [IO.Path]::GetFullPath($LogPath)
$started = $false
$phaseStarted = $false
$finished = $false
try {
    $script:Serial = [IO.Ports.SerialPort]::new($Port,$BaudRate,[IO.Ports.Parity]::None,8,[IO.Ports.StopBits]::One)
    $script:Serial.Handshake = [IO.Ports.Handshake]::None
    $script:Serial.DtrEnable = $true
    $script:Serial.RtsEnable = $false
    $script:Serial.ReadTimeout = 250
    $script:Serial.WriteTimeout = 5000
    $script:Serial.ReadBufferSize = 131072
    # No wait for PuTTY or file logging: the script owns BOTH ends.
    try { $script:Serial.Open() }
    catch {
        throw "Cannot open $Port. CLOSE PuTTY and any serial monitor first; details: $($_.Exception.Message)"
    }
    $script:Log = [IO.FileStream]::new($LogPath,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read)
    Say ("OPEN $Port at $BaudRate 8N1, handshake=None, DTR=on. Logging raw bytes to: $LogPath")
    $started = $true
    Drain-For 1300

    # First run only two commands; they must BOTH be confirmed by the device.
    Send-Checked 'sysctl kern.hakios.p5_stats'
    Send-Checked 'sysctl kern.hakios.p5_phase=0'
    Say 'PREFLIGHT PASS: received exact sysctl echo and set (0). No missing letters.'
    if ($PreflightOnly) {
        $finished = $true
    } else {
        Say 'A1: baseline, screen ON and steady'
        Send-Checked 'sysctl kern.hakios.p4_mode=0'
        Send-Checked 'sysctl kern.hakios.p4_threads=0'
        Send-Checked 'sysctl kern.hakios.p5_threads=0'
        Send-Checked 'sysctl kern.hakios.p5_mode=0'
        Send-Checked 'sysctl kern.hakios.p5_phase=1'
        $phaseStarted = $true
        Wait-Logged 'A1 screen ON baseline' $BaselineSeconds

        Say 'B1: passive P5 + P4'
        Send-Checked 'sysctl kern.hakios.p5_threads=1'
        Send-Checked 'sysctl kern.hakios.p5_mode=1'
        Send-Checked 'sysctl kern.hakios.p4_mode=1'
        Send-Checked 'sysctl kern.hakios.p5_phase=2'
        Send-Checked 'sysctl kern.hakios.p5_stats'
        Wait-Logged 'B1 passive P5 (discard first 10s for comparison)' $PassiveSeconds

        Say 'A2: return to OFF diagnostic baseline'
        Send-Checked 'sysctl kern.hakios.p5_mode=0'
        Send-Checked 'sysctl kern.hakios.p5_threads=0'
        Send-Checked 'sysctl kern.hakios.p4_mode=0'
        Send-Checked 'sysctl kern.hakios.p5_phase=3'
        Wait-Logged 'A2 reversal' $ReversalSeconds

        Say 'B2: capture bounded wake burst'
        Send-Checked 'sysctl kern.hakios.p5_threads=1'
        Send-Checked 'sysctl kern.hakios.p5_mode=2'
        Send-Checked 'sysctl kern.hakios.p4_mode=1'
        Send-Checked 'sysctl kern.hakios.p5_phase=4'
        Wait-Logged 'B2 screen ON burst detail' $BurstSeconds

        if (-not $SkipScreenCycle) {
            Say 'C1: do not touch the screen after pressing physical power'
            Send-Checked 'sysctl kern.hakios.p5_phase=5'
            Wait-Display 'off'
            Wait-Logged 'C1 confirmed screen OFF' $ScreenOffSeconds
            Wait-Display 'on'
            Send-Checked 'sysctl kern.hakios.p5_phase=6'
            Send-Checked 'sysctl kern.hakios.p5_stats'
            Wait-Logged 'C2 confirmed screen ON after wake' $AfterWakeSeconds
        } else {
            Say 'Screen cycle explicitly skipped; cannot conclude anything about G3D power OFF.'
        }
        $finished = $true
    }
} finally {
    if ($started -and $phaseStarted) { Do-SafeExit }
    if ($null -ne $script:Log) {
        try { Drain-For 1000 } catch {}
        $script:Log.Flush()
        $script:Log.Close()
        $script:Log = $null
    }
    if ($null -ne $script:Serial) {
        $script:Serial.Dispose()
        $script:Serial = $null
    }
    if ($started) { Say ("Serial port closed; log saved: $LogPath") }
}
if ($finished) {
    Say 'COMPLETE. Analyze the saved log with tools/s9-p5-report.ps1 -Log <path>.'
}
