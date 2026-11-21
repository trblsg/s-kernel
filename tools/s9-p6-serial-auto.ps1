#requires -Version 5.1
<#
S9 P6 unified one-image lab. Preview by default; -Execute is explicit.
CLOSE PuTTY before direct COM capture. All bytes are logged verbatim.
P6.1..P6.5 in one BOOT+SYSTEM build; sysctl switches need no reflash.
-EnableActualG3D is a second explicit opt-in and prompts for GATE.
Never changes CPU suspend, DVFS target, GPU voltage, or GL draw/fences.
#>
[CmdletBinding()]
param(
    [ValidatePattern('^COM[0-9]+$')][string]$Port='COM125',
    [ValidateRange(1200,3000000)][int]$BaudRate=115200,
    [ValidateRange(3,45)][int]$AckTimeoutSeconds=18,
    [ValidateRange(30,3600)][int]$PhaseSeconds=120,
    [ValidateRange(30,3600)][int]$ScreenOffSeconds=110,
    [ValidateRange(10,180)][int]$DisplayTimeoutSeconds=60,
    [string]$LogPath='',
    [switch]$SkipDisplay,
    [switch]$EnableActualG3D,
    [switch]$PreflightOnly,
    [switch]$Execute
)
Set-StrictMode -Version 2
$ErrorActionPreference='Stop'
$script:Allowed='^sysctl kern\.hakios\.(?:(?:p6_mode|p6_flags|p6_phase|p6_log_ms|p4_mode|p5_mode|p5_threads|p5_phase|g3d_lab_gate|g3d_lab_idle_ms|g3d_lab_phase|g3d_lab_reset)=[0-9]{1,5}|(?:p6_stats|p5_stats|g3d_lab_stats))$'
$script:Serial=$null;$script:Log=$null
$script:Buf=New-Object byte[] 16384
$script:Recent='';$script:Partial=''
$script:ActualAttempted=$false
function Say([string]$s){Write-Host ('[{0:HH:mm:ss}] {1}' -f (Get-Date),$s)}
function Pump {
    if($null -eq $script:Serial){return}
    try{$n=$script:Serial.Read($script:Buf,0,$script:Buf.Length)}
    catch [System.TimeoutException]{return}
    if($n -le 0){return}
    $script:Log.Write($script:Buf,0,$n)
    $chunk=[Text.Encoding]::ASCII.GetString($script:Buf,0,$n)
    $script:Recent+=$chunk
    if($script:Recent.Length -gt 50000){$script:Recent=$script:Recent.Substring($script:Recent.Length-35000)}
    $script:Partial+=$chunk
    $pos=$script:Partial.IndexOf([char]10)
    while($pos -ge 0){
        $line=$script:Partial.Substring(0,$pos).TrimEnd([char]13)
        $script:Partial=$script:Partial.Substring($pos+1)
        if($line -match '^(usb>|usb: |p6: PHASE|p6-wake:|p6-shadow:|p6-churn:|p6-missing:|p6-ca-gl:|p6-ca-thread:|p5-gpu:|g3d-lab:|GenericARMPE: the display)'){
            Say ('DEVICE '+$line.Substring(0,[Math]::Min($line.Length,290)))
        }
        $pos=$script:Partial.IndexOf([char]10)
    }
    if($script:Partial.Length -gt 16384){$script:Partial=$script:Partial.Substring($script:Partial.Length-4096)}
}
function Drain([int]$ms){
    if(-not $Execute){return}
    $until=[DateTime]::UtcNow.AddMilliseconds($ms)
    while([DateTime]::UtcNow -lt $until){Pump}
}
function Send([string]$cmd){
    if($cmd -cnotmatch $script:Allowed){throw "Command not in P6 allowlist: $cmd"}
    if(-not $Execute){Say ('PREVIEW SEND/VERIFY: '+$cmd);return}
    Drain 200
    $script:Recent=''
    $data=[Text.Encoding]::ASCII.GetBytes($cmd+"`r")
    $script:Serial.Write($data,0,$data.Length)
    Say ('TX '+$cmd)
    $echo='usb> '+$cmd
    $readOnly=$cmd -match '^sysctl kern\.hakios\.(p6_stats|p5_stats|g3d_lab_stats)$'
    $ack=if($readOnly){'usb: '+($cmd -replace '^sysctl ','')+' = '}
        else{'usb: '+$cmd+': set (0)'}
    $until=[DateTime]::UtcNow.AddSeconds($AckTimeoutSeconds)
    while([DateTime]::UtcNow -lt $until){
        Pump
        if($script:Recent.Contains($echo) -and $script:Recent.Contains($ack)){
            Say ('ACK exact echo+result: '+$cmd)
            return
        }
        if($script:Recent.Contains('not a command (help)') -or
           $script:Recent.Contains('ENOENT:') -or $script:Recent.Contains('EINVAL:') -or
           $script:Recent.Contains('EPERM:') -or $script:Recent.Contains('EBUSY:')){
            throw ('Rejected sysctl: '+$cmd+'; '+$script:Recent.Substring([Math]::Max(0,$script:Recent.Length-400)))
        }
    }
    throw ('No confirmed echo AND ACK for '+$cmd+'; '+$script:Recent.Substring([Math]::Max(0,$script:Recent.Length-500)))
}
function Rest([string]$stage,[int]$seconds){
    Say ("$stage : $seconds seconds")
    if(-not $Execute){return}
    $until=[DateTime]::UtcNow.AddSeconds($seconds)
    while([DateTime]::UtcNow -lt $until){Pump}
}
function Display([string]$state){
    Say ('PHYSICAL ACTION: display '+$state+' NOW')
    if(-not $Execute){return}
    Drain 200
    $script:Recent=''
    try{[Console]::Beep(960,160)}catch{}
    $expected='GenericARMPE: the display '+$state
    $until=[DateTime]::UtcNow.AddSeconds($DisplayTimeoutSeconds)
    while([DateTime]::UtcNow -lt $until){
        Pump
        if($script:Recent.Contains($expected)){Say ('CONFIRMED physical display '+$state);return}
    }
    throw ('Display '+$state+' not confirmed in serial log')
}
function GateGuard {
    # Read status immediately before the REAL gate, never trust earlier output.
    if(-not $Execute){Say 'PREVIEW REQUIRE: g3d_lab_stats requested_mv=950, fault=0, domain=1, gpu_safe=1, tz_ok=1';return}
    Send 'sysctl kern.hakios.g3d_lab_stats'
    $line=[regex]::Matches($script:Recent,'usb: kern\.hakios\.g3d_lab_stats = ([^\r\n]+)') |
        Select-Object -Last 1
    if($null -eq $line -or $line.Count -eq 0){throw 'Cannot parse G3D status: gate refused'}
    $values=$line[0].Groups[1].Value
    foreach($pair in @('requested_mv=950','fault=0','domain=1','gpu_safe=1','tz_ok=1','mode=1')){
        if($values -notmatch ('(?:^|\s)'+[regex]::Escape($pair)+'(?:\s|$)')){throw "Gate refused: status missing $pair : $values"}
    }
    $confirm=Read-Host 'Danger: actual G3D OFF/ON changes hardware. Type GATE to proceed or Enter to skip'
    if($confirm -cne 'GATE'){throw 'Actual G3D trial canceled; cleanup follows'}
}
function Exit-Safe {
    Say 'SAFE EXIT observers OFF; actual G3D recovery is checked when attempted'
    if(-not $Execute -or $null -eq $script:Serial){return}
    $commands=@()
    if($script:ActualAttempted){$commands+=@('sysctl kern.hakios.g3d_lab_gate=0','sysctl kern.hakios.g3d_lab_reset=1','sysctl kern.hakios.g3d_lab_stats')}
    else{$commands+='sysctl kern.hakios.g3d_lab_gate=0'}
    $commands+=@('sysctl kern.hakios.p6_mode=0','sysctl kern.hakios.p5_mode=0',
        'sysctl kern.hakios.p5_threads=0','sysctl kern.hakios.p4_mode=0','sysctl kern.hakios.p6_phase=9','sysctl kern.hakios.p6_stats')
    foreach($cmd in $commands){try{Send $cmd}catch{Write-Warning ('UNCONFIRMED safe exit: '+$cmd+' '+$_.Exception.Message)}}
}
function Run-Phases {
    Send 'sysctl kern.hakios.g3d_lab_gate=0'
    Send 'sysctl kern.hakios.p6_mode=0'
    Send 'sysctl kern.hakios.p4_mode=0'
    Send 'sysctl kern.hakios.p5_mode=0'
    Send 'sysctl kern.hakios.p5_threads=0'
    Send 'sysctl kern.hakios.p6_phase=1'
    Rest 'A1 baseline diagnostic OFF screen ON' $PhaseSeconds

    Send 'sysctl kern.hakios.p6_flags=1'
    Send 'sysctl kern.hakios.p6_mode=2'
    Send 'sysctl kern.hakios.p4_mode=1'
    Send 'sysctl kern.hakios.p6_phase=2'
    Rest 'B1 P6.1 wake and CPU scheduler idle proxies' $PhaseSeconds

    Send 'sysctl kern.hakios.p6_flags=3'
    Send 'sysctl kern.hakios.p6_phase=3'
    Rest 'B2 P6.2 process churn/context switches (no crash assumed)' $PhaseSeconds

    Send 'sysctl kern.hakios.p6_flags=5'
    Send 'sysctl kern.hakios.p6_phase=4'
    Rest 'B3 P6.3 independent thermal/park shadow (NO actual changes)' $PhaseSeconds

    if(-not $SkipDisplay){
        Send 'sysctl kern.hakios.p6_flags=9'
        Send 'sysctl kern.hakios.p6_phase=5'
        Send 'sysctl kern.hakios.p5_mode=2'
        Send 'sysctl kern.hakios.p5_phase=5'
        Send 'sysctl kern.hakios.g3d_lab_idle_ms=10000'
        Send 'sysctl kern.hakios.g3d_lab_gate=1'
        Send 'sysctl kern.hakios.g3d_lab_phase=5'
        Display 'off'
        Rest 'B4 P6.4 G3D shadow while physically OFF' $ScreenOffSeconds
        Display 'on'
        Rest 'B4 wake again' 30
        Send 'sysctl kern.hakios.g3d_lab_stats'
        if($EnableActualG3D){
            GateGuard
            Send 'sysctl kern.hakios.p6_phase=6'
            Send 'sysctl kern.hakios.p5_phase=6'
            Send 'sysctl kern.hakios.g3d_lab_phase=6'
            $script:ActualAttempted=$true
            Send 'sysctl kern.hakios.g3d_lab_gate=2'
            Display 'off'
            Rest 'B5 P6.4 ACTUAL G3D gate, physical screen OFF' $ScreenOffSeconds
            Display 'on'
            Rest 'B5 verify GPU jobs and wake recovery' 45
            Send 'sysctl kern.hakios.g3d_lab_gate=0'
            Send 'sysctl kern.hakios.g3d_lab_stats'
        }
    }else{Say 'SCREEN phases skipped: cannot claim G3D OFF residency'}

    Send 'sysctl kern.hakios.p6_flags=17'
    Send 'sysctl kern.hakios.p6_phase=7'
    Send 'sysctl kern.hakios.p5_phase=7'
    Send 'sysctl kern.hakios.p5_mode=2'
    Send 'sysctl kern.hakios.p5_threads=1'
    Rest 'B6 P6.5 CA static frame and thread work; no touches' $PhaseSeconds
    Say 'Optional controlled 10-20s swipe test NOW (same wallpaper/apps); other settings unchanged'
    Rest 'B6 CA interactions/wake burst' 30

    Send 'sysctl kern.hakios.p6_mode=0'
    Send 'sysctl kern.hakios.p5_mode=0'
    Send 'sysctl kern.hakios.p5_threads=0'
    Send 'sysctl kern.hakios.p4_mode=0'
    Send 'sysctl kern.hakios.p6_phase=8'
    Rest 'A2 reversal observer OFF' 60
}
Say ('S9 P6 unified COM one-image lab: '+$(if($Execute){'EXECUTE'}else{'PREVIEW ONLY'}))
if($EnableActualG3D){Say 'WARNING: ACTUAL G3D trial requested; runtime confirmation still required'}
else{Say 'G3D uses SHADOW only. Actual gating disabled by default.'}
if(-not $Execute){
    Send 'sysctl kern.hakios.p6_stats'
    Send 'sysctl kern.hakios.g3d_lab_stats'
    Send 'sysctl kern.hakios.p6_phase=0'
    if(-not $PreflightOnly){Run-Phases}
    Say 'PREVIEW complete; no COM port opened, no commands sent'
    return
}
if($env:OS -ne 'Windows_NT'){throw 'Windows serial COM required'}
if([IO.Ports.SerialPort]::GetPortNames() -notcontains $Port){throw "$Port not enumerated in Windows"}
if((Read-Host "Close PuTTY; type exact $Port to own and log this serial device") -cne $Port){
    throw 'COM confirmation failed'
}
if([string]::IsNullOrWhiteSpace($LogPath)){
    $LogPath='putty-P6-unified-'+(Get-Date -Format 'yyyyMMdd-HHmmss')+'.log'
}
$LogPath=[IO.Path]::GetFullPath($LogPath)
$started=$false;$finished=$false
try{
    $script:Serial=[IO.Ports.SerialPort]::new($Port,$BaudRate,[IO.Ports.Parity]::None,8,[IO.Ports.StopBits]::One)
    $script:Serial.Handshake=[IO.Ports.Handshake]::None
    $script:Serial.DtrEnable=$true;$script:Serial.RtsEnable=$false
    $script:Serial.ReadTimeout=250;$script:Serial.WriteTimeout=5000
    $script:Serial.ReadBufferSize=131072
    $script:Serial.Open()
    $script:Log=[IO.FileStream]::new($LogPath,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read)
    $started=$true
    Drain 1200
    Send 'sysctl kern.hakios.p6_stats'
    Send 'sysctl kern.hakios.g3d_lab_stats'
    Send 'sysctl kern.hakios.p6_phase=0'
    Say 'PREFLIGHT PASS: exact echo and ACK for P6 sysctls'
    if(-not $PreflightOnly){Run-Phases}
    $finished=$true
} finally {
    if($started -and -not $PreflightOnly){Exit-Safe}
    if($null -ne $script:Log){
        try{Drain 800}catch{}
        $script:Log.Flush();$script:Log.Dispose();$script:Log=$null
    }
    if($null -ne $script:Serial){$script:Serial.Dispose();$script:Serial=$null}
    if($started){Say ('RAW LOG '+$LogPath)}
}
if($finished){Say 'COMPLETE: run tools/s9-p6-report.ps1 -Log <path>'}
