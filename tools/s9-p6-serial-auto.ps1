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
    [ValidateRange(10,300)][int]$DisplayTimeoutSeconds=120,
    [string]$LogPath='',
    [switch]$SkipDisplay,
    [switch]$ResumeCAOnly,
    [switch]$ExtendedOnly,
    [switch]$DeepOnly,
    [switch]$ParallelOnly,
    [ValidateRange(40,46)][int]$BatteryStopC=46,
    [switch]$ExtendedScreenOff,
    [switch]$TestDisplayEvidence,
    [switch]$TestParallelThermal,
    [switch]$EnableActualG3D,
    [switch]$PreflightOnly,
    [switch]$Execute
)
Set-StrictMode -Version 2
$ErrorActionPreference='Stop'
$script:Allowed='^sysctl kern\.hakios\.(?:(?:p66_mode|p67_mode|p68_mode|p69_mode|p610_mode|p611_mode|p612_mode|p6_mode|p6_flags|p6_phase|p6_log_ms|p4_mode|p5_mode|p5_threads|p5_phase|g3d_lab_gate|g3d_lab_idle_ms|g3d_lab_rail_probe|g3d_lab_phase|g3d_lab_reset)=[0-9]{1,5}|(?:p66_mode|p67_mode|p68_mode|p69_mode|p610_mode|p611_mode|p612_mode|p6_stats|p5_stats|g3d_lab_stats))$'
$script:Serial=$null;$script:Log=$null
$script:Buf=New-Object byte[] 16384
$script:Recent='';$script:Partial=''
$script:ActualAttempted=$false
$script:OnEventProven=$false
$script:BatteryLatestC=0.0;$script:BatteryStopReached=$false;$script:ThermalWatchActive=$false
function Say([string]$s){Write-Host ('[{0:HH:mm:ss}] {1}' -f (Get-Date),$s)}
function Get-P6BatteryTemp([string]$line){
    if($line -match '^tmu: .*\bbattery=(\d+)(?:\s|$)'){return [double]$Matches[1]}
    if($line -match '^max77705: (?:now|every 5 minutes): .*? (-?\d+)\.(\d+) C \('){
        return [double]$Matches[1]+([double]$Matches[2]/10.0)
    }
    return $null
}
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
        if($ParallelOnly){
            $temp=Get-P6BatteryTemp $line
            if($null -ne $temp){
                $script:BatteryLatestC=$temp
                if($script:ThermalWatchActive -and $temp -ge $BatteryStopC -and -not $script:BatteryStopReached){
                    $script:BatteryStopReached=$true
                    Say ("THERMAL STOP requested: observed battery temperature "+$temp+" C >= "+$BatteryStopC+" C; read-only observers will shut off")
                }
            }
        }
        if($line -match '^(usb>|usb: |p6: PHASE|p6-wake:|p6-shadow:|p6-churn:|p6-missing:|p6-ca-gl:|p6-ca-thread:|p66-ca:|p67-cost:|p67-path:|p68-g3d:|p69-source:|p610-gov:|p610-gap:|p610-trans:|max77705: charger|p611-hash:|p611-surface:|p612-eagl:|p5-gpu:|g3d-lab:|GenericARMPE: the display)'){
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
    $readOnly=$cmd -match '^sysctl kern\.hakios\.(p66_mode|p67_mode|p68_mode|p69_mode|p610_mode|p611_mode|p612_mode|p6_stats|p5_stats|g3d_lab_stats)$'
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
    while([DateTime]::UtcNow -lt $until){Pump;if($ParallelOnly -and $script:BatteryStopReached){throw "Battery temperature threshold exceeded ($($script:BatteryLatestC) C >= $BatteryStopC C); SAFE EXIT"}}
}
# Cross-check independent ON evidence if the one-shot GenericARMPE line was
# dropped/interleaved on USB. This is *compositor activity*, NOT proof the
# physical panel lit. Never use this fallback to authorize real G3D gating.
function Get-P6DisplayEvidence([string]$state,[string]$logText){
    if($logText.Contains('GenericARMPE: the display '+$state)){
        return 'KERNEL_EVENT'
    }
    if($state -ne 'on'){return 'NONE'} # OFF requires direct hardware/request marker
    $frames=0
    $latestUp=[long]-1
    $samples=[regex]::Matches($logText,'(?m)^p5-gpu: up=(\d+)ms[^\r\n]*?frame_age_ms=(\d+)')
    foreach($m in $samples){
        $up=[long]$m.Groups[1].Value
        $age=[long]$m.Groups[2].Value
        if($up -gt $latestUp){
            if($age -le 1500){$frames++}else{$frames=0}
            $latestUp=$up
        }
    }
    # 2 distinct five-second snapshots; merely a single short frame age may
    # be a transient or an interleaved serial line.
    if($frames -ge 2){return 'FRAME_PROXY'}
    $wake=[regex]::Matches($logText,'(?m)^p6-wake: up=(\d+)ms[^\r\n]*?presents=(\d+)')
    $presentUp=[long]-1
    foreach($m in $wake){
        if([long]$m.Groups[2].Value -gt 0){
            $presentUp=[long]$m.Groups[1].Value
        }
    }
    if($frames -ge 1 -and $presentUp -gt 0 -and
        [math]::Abs($latestUp-$presentUp) -le 15000){return 'FRAME_PROXY'}
    return 'NONE'
}
function Display([ValidateSet('on','off')][string]$state){
    $script:OnEventProven=$false
    Say ('PHYSICAL ACTION: press the S9 power button to turn display '+$state+
        ' NOW; confirm actual panel state (this script does not press the key)')
    if(-not $Execute){return}
    Drain 200
    $script:Recent=''
    try{[Console]::Beep(960,160)}catch{}
    $until=[DateTime]::UtcNow.AddSeconds($DisplayTimeoutSeconds)
    $reminded=$false
    while([DateTime]::UtcNow -lt $until){
        Pump
        $evidence=Get-P6DisplayEvidence $state $script:Recent
        if($evidence -eq 'KERNEL_EVENT'){
            if($state -eq 'on'){$script:OnEventProven=$true}
            Say ('CONFIRMED display '+$state+' via GenericARMPE kernel request marker')
            return
        }
        if($state -eq 'on' -and $evidence -eq 'FRAME_PROXY'){
            Say 'WARNING: GenericARMPE display ON marker MISSING, but two independent frame-activity observations resumed.'
            Say 'PROXY ONLY: physical panel power state is UNVERIFIED; real G3D gate phase will be DISABLED.'
            return
        }
        if(-not $reminded -and [DateTime]::UtcNow -gt $until.AddSeconds(-[math]::Ceiling($DisplayTimeoutSeconds/2))){
            $reminded=$true
            Say ('STILL WAITING for display '+$state+'. Check screen; press physical power key if it stayed off.')
            try{[Console]::Beep(1120,200)}catch{}
        }
    }
    $lastGpu=[regex]::Matches($script:Recent,'(?m)^p5-gpu: up=(\d+)ms[^\r\n]*?frame_age_ms=(\d+)') |
        Select-Object -Last 1
    $lastAge=if($null -ne $lastGpu){$lastGpu.Groups[2].Value}else{'unmeasured'}
    throw ("Display $state NOT CONFIRMED in kernel log or frame-activity telemetry within $DisplayTimeoutSeconds seconds; last frame_age_ms=$lastAge. Data for earlier phases is intact; do not infer successful wake. Use -ResumeCAOnly after physically restoring screen ON.")
}
function GateGuard {
    # Read status immediately before the REAL gate, never trust earlier output.
    if(-not $Execute){Say 'PREVIEW REQUIRE: g3d_lab_stats requested_mv=950 rail_valid=1 rail_last_mv=950 fault=0 domain=1 gpu_safe=1 tz_ok=1';return}
    Send 'sysctl kern.hakios.g3d_lab_stats'
    $line=[regex]::Matches($script:Recent,'usb: kern\.hakios\.g3d_lab_stats = ([^\r\n]+)') |
        Select-Object -Last 1
    if($null -eq $line -or $line.Count -eq 0){throw 'Cannot parse G3D status: gate refused'}
    $values=$line[0].Groups[1].Value
    foreach($pair in @('requested_mv=950','rail_valid=1','rail_last_mv=950','fault=0','domain=1','gpu_safe=1','tz_ok=1','mode=1')){
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
    $commands+=@('sysctl kern.hakios.p610_mode=0','sysctl kern.hakios.p611_mode=0','sysctl kern.hakios.p612_mode=0',
        'sysctl kern.hakios.p66_mode=0','sysctl kern.hakios.p67_mode=0',
        'sysctl kern.hakios.p68_mode=0','sysctl kern.hakios.p69_mode=0',
        'sysctl kern.hakios.g3d_lab_rail_probe=0','sysctl kern.hakios.g3d_lab_phase=0',
        'sysctl kern.hakios.p6_mode=0','sysctl kern.hakios.p5_mode=0',
        'sysctl kern.hakios.p5_threads=0','sysctl kern.hakios.p4_mode=0','sysctl kern.hakios.p6_phase=9','sysctl kern.hakios.p6_stats')
    foreach($cmd in $commands){try{Send $cmd}catch{Write-Warning ('UNCONFIRMED safe exit: '+$cmd+' '+$_.Exception.Message)}}
}
function Run-ParallelPhases {
    if($script:BatteryStopReached){throw "Battery was already above $BatteryStopC C at preflight; skip parallel trial and SAFE EXIT"}
    Say 'PARALLEL charger/Scion/CA/IOSurface lab: read-only, independent short A/B windows'
    Say ('Battery thermal guard '+$BatteryStopC+' C; no change to charging, DVFS or G3D policy')
    foreach($node in @('p66_mode','p67_mode','p68_mode','p69_mode','p610_mode','p611_mode','p612_mode')){
        Send ('sysctl kern.hakios.'+$node+'=0')
    }
    Send 'sysctl kern.hakios.g3d_lab_gate=0'
    Send 'sysctl kern.hakios.g3d_lab_rail_probe=0'
    Send 'sysctl kern.hakios.p4_mode=0'
    Send 'sysctl kern.hakios.p6_mode=0'
    Send 'sysctl kern.hakios.p5_mode=0'
    Send 'sysctl kern.hakios.p5_threads=0'
    Send 'sysctl kern.hakios.p6_phase=30'
    $script:ThermalWatchActive=$true # ignore historical boot-log replay; act on live readings after ACK
    Rest 'P6 PARALLEL A1 60s all runtime observers off; MAX77705 bootarg probes already active since boot' 60

    Send 'sysctl kern.hakios.p6_flags=17'
    Send 'sysctl kern.hakios.p6_mode=2'
    Send 'sysctl kern.hakios.p5_mode=2'
    Send 'sysctl kern.hakios.p5_threads=1'
    foreach($node in @('p610_mode','p611_mode','p612_mode')){Send ('sysctl kern.hakios.'+$node+'=1')}
    Send 'sysctl kern.hakios.p6_phase=31'
    Send 'sysctl kern.hakios.p5_phase=31'
    Rest 'P6 PARALLEL all three observer categories simultaneously (one boot)' 60

    Send 'sysctl kern.hakios.p611_mode=0'
    Send 'sysctl kern.hakios.p612_mode=0'
    Send 'sysctl kern.hakios.p6_phase=32'
    Send 'sysctl kern.hakios.p5_phase=32'
    Rest 'P6 PARALLEL Scion transition snapshots; checksum/display observers OFF' 60

    Send 'sysctl kern.hakios.p610_mode=0'
    Send 'sysctl kern.hakios.p611_mode=1'
    Send 'sysctl kern.hakios.p6_phase=33'
    Send 'sysctl kern.hakios.p5_phase=33'
    Rest 'P6 PARALLEL IOSurface timing only, separated from governor/output telemetry' 60

    Send 'sysctl kern.hakios.p611_mode=0'
    Send 'sysctl kern.hakios.p612_mode=1'
    Send 'sysctl kern.hakios.p6_phase=34'
    Send 'sysctl kern.hakios.p5_phase=34'
    Rest 'P6 PARALLEL EAGL present vs independent framebuffer FPS only' 60

    foreach($node in @('p610_mode','p611_mode','p612_mode')){Send ('sysctl kern.hakios.'+$node+'=0')}
    Send 'sysctl kern.hakios.p6_mode=0'
    Send 'sysctl kern.hakios.p5_mode=0'
    Send 'sysctl kern.hakios.p5_threads=0'
    Send 'sysctl kern.hakios.p4_mode=0'
    Send 'sysctl kern.hakios.p6_phase=35'
    Rest 'P6 PARALLEL A2 60s observer OFF, record current meter again; battery gauge persists' 60
}
function Run-DeepPhases {
    Say 'P6.10-12 read-only diagnostic phases; no CPU governor, GL, buffer or display writes'
    foreach($node in @('p66_mode','p67_mode','p68_mode','p69_mode','p610_mode','p611_mode','p612_mode')){
        Send ('sysctl kern.hakios.'+$node+'=0')
    }
    Send 'sysctl kern.hakios.g3d_lab_gate=0'
    Send 'sysctl kern.hakios.p6_mode=0'
    Send 'sysctl kern.hakios.p5_mode=0'
    Send 'sysctl kern.hakios.p5_threads=0'
    Send 'sysctl kern.hakios.p4_mode=0'
    Send 'sysctl kern.hakios.p6_phase=20'
    Rest 'P6.10-12 A1 screen-ON governor/CA/scanout observers OFF' 60

    Send 'sysctl kern.hakios.p6_flags=17'
    Send 'sysctl kern.hakios.p6_mode=2'
    Send 'sysctl kern.hakios.p5_mode=2'
    Send 'sysctl kern.hakios.p5_threads=1'
    Send 'sysctl kern.hakios.p610_mode=1'
    Send 'sysctl kern.hakios.p6_phase=21'
    Send 'sysctl kern.hakios.p5_phase=21'
    Rest 'P6.10 Scion governor actual-vs-shadow state and frame-gap evidence' $PhaseSeconds

    Send 'sysctl kern.hakios.p610_mode=0'
    Send 'sysctl kern.hakios.p611_mode=1'
    Send 'sysctl kern.hakios.p6_phase=22'
    Send 'sysctl kern.hakios.p5_phase=22'
    Rest 'P6.11 IOSurface skip reasons and sampled hash time/bytes per source' $PhaseSeconds

    Send 'sysctl kern.hakios.p611_mode=0'
    Send 'sysctl kern.hakios.p612_mode=1'
    Send 'sysctl kern.hakios.p6_phase=23'
    Send 'sysctl kern.hakios.p5_phase=23'
    Rest 'P6.12 EAGL A/B output + independent kernel fps presented/flips/zero-copy' $PhaseSeconds

    foreach($node in @('p610_mode','p611_mode','p612_mode')){
        Send ('sysctl kern.hakios.'+$node+'=0')
    }
    Send 'sysctl kern.hakios.p6_mode=0'
    Send 'sysctl kern.hakios.p5_mode=0'
    Send 'sysctl kern.hakios.p5_threads=0'
    Send 'sysctl kern.hakios.p4_mode=0'
    Send 'sysctl kern.hakios.p6_phase=24'
    Rest 'P6.10-12 A2 observer-OFF thermal/performance reversal' 60
}
function Run-ExtendedPhases {
    Say 'EXTENDED P6.6-P6.9: read-only; no skip/damage enforcement, clock, voltage, poll or power gating'
    Send 'sysctl kern.hakios.g3d_lab_gate=0'
    Send 'sysctl kern.hakios.g3d_lab_rail_probe=0'
    Send 'sysctl kern.hakios.g3d_lab_phase=0'
    Send 'sysctl kern.hakios.p66_mode=0'
    Send 'sysctl kern.hakios.p67_mode=0'
    Send 'sysctl kern.hakios.p68_mode=0'
    Send 'sysctl kern.hakios.p69_mode=0'
    Send 'sysctl kern.hakios.p6_mode=0'
    Send 'sysctl kern.hakios.p5_mode=0'
    Send 'sysctl kern.hakios.p5_threads=0'
    Send 'sysctl kern.hakios.p4_mode=0'
    Send 'sysctl kern.hakios.p6_phase=10'
    Rest 'P6.6-9 A1 screen-ON observer-OFF baseline (keep brightness/static scene)' 60

    Send 'sysctl kern.hakios.p6_flags=17'
    Send 'sysctl kern.hakios.p6_mode=2'
    Send 'sysctl kern.hakios.p5_mode=2'
    Send 'sysctl kern.hakios.p5_threads=1'
    Send 'sysctl kern.hakios.p66_mode=1'
    Send 'sysctl kern.hakios.p6_phase=11'
    Send 'sysctl kern.hakios.p5_phase=11'
    Rest 'P6.6 target identity / IOSurface evidence (UNVERIFIED pixel sameness)' $PhaseSeconds

    Send 'sysctl kern.hakios.p66_mode=0'
    Send 'sysctl kern.hakios.p67_mode=1'
    Send 'sysctl kern.hakios.p6_phase=12'
    Send 'sysctl kern.hakios.p5_phase=12'
    Rest 'P6.7 EAGL GL slot costs + sampled native vs lookup/check' $PhaseSeconds

    Send 'sysctl kern.hakios.p67_mode=0'
    Send 'sysctl kern.hakios.p68_mode=1'
    Send 'sysctl kern.hakios.p6_flags=9'
    Send 'sysctl kern.hakios.p6_phase=13'
    Send 'sysctl kern.hakios.p5_phase=13'
    Send 'sysctl kern.hakios.g3d_lab_phase=13'
    Send 'sysctl kern.hakios.g3d_lab_gate=1'
    Rest 'P6.8 G3D ON/read-only quiet vs PMU correlation; shadow gate1 ONLY' $PhaseSeconds
    if($ExtendedScreenOff){
        Send 'sysctl kern.hakios.p6_phase=14'
        Send 'sysctl kern.hakios.p5_phase=14'
        Send 'sysctl kern.hakios.g3d_lab_phase=14'
        Display 'off'
        Rest 'P6.8 optional physical OFF 5s PMU readback; NO actual gate' $ScreenOffSeconds
        Display 'on'
        Rest 'P6.8 verify physical wake and new GPU jobs; no actual G3D gate' 30
    }

    Send 'sysctl kern.hakios.g3d_lab_gate=0'
    Send 'sysctl kern.hakios.g3d_lab_phase=0'
    Send 'sysctl kern.hakios.p68_mode=0'
    Send 'sysctl kern.hakios.p5_mode=0'
    Send 'sysctl kern.hakios.p5_threads=0'
    Send 'sysctl kern.hakios.p6_flags=1'
    Send 'sysctl kern.hakios.p69_mode=1'
    Send 'sysctl kern.hakios.p6_phase=15'
    Rest 'P6.9 1s USB/touch/KEY actual poll counts; no human touch unless explicitly testing' $PhaseSeconds
    Send 'sysctl kern.hakios.p69_mode=0'

    Send 'sysctl kern.hakios.p66_mode=0'
    Send 'sysctl kern.hakios.p67_mode=0'
    Send 'sysctl kern.hakios.p68_mode=0'
    Send 'sysctl kern.hakios.p6_mode=0'
    Send 'sysctl kern.hakios.p5_mode=0'
    Send 'sysctl kern.hakios.p5_threads=0'
    Send 'sysctl kern.hakios.p4_mode=0'
    Send 'sysctl kern.hakios.p6_phase=16'
    Rest 'P6.6-9 A2 screen-ON observer-OFF reversal' 60
}
function Run-CAStage {
    if($ResumeCAOnly){
        Say 'RESUME P6.5 ONLY: previous A/B/C phases are NOT repeated.'
        if($Execute){
            $visual=Read-Host 'Ensure screen is PHYSICALLY ON and interactive. Type ON to continue or anything else to abort'
            if($visual -cne 'ON'){throw 'CA-only resume canceled; screen ON not confirmed by operator'}
        }
        Send 'sysctl kern.hakios.g3d_lab_gate=0'
        Send 'sysctl kern.hakios.g3d_lab_rail_probe=0'
        Send 'sysctl kern.hakios.p4_mode=0'
        Send 'sysctl kern.hakios.p5_mode=0'
        Send 'sysctl kern.hakios.p5_threads=0'
        Send 'sysctl kern.hakios.p6_mode=2'
    }
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
function Run-Phases {
    if($ResumeCAOnly){Run-CAStage;return}
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
        Send 'sysctl kern.hakios.g3d_lab_rail_probe=1'
        Send 'sysctl kern.hakios.g3d_lab_phase=5'
        Display 'off'
        Rest 'B4 P6.4 G3D shadow while physically OFF' $ScreenOffSeconds
        Display 'on'
        Rest 'B4 wake again' 30
        Send 'sysctl kern.hakios.g3d_lab_stats'
        if($EnableActualG3D){
            if($Execute -and -not $script:OnEventProven){throw 'G3D real gate REFUSED: ON proxy only, missing GenericARMPE kernel event'}
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
    Run-CAStage
}
if($TestParallelThermal){
    $cases=@(
        @('tmu: 8 sensors read; hottest big at 79.0 C; battery=47',47.0),
        @('max77705: every 5 minutes: 99.73%, 4277 mV, 0 mA, 47.4 C (ADC 1113)',47.4),
        @('max77705: now: 80.00%, 4050 mV, -100 mA, 35.4 C (ADC 1560)',35.4),
        @('fps: 10 presented, 10 drawn, 10 flips, 0 lost, 0 held', $null)
    )
    foreach($c in $cases){
        $got=Get-P6BatteryTemp $c[0]
        if(($null -eq $got) -ne ($null -eq $c[1]) -or
           ($null -ne $got -and [Math]::Abs($got-[double]$c[1]) -gt 0.01)){
            throw "P6 thermal parser regression: $($c[0]) => $got, expected=$($c[1])"
        }
    }
    Say ('P6 thermal parser: '+$cases.Count+' deterministic tests PASS; no COM opened or commands sent')
    return
}
if($TestDisplayEvidence){
    $cases=@(
        @('on',"GenericARMPE: the display on (iOS's power request)",'KERNEL_EVENT'),
        @('off',"GenericARMPE: the display off (iOS's power request)",'KERNEL_EVENT'),
        @('on',"p5-gpu: up=1000ms phase=5 frame_age_ms=75 job_age_ms=0`np5-gpu: up=6000ms phase=5 frame_age_ms=99 job_age_ms=0",'FRAME_PROXY'),
        @('on',"p5-gpu: up=1000ms phase=5 frame_age_ms=75`np6-wake: up=5500ms phase=5 presents=50",'FRAME_PROXY'),
        @('on',"p5-gpu: up=1000ms phase=5 frame_age_ms=168034`np5-gpu: up=6000ms phase=5 frame_age_ms=173034",'NONE'),
        @('on',"p5-gpu: up=1000ms frame_age_ms=60`np5-gpu: up=6000ms frame_age_ms=60`np5-gpu: up=11000ms frame_age_ms=6000",'NONE'),
        @('on',"p5-gpu: up=1000ms phase=5 frame_age_ms=75",'NONE'),
        @('off',"p5-gpu: up=1000ms phase=5 frame_age_ms=500000`np5-gpu: up=6000ms phase=5 frame_age_ms=505000",'NONE'),
        @('on',"GenericARMPE: the display off",'NONE')
    )
    foreach($c in $cases){
        $got=Get-P6DisplayEvidence $c[0] $c[1]
        if($got -cne $c[2]){throw "Display evidence test FAILED for $($c[0]): expected=$($c[2]), got=$got"}
    }
    Say ('P6 display evidence: '+$cases.Count+' deterministic tests PASS; no COM opened or commands sent')
    return
}
if($ResumeCAOnly -and $EnableActualG3D){throw '-ResumeCAOnly cannot request hardware G3D gating'}
if($ExtendedOnly -and ($ResumeCAOnly -or $EnableActualG3D -or $DeepOnly -or $ParallelOnly)){throw '-ExtendedOnly is read-only and mutually exclusive with DeepOnly/ResumeCAOnly/actual gating'}
if($DeepOnly -and ($ResumeCAOnly -or $EnableActualG3D -or $ExtendedScreenOff -or $ParallelOnly)){throw '-DeepOnly is read-only; incompatible with resume, screen OFF or actual gating'}
if($ParallelOnly -and ($ResumeCAOnly -or $EnableActualG3D -or $ExtendedScreenOff)){throw '-ParallelOnly is read-only; cannot request actual GPU, display OFF or CA-only trial'}
if($ExtendedScreenOff -and -not $ExtendedOnly){throw '-ExtendedScreenOff requires -ExtendedOnly'}
Say ('S9 P6 unified COM one-image lab: '+$(if($Execute){'EXECUTE'}else{'PREVIEW ONLY'}))
if($EnableActualG3D){Say 'WARNING: ACTUAL G3D trial requested; runtime confirmation still required'}
else{Say 'G3D uses SHADOW only. Actual gating disabled by default.'}
if(-not $Execute){
    Send 'sysctl kern.hakios.p6_stats'
    Send 'sysctl kern.hakios.g3d_lab_stats'
    if($ExtendedOnly -or $DeepOnly -or $ParallelOnly){
        foreach($node in $(if($DeepOnly -or $ParallelOnly){@('p610_mode','p611_mode','p612_mode')}else{@('p66_mode','p67_mode','p68_mode','p69_mode')})){Send ('sysctl kern.hakios.'+$node)}
    }
    Send 'sysctl kern.hakios.p6_phase=0'
    if(-not $PreflightOnly){if($ParallelOnly){Run-ParallelPhases}elseif($DeepOnly){Run-DeepPhases}elseif($ExtendedOnly){Run-ExtendedPhases}else{Run-Phases}}
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
    if($ExtendedOnly -or $DeepOnly -or $ParallelOnly){
        foreach($node in $(if($DeepOnly -or $ParallelOnly){@('p610_mode','p611_mode','p612_mode')}else{@('p66_mode','p67_mode','p68_mode','p69_mode')})){Send ('sysctl kern.hakios.'+$node)}
    }
    Send 'sysctl kern.hakios.p6_phase=0'
    Say 'PREFLIGHT PASS: exact echo and ACK for P6 sysctls'
    if(-not $PreflightOnly){if($ParallelOnly){Run-ParallelPhases}elseif($DeepOnly){Run-DeepPhases}elseif($ExtendedOnly){Run-ExtendedPhases}else{Run-Phases}}
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
if($finished){Say 'COMPLETE: run tools/s9-p6-report.ps1 -Log <path>';if($DeepOnly -or $ParallelOnly){Say 'DEEP REPORT: tools/s9-p6-deep-report.ps1 -Log <path>'};if($ParallelOnly){Say 'POWER REPORT: tools/s9-p6-parallel-report.ps1 -Log <path>'}}
