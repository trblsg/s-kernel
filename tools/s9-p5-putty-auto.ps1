#requires -version 5.1
<#
.SYNOPSIS
  Automates S9 P5 A1/B1/A2/B2/OFF/ON sysctl phases through the ALREADY OPEN
  PuTTY serial terminal (typically COM125); does not open the COM port itself.
.DESCRIPTION
  Preview by default. -Execute requires an explicit confirmation and a
  user-observed P5 sysctl response before starting timed phases.
  Requires an interactive Windows desktop / Windows PowerShell 5.1 session.
  PuTTY must be logged in at a shell and its serial logfile already enabled.
  Do not use with PuTTY remote command (-m): that is not serial shell scripting.
.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\s9-p5-putty-auto.ps1
.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\s9-p5-putty-auto.ps1 -Port COM125 -Execute
.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\s9-p5-putty-auto.ps1 -Port COM125 -PuttyProcessId 1234 -Execute
#>
[CmdletBinding()]
param(
    [ValidatePattern('^COM[0-9]+$')][string]$Port = 'COM125',
    [int]$PuttyProcessId = 0,
    [ValidateRange(1,3600)][int]$BaselineSeconds = 120,
    [ValidateRange(1,3600)][int]$PassiveSeconds = 180,
    [ValidateRange(1,3600)][int]$ReversalSeconds = 120,
    [ValidateRange(1,3600)][int]$BurstSeconds = 120,
    [ValidateRange(30,3600)][int]$ScreenOffSeconds = 110,
    [ValidateRange(1,3600)][int]$AfterWakeSeconds = 120,
    [switch]$SkipScreenCycle,
    [switch]$Execute
)

Set-StrictMode -Version 2
$ErrorActionPreference = 'Stop'

# ALL transmitted input is drawn from this finite, read-only lab allowlist.
# No shell escapes, arbitrary text, voltage/clock edits or GPU gating.
$script:AllowedCommand = '^sysctl kern\.hakios\.(?:(?:p4_mode|p4_threads|p5_mode|p5_threads|p5_phase)=[0-9]{1,2}|p5_stats)$'
$script:TargetHandle = [IntPtr]::Zero
$script:ConsoleHandle = [IntPtr]::Zero
$script:TargetProcessId = 0
$script:TargetTitle = ''

function Write-Stage([string]$Message) {
    Write-Host ('[{0:HH:mm:ss}] {1}' -f (Get-Date), $Message)
}

function Find-Putty {
    $candidates = @(Get-Process -Name 'putty' -ErrorAction SilentlyContinue |
        Where-Object { $_.MainWindowHandle -ne [IntPtr]::Zero })

    if ($PuttyProcessId -gt 0) {
        $candidates = @($candidates | Where-Object { $_.Id -eq $PuttyProcessId })
        if ($candidates.Count -ne 1) {
            throw "PuTTY PID $PuttyProcessId not found as a visible desktop window."
        }
        if ($candidates[0].MainWindowTitle -notmatch [regex]::Escape($Port)) {
            Write-Warning ("Selected PID cannot be proven to use {0} from its title. Check PuTTY Configuration > Serial before continuing." -f $Port)
        }
    } else {
        $candidates = @($candidates | Where-Object {
            $_.MainWindowTitle -match ('(?<![A-Za-z0-9])' + [regex]::Escape($Port) + '(?![0-9])')
        })
        if ($candidates.Count -ne 1) {
            $all = @(Get-Process -Name 'putty' -ErrorAction SilentlyContinue |
                Where-Object { $_.MainWindowHandle -ne [IntPtr]::Zero } |
                ForEach-Object { 'PID=' + $_.Id + ' title=' + $_.MainWindowTitle })
            throw ("Expected exactly ONE PuTTY window whose title contains {0}; found {1}. Windows: {2}. Use -PuttyProcessId after verifying its serial port." -f $Port,$candidates.Count,($all -join ' / '))
        }
    }

    $script:TargetProcessId = [int]$candidates[0].Id
    $script:TargetHandle = $candidates[0].MainWindowHandle
    $script:TargetTitle = [string]$candidates[0].MainWindowTitle
}

