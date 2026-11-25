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
    [switch]$InteractiveAB,
    [switch]$WakeCheck,
    [switch]$LegacyFirmware,
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
$script:P12Refusal=$null
$script:P12ReadPaceMs=0
$script:P12ReadAttempts=1
$script:P12Journal=''
$script:P12PreflightVerified=$false
$script:P12CpuDirty=$false
$script:P12WfiDirty=$false
$script:P12HiStopConfirmed=$false
$script:P12HiDirty=$false
# Only the P12.3 wrapper opts in; P12/P12.2 retain the old allowlist.
$script:P123HostEnabled=$false
$script:P124HostEnabled=$false
$script:P125HostEnabled=$false
$script:P126HostEnabled=$false
$script:P127HostEnabled=$false
$script:P124ClockAllowed=$false
$script:P123ClockAllowed=$false
$script:P123Strict=$false
$script:LifeHostOnly=$false
$script:P12HiAbortSeen=$false
$script:P12HiGateStopSeen=$false
$script:P12HiUnexpectedPhase=$false
$script:P12GpuOffBaseline=0L
$script:P12GpuWakeBaseline=0L
# P12.1 conservative start gate, below immutable hard thermal abort thresholds.
$script:P12StartBigC=70
$script:P12StartLittleC=68
$script:P12StartGpuC=68
$script:P12Phase='PREFLIGHT'
$script:P12Results=New-Object 'System.Collections.Generic.List[string]'
$script:P12Phases=New-Object 'System.Collections.Generic.List[object]'
$script:P12CA=@{}
# P12.7.1: accept a complete, typed USB reply following a real command
# echo even if another producer prepended text on the same physical line.
# Never strip arbitrary suffixes or accept a partial numeric/structured value.
function P12-ReplyShape([string]$Name,[string]$Reply,[bool]$Mixed){
    $hex='[0-9a-fA-F]{1,16}'
    $dec='[0-9]{1,20}'
    $fields=@{
        p126_status='mode reason haptic wifi_worker gnss touch touch_leeway shub lease_s watchdog_ms'
        p126_diag='bt_idle bt_busy bt_rx bt_tty_tx_dequeued bt_uart_error abox_quiet abox_busy shub_quiet shub_busy hal_speaker'
        p127_status='mode reason lease_s little_request little_cur ufs_safe ufs_busy floor_hits floor_bypass'
        p127_metrics='usb_stock usb_8ms usb_linkdown usb_bypass usb_other abox_stop abox_resume abox_idle_wait abox_fault psr_mask psr_unmask psr_bypass psr_fault'
        p127_wifi='attach irq watchdog fallback detach'
        p127_bt='attach irq watchdog fallback detach tx_wake'
        p127_ticker='wait due wake'
        p127_fg_seq='seq slots'
    }
    if($Name -match '^(?:p125_(?:mode|select)|p126_mode|p127_mode|p6_usb_(?:ticks|connected_ticks)|p69_usb_disconnected_ticks)$'){
        $n=[uint64]0
        return ($Reply -cmatch "^$dec$" -and [uint64]::TryParse($Reply,[ref]$n))
    }
    if($fields.ContainsKey($Name)){
        $pattern='^version=1'
        foreach($k in $fields[$Name].Split(' ')){$pattern+=" $k=$dec"}
        return ($Reply -cmatch ($pattern+'$'))
    }
    switch -Regex ($Name){
        '^p125_status$' {
            return ($Reply -cmatch "^version=1 active=[0-3] armed=[0-3] start=$dec end=$dec now=$dec hz=$dec reason=$dec down=$dec up=$dec display_on=$dec anchor=$hex slots=512 rows=2 counters=28 kinds=15 cumulative=1(?: page_lookup=scalar)?$")
        }
        '^p125_counts([0-7])$' {
            $cpu=$Matches[1];return ($Reply -cmatch "^version=1 cpu=$cpu counters=$hex(?:,$hex){27}$")
        }
        '^p125_coverage([0-7])$' {
            $cpu=$Matches[1];return ($Reply -cmatch "^version=1 cpu=$cpu pages=$hex(?:,$hex){3} mapdrop=$dec bins=$hex(?:,$hex){4} totals=$hex/$hex(?:,$hex/$hex){13}$")
        }
        '^p127_wfi([0-7])$' {
            $cpu=$Matches[1];return ($Reply -cmatch "^version=1 cpu=$cpu sgi=$hex ppi=$hex spi=$hex bins=$hex/$hex(?:,$hex/$hex){6}$")
        }
        '^p127_fg([0-9]|1[0-5])$' {
            $page=$Matches[1];return ($Reply -cmatch "^version=1 page=$page(?: r=$dec/$dec/-?$dec/-?$dec/$dec/[01]/$dec/$dec){0,4}$")
        }
        '^p127_bus$' {return ($Reply -cmatch "^version=1 at=$dec valid=$hex mif=$dec int=$dec intcam=$dec cam=$dec disp=$dec aud=$dec$")}
        '^p127_shub$' {
            $pattern="^version=1 enabled=$hex"
            foreach($i in @(0,1,4,5,7,9)){$pattern+=" s$i=$dec/$dec"}
            return ($Reply -cmatch ($pattern+'$'))
        }
        '^p125_page$' {return ($Reply -cmatch "^version=1 page=$dec(?: r=$hex(?:/$hex){11}/[a-zA-Z0-9_.-]+){1,2}$")}
        '^g3d_lab_stats$' {
            # Older firmware may use a different field set; require typed
            # key=value tokens only for the mixed-line recovery case.
            if($Mixed){return ($Reply -cmatch '^[a-zA-Z_][a-zA-Z0-9_]*=-?[0-9]+(?: [a-zA-Z_][a-zA-Z0-9_]*=-?[0-9]+)+$')}
        }
    }
    # Preserve legacy parser compatibility for unmixed older sysctls.
    return (-not $Mixed -and $Reply.Length -gt 0)
}
$script:LifeLineObserver={
    param([string]$Line,[datetime]$At)
    if($script:P12Command){
        $echo=[regex]::Escape('usb> '+$script:P12Command)
        if($Line -cmatch "(?:^|(?<=\s))$echo\s*$"){
            $script:P12Echo=$true
        }
        if($script:P12Echo){
            $ref=[regex]::Escape('usb: '+$script:P12Command+':')
            if($Line -cmatch "(?:^|(?<=\s))$ref(?<tail>.*)$" -and $Matches['tail'] -cnotmatch '^ set \(0\)\s*$'){
                $script:P12Refusal=$Line
            }
            $expected=$script:P12Command.Substring(('sysctl kern.hakios.').Length).Split('=')[0]
            $prefix=[regex]::Escape('usb: kern.hakios.'+$expected+' = ')
            if($Line -cmatch "(?:^|(?<=\s))$prefix(?<reply>.*)$"){
                $value=$Matches['reply'].Trim()
                $mixed=-not $Line.StartsWith('usb: kern.hakios.'+$expected+' = ',[StringComparison]::Ordinal)
                # Multiple USB tokens on a line are ambiguous.
                if(([regex]::Matches($Line,'(?:^|(?<=\s))usb[:>]')).Count -eq 1 -and (P12-ReplyShape $expected $value $mixed)){
                    if($null -ne $script:P12Reply -and $script:P12Reply -cne $value){
                        $script:P12Refusal='Conflicting duplicate sysctl replies: '+$expected
                    }else{
                        $script:P12Reply=$value
                        if($mixed){Life-Say "P12 MIXED_REPLY_RECOVERED $expected"}
                    }
                }
            }
        }
    }
    if($script:LifeSynced -and $Line -match '^hi: auto=ABORT .+reason=manual_usb_control'){
        $script:P12HiAbortSeen=$true
    }
    if($script:LifeSynced -and $Line -match '^hi: gate=usb-stop'){
        $script:P12HiGateStopSeen=$true
    }
    if($script:P12HiStopConfirmed -and $script:P12Phase -in @('A0','B','A1') -and
       $Line -match '^hi: auto=(baseline|cut1|recover1|cut2|recover2)'){
        $script:P12HiUnexpectedPhase=$true
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
function P12-Query([string]$Name,[string]$Value='',[switch]$Cleanup){
    $allowed=@('hi_cmd','p12_cpu_mode','p12_cpu_stats','p12_wfi_probe','p12_wfi_stats','p12_idle_diag',
        'p12_gpu_boot_stats','p9_auto_gpu','p9_panel_stats','g3d_lab_stats',
        'p122_diag_mode','p122_gpu_stats',
        'p7_cpu_stats','p4_stats')
    if($script:P123HostEnabled){$allowed+=@('p123_floor_mode','p123_floor_stats','p1231_hash_mode','p1231_ufs_stats')}
    if($script:P124HostEnabled){$allowed+=@('p124_cpu_mode','p124_cpu_stats')+(0..7|ForEach-Object {"p124_irq$_";"p124_timer$_"})}
    if($script:P125HostEnabled){$allowed+=@('p125_mode','p125_status','p125_select','p125_page','p6_usb_ticks','p6_usb_connected_ticks','p69_usb_disconnected_ticks')+(0..7|ForEach-Object {"p125_counts$_";"p125_coverage$_"})}
    if($script:P126HostEnabled){$allowed+=@('p126_mode','p126_status','p126_diag')}
    if($script:P127HostEnabled){$allowed+=@('p127_mode','p127_status','p127_metrics','p127_wifi','p127_bt','p127_ticker','p127_bus','p127_shub','p127_fg_seq')+(0..7|ForEach-Object {"p127_wfi$_"})+(0..15|ForEach-Object {"p127_fg$_"})}
    if($Name -notin $allowed){throw "P12 sysctl disallowed $Name"}
    if($script:P124HostEnabled -and $Name -eq 'p1231_hash_mode' -and $Value -eq '1'){throw 'P12.4 rejects scalar thermal trial'}
    if($Value -ne '' -and -not (
        (($Name -eq 'p12_cpu_mode' -or $Name -eq 'p12_wfi_probe') -and $Value -in @('0','1')) -or
        ($Name -eq 'hi_cmd' -and $Value -in @('0','201')) -or
        ($Name -eq 'p122_diag_mode' -and $Value -in @('0','1','2')) -or
        ($script:P123HostEnabled -and $Name -eq 'p123_floor_mode' -and $Value -in @('0','1')) -or
        ($script:P123HostEnabled -and $Name -eq 'p1231_hash_mode' -and $Value -in @('0','1','2')) -or
        ($script:P123HostEnabled -and $script:P123ClockAllowed -and $Name -eq 'hi_cmd' -and $Value -eq '252') -or
        ($script:P127HostEnabled -and $Name -eq 'p127_mode' -and $Value -in @('0','1','3','5','9','17','33','49','65','81','129','257','513')) -or
        ($script:P126HostEnabled -and $Name -eq 'p126_mode' -and $Value -match '^\d{1,2}$' -and [int]$Value -le 63) -or
        ($script:P125HostEnabled -and $Name -eq 'p125_mode' -and $Value -in @('0','1','2','3')) -or
        ($script:P125HostEnabled -and $Name -eq 'p125_select' -and $Value -match '^\d{1,4}$' -and [int]$Value -lt 2048) -or
        ($script:P124HostEnabled -and $Name -eq 'p124_cpu_mode' -and $Value -in @('0','1','2','3','4')) -or
        ($script:P124HostEnabled -and $script:P124ClockAllowed -and $Name -eq 'hi_cmd' -and $Value -eq '253'))){
        throw 'P12.2 only permits bounded CPU/WFI/HI2 and diagnostic mode=0/1/2; no GPU/panel/voltage writes'
    }
    $restore=@('p12_cpu_mode','p12_wfi_probe','hi_cmd','p122_diag_mode')
    if($script:P123HostEnabled){$restore+=@('p123_floor_mode','p1231_hash_mode')}
    if($script:P124HostEnabled){$restore+='p124_cpu_mode'}
    if($script:P125HostEnabled){$restore+='p125_mode'}
    if($script:P126HostEnabled){$restore+='p126_mode'}
    if($script:P127HostEnabled){$restore+='p127_mode'}
    $clockStop=(($script:P123ClockAllowed -or $script:P124ClockAllowed) -and $script:P123HostEnabled -and $Name -eq 'hi_cmd' -and $Value -eq '201')
    if($Cleanup -and -not $clockStop -and ($Name -notin $restore -or $Value -ne '0')){
        throw 'P12.1/P12.2 Cleanup bypass is reserved for bounded restore-to-zero writes only'
    }
    $cmd='sysctl kern.hakios.'+$Name
    if($Value -ne ''){$cmd+='='+$Value}
    # Mark a failed/uncertain ENABLE write dirty before sending it, so
    # a thermal abort cannot skip its bounded restore-to-zero attempt.
    if($Name -eq 'p12_cpu_mode' -and $Value -eq '1'){$script:P12CpuDirty=$true}
    if($Name -eq 'p12_wfi_probe' -and $Value -eq '1'){$script:P12WfiDirty=$true}
    if($Name -eq 'hi_cmd' -and $Value -in @('201','252','253')){$script:P12HiDirty=$true}
    $bytes=[Text.Encoding]::ASCII.GetBytes($cmd+[char]13)
    $attempts=if($Value -eq '' -and -not $Cleanup){$script:P12ReadAttempts}else{1}
    for($attempt=1;$attempt -le $attempts;$attempt++){
        $script:P12Command=''
        # Drain briefly before retry so late data cannot satisfy a new echo.
        # Read-only retries only: never retransmit a policy or cursor write.
        $pace=if($attempt -gt 1){300}else{$script:P12ReadPaceMs}
        if($pace -gt 0 -and -not $Cleanup){
            $until=(Life-Now).AddMilliseconds($pace)
            while((Life-Now) -lt $until){Life-Pump;Life-CheckSafety (Life-Now)}
        }
        $script:P12Reply=$null;$script:P12Refusal=$null;$script:P12Echo=$false
        $script:P12Command=$cmd
        $script:LifeSerial.Write($bytes,0,$bytes.Length)
        Life-Say ('P12 '+$(if($Value -eq ''){'READ '}else{'WRITE '})+$cmd)
        $deadline=(Life-Now).AddSeconds(12)
        while((Life-Now) -lt $deadline -and $null -eq $script:P12Reply -and $null -eq $script:P12Refusal){
            Life-Pump
            # During cleanup, do not mask the restore ACK with old heat.
            if(-not $Cleanup){Life-CheckSafety (Life-Now)}
        }
        if($null -ne $script:P12Refusal){$reason=$script:P12Refusal;$script:P12Command='';throw "P12 sysctl rejected: $reason"}
        if($null -ne $script:P12Reply){break}
        $script:P12Command=''
        if($attempt -lt $attempts){Life-Say "P12 READ_RETRY $Name attempt=$($attempt+1)/$attempts"}
    }
    if($null -eq $script:P12Reply){$script:P12Command='';throw "Missing echo and sysctl readback $cmd"}
    $reply=$script:P12Reply
    $record=('{0:HH:mm:ss.fff}Z {1} {2}={3}' -f (Life-Now),$script:P12Phase,$Name,$reply)
    $script:P12Results.Add($record)
    if($script:P12Journal){Add-Content -Encoding UTF8 -LiteralPath $script:P12Journal -Value $record}
    $script:P12Command=''
    $script:P12Echo=$false
    if($Value -ne '' -and $reply -ne $Value){throw "P12 readback mismatch for $Name, got $reply"}
    if($Name -eq 'p12_cpu_mode' -and $Value -eq '0'){$script:P12CpuDirty=$false}
    if($Name -eq 'p12_wfi_probe' -and $Value -eq '0'){$script:P12WfiDirty=$false}
    if($Name -eq 'hi_cmd' -and $Value -eq '0'){$script:P12HiDirty=$false}
    return $reply
}

# P12.1 fail-closed cool-start gate: live-only thermal telemetry.
function P12-CoolEnough([double]$Big,[double]$Little,[double]$Gpu){
    return ($Big -le $script:P12StartBigC -and $Little -le $script:P12StartLittleC -and
        $Gpu -le $script:P12StartGpuC)
}
function P12-CoolStart([int]$Seconds=12){
    $script:P12Phase='COOL_PREFLIGHT'
    $before=$script:LifeSamples.Count
    Life-Say "P12.1 COOL_PREFLIGHT $Seconds seconds: NO SWIPES. Need fresh CPU/GPU and BIG <= $($script:P12StartBigC)C LITTLE <= $($script:P12StartLittleC)C G3D <= $($script:P12StartGpuC)C; hard thermal stops still enforced."
    $until=(Life-Now).AddSeconds($Seconds)
    while((Life-Now) -lt $until){
        Life-Pump
        $now=Life-Now
        Life-CheckSafety $now
        P12-EarlyThermalGuard
        if($script:LifeState -ne 'ON'){
            throw 'P12.1 cool preflight: screen is not ON; refuse active A/B. Leave screen OFF for cooling.'
        }
    }
    $cpu=@($script:LifeSamples | Select-Object -Skip $before | Where-Object {$_.type -eq 'CPU'})
    $gpu=@($script:LifeSamples | Select-Object -Skip $before | Where-Object {
        $_.gpu_temp_c -ne '' -and $_.display -eq 'ON'
    })
    $now=Life-Now
    if($cpu.Count -lt 3 -or $gpu.Count -lt 2 -or
       $script:LifeLastCpuAt -eq [datetime]::MinValue -or
       $script:LifeLastGpuAt -eq [datetime]::MinValue -or
       ($now-$script:LifeLastCpuAt).TotalSeconds -gt 4 -or
       ($now-$script:LifeLastGpuAt).TotalSeconds -gt 8){
        throw "P12.1 preflight lacks fresh thermal telemetry: cpu_n=$($cpu.Count) gpu_n=$($gpu.Count); refuse interaction"
    }
    $big=[double]$script:LifeLastBig
    $little=[double]$script:LifeLastLittle
    $g3d=[double]$script:LifeLastGpuC
    if(-not (P12-CoolEnough $big $little $g3d)){
        throw "P12.1 COOL_START_REFUSED BIG=$big LITTLE=$little G3D=$g3d. No swipes: turn screen OFF/cool; do NOT raise thresholds."
    }
    Life-Say "P12.1 COOL_START_PASS fresh cpu=$($cpu.Count) gpu=$($gpu.Count) BIG=$big LITTLE=$little G3D=$g3d"
}
function P12-EarlyThermalGuard {
    $now=Life-Now
    if(($now-$script:LifeLastCpuAt).TotalSeconds -lt 4 -and
       $null -ne $script:LifeLastBig -and $null -ne $script:LifeLastLittle){
        if($script:LifeLastBig -ge 76 -or $script:LifeLastLittle -ge 70){
            throw "P12.1 EARLY_THERMAL_STOP BIG=$($script:LifeLastBig)C LITTLE=$($script:LifeLastLittle)C (hard stops unmodified)"
        }
    }
    if(($now-$script:LifeLastGpuAt).TotalSeconds -lt 8 -and
       $null -ne $script:LifeLastGpuC -and $script:LifeLastGpuC -ge 72){
        throw "P12.1 EARLY_THERMAL_STOP G3D=$($script:LifeLastGpuC)C (hard stop unmodified)"
    }
}
function P12-QuiesceHI2 {
    # hi_cmd=201 is an existing SpringBoard manual STOP, NOT a
    # frame skip. It cancels hi=2 crossover and clears its gated Clock.
    $script:P12Phase='QUIESCE_HI2'
    $script:P12HiGateStopSeen=$false
    # Repeated launches in one boot may find mailbox=201 already.
    # Toggle to 0 before 201 so the SpringBoard one-second poll sees a
    # NEW command and emits a real gate=usb-stop acknowledgement.
    if((P12-Query 'hi_cmd') -eq '201'){
        [void](P12-Query 'hi_cmd' '0')
        $pause=(Life-Now).AddSeconds(2)
        while((Life-Now) -lt $pause){Life-Pump;Life-CheckSafety (Life-Now)}
    }
    [void](P12-Query 'hi_cmd' '201')
    $until=(Life-Now).AddSeconds(7)
    while((Life-Now) -lt $until -and -not $script:P12HiGateStopSeen){
        Life-Pump
        Life-CheckSafety (Life-Now)
    }
    if(-not $script:P12HiGateStopSeen){
        throw 'P12.1 hi_cmd=201 readback but NO SpringBoard gate=usb-stop ACK. Refuse A/B until HI2 interference is proven stopped.'
    }
    $script:P12HiStopConfirmed=$true
    Life-Say 'P12.1 HI2_QUIESCED gate=usb-stop ACK: autonomous cut/recover suppressed until manual rearm (hi_cmd=252) or reboot'
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
        P12-EarlyThermalGuard
        if($script:P123Strict){P123-Guard}
        if($script:P12HiUnexpectedPhase){
            throw 'P12.1 HI2_AUTO_REARMED during interactive A/B: abort invalid causal comparison'
        }
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
    $clock=@($sample | Where-Object {$_.gpu_clock_mhz -ne ''} | ForEach-Object {[double]$_.gpu_clock_mhz})
    $busy=@($sample | Where-Object {$_.gpu_busy_pct -ne ''} | ForEach-Object {[double]$_.gpu_busy_pct})
    $clockMean='n/a';$busyMean='n/a';$clockMax='n/a'
    if($clock.Count){$clockMean=[Math]::Round(($clock | Measure-Object -Average).Average,1);$clockMax=($clock | Measure-Object -Maximum).Maximum}
    if($busy.Count){$busyMean=[Math]::Round(($busy | Measure-Object -Average).Average,1)}
    $stat=[pscustomobject]@{Name=$Phase;Window=$Seconds;Cpu=$cpu.Count;Fps=$frames.Count;BigMean=$big;BigPeak=$peak;Presented=$presented;Refresh=$refresh;Missed=$missed;GpuClkN=$clock.Count;GpuClkMean=$clockMean;GpuClkMax=$clockMax;GpuBusyN=$busy.Count;GpuBusyMean=$busyMean}
    $script:P12Phases.Add($stat)
    Life-Say ("P12 $Phase ON cpu_n=$($stat.Cpu) fps_n=$($stat.Fps) BIG_mean=$big BIG_peak=$peak frames=$presented/$refresh missed=$missed G3D_clk_n=$($clock.Count) G3D_clk_avg_MHz=$clockMean G3D_busy_n=$($busy.Count) G3D_busy_avg_pct=$busyMean")
}
function P12-ObserveIdle{
    $script:P12Phase='NATURAL_OFF'
    Life-Say "P12.1 IDLE $IdleWindowSeconds seconds: manually press POWER ONCE to turn screen OFF (auto-lock disabled). NO SWIPES. After >=60s OFF, optional -WakeCheck: press POWER once to wake."
    $beg=Life-Now
    $lastProbe=$beg
    $offStableSeen=$false
    $offGpuConfirmed=$false
    $wakeChecked=$false
    while(((Life-Now)-$beg).TotalSeconds -lt $IdleWindowSeconds){
        Life-Pump
        $now=Life-Now
        Life-CheckStable $now
        Life-CheckSafety $now
        if($script:LifeState -eq 'OFF' -and ($now-$script:LifeStateSince).TotalSeconds -ge $OffStableSeconds){
            if(-not $offStableSeen -and $script:P124HostEnabled -and $WfiProbe){P124-IrqSnapshot 'IRQ_OFF_STABLE_BEGIN'}
            $offStableSeen=$true
            if(($now-$lastProbe).TotalSeconds -ge 20){
                $lastProbe=$now
                $offStats=P12-Query 'g3d_lab_stats'
                $newOff=0L
                if($offStats -match '\boff_count=(\d+)'){$newOff=[long]$Matches[1]}
                if($offStats -match 'domain=0 pll=0' -and $offStats -match 'off_total_ms=([1-9][0-9]*)' -and
                   $newOff -gt $script:P12GpuOffBaseline -and $offStats -match '\bfault=0(?:\s|$)'){
                    $offGpuConfirmed=$true
                }
                [void](P12-Query 'p9_auto_gpu')
                if($WfiProbe){
                    [void](P12-Query 'p12_wfi_stats')
                    if(-not $LegacyFirmware){[void](P12-Query 'p12_idle_diag')}
                    if($script:P124HostEnabled){P124-IrqSnapshot 'IRQ_OFF_STABLE_PERIODIC'}
                }
            }
        }
        if($WakeCheck -and $offStableSeen -and $script:LifeState -eq 'ON' -and -not $wakeChecked){
            # Human physical button supplies the wake source; this script
            # NEVER simulates wake or writes the G3D PMU/PLL.
            $prevOff=@($script:LifeSegments | Where-Object {$_.State -eq 'OFF' -and $null -ne $_.End} | Select-Object -Last 1)
            if($prevOff.Count -ne 1 -or ($prevOff[0].End-$prevOff[0].Start).TotalSeconds -lt 60 -or -not $offGpuConfirmed){
                throw 'P12.1 WAKE_REFUSED: need >=60s physical screen OFF AND actual G3D domain=0 pll=0 off_total_ms>0 before wake measurement'
            }
            $script:P12Phase='MANUAL_WAKE_VERIFY'
            $afterWake=(Life-Now).AddSeconds(3)
            while((Life-Now) -lt $afterWake){Life-Pump;Life-CheckSafety (Life-Now)}
            $stats=P12-Query 'g3d_lab_stats'
            $wakes=0L
            if($stats -match '\bwake_count=(\d+)'){$wakes=[long]$Matches[1]}
            if($stats -notmatch 'domain=1 pll=1' -or $wakes -le $script:P12GpuWakeBaseline -or $stats -notmatch '\bwake_fail=0(?:\s|$)'){
                throw "P12.1 GPU manual wake not verified; PMU/PLL or new wake counter not advanced (baseline=$($script:P12GpuWakeBaseline)): $stats"
            }
            $wakeChecked=$true
            Life-Say 'P12.1 MANUAL_WAKE VERIFIED: GPU PMU+PLL ON, wake_count increased, wake_fail=0'
            $script:P12Phase='NATURAL_OFF'
        }
    }
    if(-not $offStableSeen){throw 'P12.1 NO_OFF_STABLE: manual power button OFF was not observed; no idle claim'}
    if($WakeCheck -and -not $wakeChecked){throw 'P12.1 NO_WAKE: -WakeCheck requested but >=60s physical OFF -> GPU domain/pll OFF -> manual wake could not be verified'}
}
function P12-Report([string]$Stem,[string]$Reason){
    Life-Report $Stem (Life-Now) $Reason
    $out=New-Object 'System.Collections.Generic.List[string]'
    $out.Add('P12.1 SAFE dual source capture: PASSIVE by default; interactive only by explicit opt-in after cool-start.')
    $out.Add('HI2 manual stop mailbox=201 is used ONLY for -InteractiveAB; default passive capture leaves HI2 unchanged. A stopped HI2 crossover remains aborted until explicit rearm/reboot.')
    $out.Add('HI2 quiesce confirmed='+$script:P12HiStopConfirmed+'; unexpected_auto_phase='+$script:P12HiUnexpectedPhase+'; interactive_requested='+[bool]$InteractiveAB+'; wake_requested='+[bool]$WakeCheck+'; legacy_firmware='+[bool]$LegacyFirmware)
    $out.Add('G3D baseline off_count='+$script:P12GpuOffBaseline+' wake_count='+$script:P12GpuWakeBaseline+'; -WakeCheck requires fresh increments, PMU/PLL OFF then ON')
    $out.Add('P12 integrated thermal/idle hardware A/B - DO NOT CLAIM POWER OR FPS IMPROVEMENT WITHOUT SAME-GESTURE COMPARISON')
    $out.Add('P12 legacy A0/B/A1 CPU no-touch. P12.4 optional floor/boost trials explicitly labeled; stock load target and thermal ceilings retained.')
    $out.Add('P12.1 WFI observation also records per-core idle entry/denied/timer/WFE/no-WFI/exit; NOT actual C2/CPD/SICD or system suspend.')
    $out.Add('CPU frame mode0/1 needs verified sysctl readback; WFI observation is NOT actual C2/CPD/SICD or system suspend.')
    $out.Add('GPU boot gate uses P9 controls unchanged, verified via independent g3d_lab_stats PMU/PLL and off_total_ms.')
    $out.Add('No CPU_OFF, no forced Lock Screen, no panel DCS sleep, no voltage commands.')
    $out.Add('STOP='+$Reason)
    foreach($p in $script:P12Phases){
        $out.Add("PHASE $($p.Name) ON_cpu_n=$($p.Cpu) ON_fps_n=$($p.Fps) BIG_mean=$($p.BigMean) BIG_peak=$($p.BigPeak) presented=$($p.Presented) refresh=$($p.Refresh) missed=$($p.Missed) gpu_clk_n=$($p.GpuClkN) gpu_clk_MHz=$($p.GpuClkMean) gpu_clk_max_MHz=$($p.GpuClkMax) gpu_busy_n=$($p.GpuBusyN) gpu_busy_pct=$($p.GpuBusyMean)")
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
        Write-Host 'P12.1 PREVIEW: SAFE PASSIVE by default. Cool-start, preserve existing HI2 policy, observe ON without swipes, then manual power-button OFF.'
        Write-Host "ON window $ActiveWindowSeconds s; OFF observation $IdleWindowSeconds s. NO forced screen ON."
        Write-Host 'Keep PuTTY CLOSED. NO TOUCH / SWIPES in default mode. -InteractiveAB is opt-in ONLY after naturally cool baseline; early thermal guard aborts.'
        Write-Host 'Only -InteractiveAB writes hi_cmd=201 to stop legacy HI2 crossover (verified SpringBoard ACK); passive default leaves HI2 untouched.'
        Write-Host 'P12 bounded sysctl writes: hi_cmd=201/0 and CPU mode0/1 ONLY with -InteractiveAB; WFI0/1 only if requested, plus restoring inherited nonzero modes.'
        Write-Host 'Optional -WakeCheck: after OFF>=25s manually press power button once, then read GPU domain/PLL and wake counters.'
        Write-Host 'P12 NEVER writes GPU/panel/CPU_OFF/voltage policies or forces a wake.'
        Write-Host 'On old P12 kernel g197d148, use -LegacyFirmware to skip NEW p12_idle_diag; full idle-path counters require matched new kernel.'
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
        $priorCpu=P12-Query 'p12_cpu_stats'
        if($priorCpu -notmatch '^mode=[01] '){throw 'Wrong kernel: P12 sysctl not recognized'}
        $script:P12PreflightVerified=$true
        if($priorCpu -match '^mode=1 '){
            $script:P12CpuDirty=$true
            [void](P12-Query 'p12_cpu_mode' '0' -Cleanup)
            Life-Say 'P12.1 recovered inherited CPU trial mode1 to 0 before ANY heat experiment'
        }
        [void](P12-Query 'p12_gpu_boot_stats')
        [void](P12-Query 'p9_auto_gpu')
        $gpuStart=P12-Query 'g3d_lab_stats'
        if($gpuStart -match '\boff_count=(\d+)'){$script:P12GpuOffBaseline=[long]$Matches[1]}
        if($gpuStart -match '\bwake_count=(\d+)'){$script:P12GpuWakeBaseline=[long]$Matches[1]}
        [void](P12-Query 'p9_panel_stats')
        $priorWfi=P12-Query 'p12_wfi_stats'
        if($priorWfi -match '^enabled=1 '){
            $script:P12WfiDirty=$true
            [void](P12-Query 'p12_wfi_probe' '0' -Cleanup)
            Life-Say 'P12.1 recovered inherited WFI probe1 to 0 before preflight'
        }
        if(-not $LegacyFirmware){[void](P12-Query 'p12_idle_diag')}
        [void](P12-Query 'p7_cpu_stats')
        # A0/B/A1 are never run on an already hot boot. Live, fresh,
        # threshold-gated preflight precedes ALL performance policy writes.
        P12-CoolStart 12
        if($InteractiveAB){
            # Quiesce legacy HI2 ONLY during causal A/B. Passive capture
            # must leave the production timer/clock policy unchanged.
            P12-QuiesceHI2
            P12-EarlyThermalGuard
            [void](P12-Query 'p12_cpu_mode' '0')
            P12-Observe 'A0' $ActiveWindowSeconds 'STOCK CPU target; natural modest interaction ONLY if still cool'
            [void](P12-Query 'p12_cpu_stats')
            P12-EarlyThermalGuard
            [void](P12-Query 'p12_cpu_mode' '1')
            P12-Observe 'B' $ActiveWindowSeconds 'P12 mode1; original touch boost and frame deadlines intact'
            [void](P12-Query 'p12_cpu_stats')
            [void](P12-Query 'p12_cpu_mode' '0')
            P12-Observe 'A1' $ActiveWindowSeconds 'STOCK restored; matched minimal interaction'
            [void](P12-Query 'p12_cpu_stats')
        }else{
            P12-Observe 'PASSIVE_ON' $ActiveWindowSeconds 'NO swipes, no touches; collect natural render/thermal baseline'
        }
        if($WfiProbe){[void](P12-Query 'p12_wfi_probe' '1')}
        P12-ObserveIdle
        if($WfiProbe){
            [void](P12-Query 'p12_wfi_stats')
            if(-not $LegacyFirmware){[void](P12-Query 'p12_idle_diag')}
        }
        [void](P12-Query 'g3d_lab_stats')
        [void](P12-Query 'p12_gpu_boot_stats')
        Life-Say 'P12 capture complete; restoring all modified CPU observational policies'
    }catch{
        $errorText=$_.Exception.Message
        Life-Say ('P12 STOPPED: '+$errorText)
    }finally{
        if($script:P12PreflightVerified -and $null -ne $script:LifeSerial -and $script:LifeSerial.IsOpen){
            # Bounded restores must not be blocked by the SAME hot sample
            # that correctly caused the experiment to abort. Only write a
            # control if this run attempted to change it.
            if($script:P12CpuDirty){
                try{[void](P12-Query 'p12_cpu_mode' '0' -Cleanup);Life-Say 'P12 CPU mode0 RESTORE ACK confirmed'}
                catch{Life-Say ('P12 CPU mode0 restore UNSURE: '+$_.Exception.Message)}
            }
            if($script:P12WfiDirty){
                try{[void](P12-Query 'p12_wfi_probe' '0' -Cleanup);Life-Say 'P12 WFI probe0 RESTORE ACK confirmed'}
                catch{Life-Say ('P12 WFI probe0 restore UNSURE: '+$_.Exception.Message)}
            }
            if($script:P12HiDirty){
                try{[void](P12-Query 'hi_cmd' '0' -Cleanup);Life-Say 'P12 HI2 mailbox0 ACK; autonomous crossover remains canceled until explicit rearm/reboot'}
                catch{Life-Say ('P12 HI2 mailbox restore UNSURE: '+$_.Exception.Message)}
            }
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