function Initialize-Keyboard {
    if ($env:OS -ne 'Windows_NT') {
        throw 'The -Execute path requires Windows desktop and an interactive PuTTY window.'
    }
    Add-Type -AssemblyName System.Windows.Forms
    if (-not ('Ioss9P5PuttyNative' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class Ioss9P5PuttyNative {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool ShowWindowAsync(IntPtr hWnd, int cmd);
    [DllImport("kernel32.dll")] public static extern IntPtr GetConsoleWindow();
}
'@
    }
    $script:ConsoleHandle = [Ioss9P5PuttyNative]::GetConsoleWindow()
    if ($script:ConsoleHandle -eq [IntPtr]::Zero) {
        throw 'Cannot locate the Windows PowerShell console. Run from a classic, separate powershell.exe console, not Windows Terminal or a background job.'
    }
}

function Focus-ControllerConsole {
    if (-not [Ioss9P5PuttyNative]::IsWindow($script:ConsoleHandle)) {
        throw 'PowerShell controller console disappeared.'
    }
    [void][Ioss9P5PuttyNative]::ShowWindowAsync($script:ConsoleHandle, 9)
    [void][Ioss9P5PuttyNative]::SetForegroundWindow($script:ConsoleHandle)
    Start-Sleep -Milliseconds 250
    if ([Ioss9P5PuttyNative]::GetForegroundWindow() -ne $script:ConsoleHandle) {
        Write-Warning 'Please click the PowerShell console to answer the confirmation; waiting up to 30 seconds.'
        for ($i = 0; $i -lt 30; $i++) {
            Start-Sleep -Seconds 1
            if ([Ioss9P5PuttyNative]::GetForegroundWindow() -eq $script:ConsoleHandle) { return }
        }
        throw 'PowerShell console never regained focus; no further input sent to PuTTY.'
    }
}

function Focus-VerifiedPutty {
    if (-not [Ioss9P5PuttyNative]::IsWindow($script:TargetHandle)) {
        throw 'PuTTY window has closed; aborting automation.'
    }
    $process = Get-Process -Id $script:TargetProcessId -ErrorAction SilentlyContinue
    if ($null -eq $process -or $process.MainWindowHandle -ne $script:TargetHandle) {
        throw 'PuTTY PID/window changed; aborting rather than typing into the wrong window.'
    }
    [void][Ioss9P5PuttyNative]::ShowWindowAsync($script:TargetHandle, 9)
    [void][Ioss9P5PuttyNative]::SetForegroundWindow($script:TargetHandle)
    Start-Sleep -Milliseconds 250
    if ([Ioss9P5PuttyNative]::GetForegroundWindow() -ne $script:TargetHandle) {
        throw 'PuTTY could not obtain keyboard focus. No command was sent.'
    }
}

function Send-LabCommand([string]$Command) {
    if ($Command -cnotmatch $script:AllowedCommand) {
        throw "Blocked non-lab command: $Command"
    }
    if (-not $Execute) {
        Write-Stage ("PREVIEW SEND: " + $Command)
        return
    }
    Focus-VerifiedPutty
    # Commands contain only SendKeys-literal ASCII letters/digits/dots/underscores,
    # equals and spaces. No SendKeys metacharacters or clipboard injection.
    [System.Windows.Forms.SendKeys]::SendWait($Command + '{ENTER}')
    Start-Sleep -Milliseconds 220
    Write-Stage ("Sent to PuTTY PID {0}: {1}" -f $script:TargetProcessId, $Command)
}

function Wait-Phase([string]$Description, [int]$Seconds) {
    if (-not $Execute) {
        Write-Stage ("PREVIEW WAIT {0}s: {1}" -f $Seconds, $Description)
        return
    }
    Write-Stage ("WAIT {0}s: {1}" -f $Seconds, $Description)
    Start-Sleep -Seconds $Seconds
}

function Request-PhysicalAction([string]$Instruction) {
    if (-not $Execute) {
        Write-Stage ("PREVIEW OPERATOR ACTION: " + $Instruction)
        return
    }
    try { [console]::Beep(920, 180) } catch {}
    Focus-ControllerConsole
    Write-Stage $Instruction
    # Read-Host deliberately brings attention back to the script. Send-LabCommand
    # will refocus the verified PuTTY window after the physical display action.
    [void](Read-Host 'Press ENTER in this PowerShell console after completing this action')
}

function Assert-UserConfirmation([string]$Instruction, [string]$Expected) {
    Focus-ControllerConsole
    $reply = Read-Host $Instruction
    if ($reply -cne $Expected) {
        throw "Confirmation mismatch. Expected '$Expected'; no phase plan started."
    }
}

$completed = $false
$started = $false
try {
    Write-Stage ("P5 one-boot plan for {0}; mode={1}" -f $Port, $(if($Execute){'EXECUTE'}else{'PREVIEW ONLY'}))
    Write-Stage 'Conditions: P5 matching BOOT+SYSTEM installed, shell logged in, PuTTY serial LOG enabled, warm boot >=420s.'
    Write-Stage 'The plan NEVER opens COM125; PuTTY continues to own serial and its log.'
    Write-Stage 'Leave charging, brightness and wallpaper unchanged. Do not type into the PC during automatic phase timing.'
    if ($Execute) {
        Initialize-Keyboard
        Find-Putty
        Write-Stage ("Target PID={0} title={1}" -f $script:TargetProcessId, $script:TargetTitle)
        Assert-UserConfirmation ("Verify this really is PuTTY on $Port and enter $Port to authorize one read-only probe") $Port
        Send-LabCommand 'sysctl kern.hakios.p5_stats'
        Assert-UserConfirmation 'Check PuTTY output: p5_stats exists and is readable (no unknown oid). Type READY to continue' 'READY'
        $started = $true
    }

    Write-Stage 'A1: screen ON, P5/P4 diagnostics OFF'
    Send-LabCommand 'sysctl kern.hakios.p4_mode=0'
    Send-LabCommand 'sysctl kern.hakios.p4_threads=0'
    Send-LabCommand 'sysctl kern.hakios.p5_threads=0'
    Send-LabCommand 'sysctl kern.hakios.p5_mode=0'
    Send-LabCommand 'sysctl kern.hakios.p5_phase=1'
    Wait-Phase 'A1 steady screen ON baseline' $BaselineSeconds

    Write-Stage 'B1: passive P5, backboardd thread tracing, P4 CPU idle'
    Send-LabCommand 'sysctl kern.hakios.p5_threads=1'
    Send-LabCommand 'sysctl kern.hakios.p5_mode=1'
    Send-LabCommand 'sysctl kern.hakios.p4_mode=1'
    Send-LabCommand 'sysctl kern.hakios.p5_phase=2'
    Send-LabCommand 'sysctl kern.hakios.p5_stats'
    Wait-Phase 'B1 passive (discard first 10s in analysis)' $PassiveSeconds

    Write-Stage 'A2: reversal to estimate measurement overhead'
    Send-LabCommand 'sysctl kern.hakios.p5_mode=0'
    Send-LabCommand 'sysctl kern.hakios.p5_threads=0'
    Send-LabCommand 'sysctl kern.hakios.p4_mode=0'
    Send-LabCommand 'sysctl kern.hakios.p5_phase=3'
    Wait-Phase 'A2 steady screen ON reversal' $ReversalSeconds

    Write-Stage 'B2: bounded wake burst detail'
    Send-LabCommand 'sysctl kern.hakios.p5_threads=1'
    Send-LabCommand 'sysctl kern.hakios.p5_mode=2'
    Send-LabCommand 'sysctl kern.hakios.p4_mode=1'
    Send-LabCommand 'sysctl kern.hakios.p5_phase=4'
    Wait-Phase 'B2 detailed screen ON' $BurstSeconds

    if (-not $SkipScreenCycle) {
        Write-Stage 'C1: physical screen OFF/ON; P5 mode2 remains active'
        Send-LabCommand 'sysctl kern.hakios.p5_phase=5'
        Request-PhysicalAction 'Physically turn S9 display OFF now. Do not unplug serial or change charger.'
        Wait-Phase 'Real physical screen OFF, G3D PMU/CMU sampling continues' $ScreenOffSeconds
        Request-PhysicalAction 'Physically turn S9 display ON now, then press ENTER promptly; avoid touch gestures after waking.'
        Send-LabCommand 'sysctl kern.hakios.p5_phase=6'
        Send-LabCommand 'sysctl kern.hakios.p5_stats'
        Wait-Phase 'Wake burst settles, display remains ON' $AfterWakeSeconds
    } else {
        Write-Stage 'Screen cycle skipped explicitly; no OFF-state power conclusions permitted.'
    }
    $completed = $true
} finally {
    Write-Stage 'SAFE EXIT: disable diagnostics on the same verified PuTTY window'
    # Do not claim safety commands reached the device if PuTTY closed or lost focus.
    foreach ($cmd in @(
        'sysctl kern.hakios.p5_mode=0',
        'sysctl kern.hakios.p5_threads=0',
        'sysctl kern.hakios.p4_mode=0',
        'sysctl kern.hakios.p5_phase=7',
        'sysctl kern.hakios.p5_stats')) {
        try {
            if (-not $Execute -or $started) { Send-LabCommand $cmd }
        } catch {
            Write-Warning ("SAFE EXIT NOT VERIFIED: " + $_.Exception.Message)
            Write-Warning ("Manually enter: " + $cmd)
        }
    }
}
if ($completed) {
    Write-Stage 'P5 plan finished. Save PuTTY serial log as putty-P5-display-wake-gpu.log.'
    Write-Stage 'Analyze: powershell -ExecutionPolicy Bypass -File tools\s9-p5-report.ps1 -Log putty-P5-display-wake-gpu.log'
}
