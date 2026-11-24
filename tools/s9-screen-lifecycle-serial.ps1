#requires -Version 5.1
<#
.SYNOPSIS
  S9 P11: passive natural display lifecycle and power-source capture.
.DESCRIPTION
  Captures ON -> iOS auto-OFF -> OFF-STABLE -> user wake, without forcing
  an artificial 18s Lock Screen. Uses the P9/P10 image already flashed.
  One READ-ONLY sysctl anchors the live USB stream after backlog drains.
  NO sysctl writes, button injection, fake frames, GPU/CPU/voltage changes.
  Never equate fuel-gauge current with external laboratory power.
.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File .\s9-screen-lifecycle-serial.ps1 -Port COM125 -DurationSeconds 180 -Execute
#>
[CmdletBinding()]
param(
    [ValidatePattern('^COM[0-9]+$')][string]$Port='COM125',
    [ValidateRange(1200,3000000)][int]$BaudRate=115200,
    [ValidateRange(60,1800)][int]$DurationSeconds=180,
    [ValidateRange(5,120)][int]$OffStableSeconds=25,
    [ValidateRange(75,85)][int]$BigStopC=82,
    [ValidateRange(70,80)][int]$LittleStopC=75,
    [ValidateRange(70,85)][int]$G3DStopC=77,
    [ValidateRange(43,55)][int]$BatteryStopC=47,
    [ValidateRange(3,45)][int]$AckTimeoutSeconds=24,
    [string]$LogPath='',
    [switch]$Execute,
    [switch]$LibraryOnly
)
Set-StrictMode -Version 2
$ErrorActionPreference='Stop'
$script:LifeState='UNKNOWN'
$script:LifeScene='UNKNOWN'
$script:LifeStart=[datetime]::MinValue
$script:LifeStateSince=[datetime]::MinValue
$script:LifeOffStableAnnounced=$false
$script:LifeSynced=$false
$script:LifeFenceSent=$false
$script:LifeFenceEcho=$false
$script:LifeInitialized=$false
$script:LifeLastCpuAt=[datetime]::MinValue
$script:LifeLastBig=$null
$script:LifeLastLittle=$null
$script:LifeLastGpuC=$null
$script:LifeLastGpuAt=[datetime]::MinValue
$script:LifeBatteryC=$null
$script:LifeSourcePolicy=$null
$script:LifeLatestLockProof=$null
$script:LifeSegments=New-Object 'System.Collections.Generic.List[object]'
$script:LifeTransitions=New-Object 'System.Collections.Generic.List[object]'
$script:LifeSamples=New-Object 'System.Collections.Generic.List[object]'
$script:LifeEvents=New-Object 'System.Collections.Generic.List[string]'
$script:LifeBuckets=@{}
$script:LifeSerial=$null
$script:LifeRaw=$null
$script:LifePartial=''
$script:LifeBuffer=New-Object byte[] 32768
$script:LifeBytes=0L
$script:LifeLogFinal=''
$script:LifeCurrentSegment=$null
$script:LifeOnToOff=0
$script:LifeOffToOn=0
$script:LifeCompleteCycles=0
$script:LifeHI2CutPhases=0
$script:LifeUnexpectedPanelDCS=0
$script:LifeFatal=''
$script:LifeHostOnly=$true
# P12 optional line observer for a standalone dual-source controller.
# Unset in original P11; no extra serial commands or power writes.
$script:LifeLineObserver=$null

function Life-Now { return [datetime]::UtcNow }
function Life-Say([string]$Message) {
    $record=('{0:yyyy-MM-ddTHH:mm:ss.fffZ} {1}' -f (Life-Now),$Message)
    $script:LifeEvents.Add($record)
    Write-Host ('[{0:HH:mm:ss}] {1}' -f (Get-Date),$Message)
}
function Life-New-Bucket {
    return [pscustomobject]@{
        CpuCount=0;BigSum=0.0;LittleSum=0.0;BigPeak=$null;LittlePeak=$null
        CpuBigLoadSum=0.0;CpuLittleLoadSum=0.0
        CpuBigMHzSum=0.0;CpuLittleMHzSum=0.0;CpuBigOnlineMin=$null;CpuLittleOnlineMin=$null
        GpuCount=0;GpuOff=0;PllOff=0;GpuClkSum=0.0;GpuBusyCount=0;GpuBusySum=0.0
        GpuFrameAgeSum=0.0;GpuJobAgeSum=0.0;GpuAgesCount=0
        GpuThermalCount=0;GpuTempSum=0.0;GpuTempPeak=$null
        FuelCount=0;FuelSignedSum=0.0;DischargeCount=0;DischargeSum=0.0
        FpsCount=0;Presented=0L;Refresh=0L;Missed=0L;GapMax=0.0
        TopCount=0;BackboardSum=0.0;SpringBoardSum=0.0;BackboardCount=0;SpringBoardCount=0
        LocationdSum=0.0;LocationdCount=0;WifidSum=0.0;WifidCount=0;GnssdSum=0.0;GnssdCount=0
        PosterCount=0;PosterSum=0.0;PosterPeakRSSKiB=$null
        SensorLogLines=0;WifiLogLines=0;LocationLogLines=0
        CoverWindowCount=0;LockProven=0
        ClockCount=0;P10ClockCut=0L;AnimCount=0;P10AnimCut=0L
    }
}
foreach($key in @('ON_HOME','ON_NOT_HOME','ON_UNKNOWN','OFF_EARLY','OFF_STABLE')){
    $script:LifeBuckets[$key]=Life-New-Bucket
}
function Life-Bucket([datetime]$At) {
    if($script:LifeState -eq 'OFF'){
        if(($At-$script:LifeStateSince).TotalSeconds -ge $OffStableSeconds){return 'OFF_STABLE'}
        return 'OFF_EARLY'
    }
    if($script:LifeState -ne 'ON'){return ''}
    if($script:LifeScene -eq 'HOME'){return 'ON_HOME'}
    if($script:LifeScene -eq 'NOT_HOME'){return 'ON_NOT_HOME'}
    return 'ON_UNKNOWN'
}
function Life-AddSample([string]$Kind,[datetime]$At,[hashtable]$Data) {
    if(-not $script:LifeSynced){return}
    $bucket=Life-Bucket $At
    $elapsed=[Math]::Round(($At-$script:LifeStart).TotalSeconds,2)
    $row=[ordered]@{
        utc=$At.ToString('yyyy-MM-ddTHH:mm:ss.fffZ')
        elapsed_s=$elapsed;display=$script:LifeState;bucket=$bucket
        scene=$script:LifeScene;type=$Kind
        big_c='';little_c='';big_load_pct='';little_load_pct=''
        big_mhz='';little_mhz='';big_online='';little_online=''
        gpu_domain='';gpu_pll='';gpu_clock_mhz='';gpu_busy_pct='';gpu_temp_c=''
        gpu_frame_age_ms='';gpu_job_age_ms=''
        fg_signed_ma='';fg_avg_ma='';fps_presented='';fps_refresh='';fps_missed=''
        backboard_pct='';springboard_pct='';locationd_pct='';wifid_pct='';gnssd_pct=''
        poster_cpu_pct='';poster_rss_kib=''
        lock_display='';cover_locked='';lock_stable_ms='';p10_clock_cut='';p10_anim_cut=''
    }
    foreach($k in $Data.Keys){if(-not $row.Contains($k)){throw "Unknown lifecycle sample column $k"};$row[$k]=$Data[$k]}
    $script:LifeSamples.Add([pscustomobject]$row)
}
function Life-SetState([string]$NewState,[string]$Reason,[datetime]$At) {
    if(-not $script:LifeSynced){return}
    if($NewState -notin @('ON','OFF')){throw "Invalid display state $NewState"}
    if($script:LifeState -eq $NewState){return}
    $old=$script:LifeState
    if($null -ne $script:LifeCurrentSegment){
        $script:LifeCurrentSegment.End=$At
        if($old -eq 'OFF' -and $NewState -eq 'ON' -and
           ($At-$script:LifeStateSince).TotalSeconds -ge $OffStableSeconds){
            $script:LifeCompleteCycles++
        }
    }
    if($old -eq 'ON' -and $NewState -eq 'OFF'){$script:LifeOnToOff++}
    if($old -eq 'OFF' -and $NewState -eq 'ON'){$script:LifeOffToOn++}
    $script:LifeState=$NewState
    $script:LifeStateSince=$At
    $script:LifeOffStableAnnounced=$false
    $segment=[pscustomobject]@{Index=($script:LifeSegments.Count+1);State=$NewState;Start=$At;End=$null;Reason=$Reason}
    $script:LifeSegments.Add($segment)
    $script:LifeCurrentSegment=$segment
    $script:LifeTransitions.Add([pscustomobject]@{At=$At;Old=$old;New=$NewState;Reason=$Reason})
    Life-Say ("DISPLAY $old -> $NewState source=$Reason (no button commands or screen changes)")
}
function Life-CheckStable([datetime]$At) {
    if($script:LifeSynced -and $script:LifeState -eq 'OFF' -and
       -not $script:LifeOffStableAnnounced -and
       ($At-$script:LifeStateSince).TotalSeconds -ge $OffStableSeconds){
        $script:LifeOffStableAnnounced=$true
        Life-Say ("OFF-STABLE after $OffStableSeconds s: observe autonomous GPU/CPU/PMU and fuel gauge; NO forced wake")
    }
}
function Life-ParseLine([string]$Line,[datetime]$At) {
    # USB response is a live capture fence; old boot/ring log lines before it
    # are always excluded. Do not reset the display state after the fence!
    if(-not $script:LifeSynced){
        if($script:LifeFenceSent -and $Line -like '*usb> sysctl kern.hakios.hi_display_requested*'){
            $script:LifeFenceEcho=$true
        }
        if($script:LifeFenceEcho -and
           $Line -match 'usb: kern\.hakios\.hi_display_requested = ([01])(?:\s|$)'){
            $script:LifeSynced=$true
            $script:LifeStart=$At
            $script:LifeLastBig=$null;$script:LifeLastLittle=$null
            $script:LifeLastGpuC=$null;$script:LifeLastGpuAt=[datetime]::MinValue
            $script:LifeLastCpuAt=[datetime]::MinValue
            $script:LifeBatteryC=$null
            Life-SetState $(if($Matches[1] -eq '1'){'ON'}else{'OFF'}) 'READ_ONLY_SYSCTL_ANCHOR' $At
            Life-Say 'LIVE FENCE VERIFIED: only subsequent logs contribute to ON/OFF statistics'
        }
        return
    }
    if($Line -match '^GenericARMPE: the display (on|off)'){
        Life-SetState $Matches[1].ToUpperInvariant() 'GENERICARMPE_REQUEST' $At
        return
    }
    if($Line -match '^hi: scene home=([01])'){
        $newScene=if($Matches[1] -eq '1'){'HOME'}else{'NOT_HOME'}
        $script:LifeScene=$newScene
        return
    }
    if($Line -match '^hi: auto=(baseline|cut1|recover1|cut2|recover2|FINISHED)'){
        $phase=$Matches[1]
        if($phase -eq 'cut1' -or $phase -eq 'cut2'){$script:LifeHI2CutPhases++}
        Life-Say ('LEGACY HI2 AUTO PHASE '+$phase+' observed; do NOT attribute its Clock changes to P10')
        return
    }
    if($Line -match '^GenericARMPE: sleep_cores: cpu \d+ NOT (?:taken off|started again)'){
        throw ('KERNEL CPU hardware restart safety fault: '+$Line)
    }
    if($Line -match '^s6e3ha8: panel_sleep: screen (?:off|on):'){
        $script:LifeUnexpectedPanelDCS++
        Life-Say 'P9.1 WARNING: panel DCS sleep/wake event observed; physical image must be checked'
        return
    }
    $bucket=Life-Bucket $At
    if(-not $bucket){return}
    $v=$script:LifeBuckets[$bucket]
    if($Line -match '^cpufreq_gov: \d+ s: lit \d+ MHz asked load (\d+)% .*?\| big \d+ MHz asked load (\d+)% .*?\| temp big ([\d.]+) lit ([\d.]+) C'){
        $littleLoad=[int]$Matches[1];$bigLoad=[int]$Matches[2]
        $big=[double]::Parse($Matches[3],[Globalization.CultureInfo]::InvariantCulture)
        $little=[double]::Parse($Matches[4],[Globalization.CultureInfo]::InvariantCulture)
        $script:LifeLastBig=$big;$script:LifeLastLittle=$little;$script:LifeLastCpuAt=$At
        $v.CpuCount++;$v.BigSum+=$big;$v.LittleSum+=$little
        $v.BigPeak=if($null -eq $v.BigPeak){$big}else{[Math]::Max($v.BigPeak,$big)}
        $v.LittlePeak=if($null -eq $v.LittlePeak){$little}else{[Math]::Max($v.LittlePeak,$little)}
        $v.CpuBigLoadSum+=$bigLoad;$v.CpuLittleLoadSum+=$littleLoad
        $data=@{big_c=$big;little_c=$little;big_load_pct=$bigLoad;little_load_pct=$littleLoad}
        if($Line -match 'lit (\d+) MHz asked load \d+% \(avg \d+%, (\d+) online'){
            $littleMHz=[int]$Matches[1];$littleOnline=[int]$Matches[2]
            $v.CpuLittleMHzSum+=$littleMHz
            $v.CpuLittleOnlineMin=if($null -eq $v.CpuLittleOnlineMin){$littleOnline}else{[Math]::Min($v.CpuLittleOnlineMin,$littleOnline)}
            $data.little_mhz=$littleMHz;$data.little_online=$littleOnline
        }
        if($Line -match 'big (\d+) MHz asked load \d+% \(avg \d+%, (\d+) online'){
            $bigMHz=[int]$Matches[1];$bigOnline=[int]$Matches[2]
            $v.CpuBigMHzSum+=$bigMHz
            $v.CpuBigOnlineMin=if($null -eq $v.CpuBigOnlineMin){$bigOnline}else{[Math]::Min($v.CpuBigOnlineMin,$bigOnline)}
            $data.big_mhz=$bigMHz;$data.big_online=$bigOnline
        }
        Life-AddSample 'CPU' $At $data
        return
    }
    if($Line -match '^mali-power: up=\d+ms domain=([01]) .*?pll_en=([01]) .*?clock=(\d+)MHz'){
        $domain=[int]$Matches[1];$pll=[int]$Matches[2];$mhz=[int]$Matches[3]
        $v.GpuCount++;$v.GpuOff+=1-$domain;$v.PllOff+=1-$pll;$v.GpuClkSum+=$mhz
        $data=@{gpu_domain=$domain;gpu_pll=$pll;gpu_clock_mhz=$mhz}
        if($Line -match 'frame_age_ms=(\d+) job_age_ms=(\d+)'){
            $frameAge=0L;$jobAge=0L
            if([long]::TryParse($Matches[1],[ref]$frameAge) -and
               [long]::TryParse($Matches[2],[ref]$jobAge) -and
               $frameAge -le 1000000000 -and $jobAge -le 1000000000){
                $v.GpuAgesCount++;$v.GpuFrameAgeSum+=$frameAge;$v.GpuJobAgeSum+=$jobAge
                $data.gpu_frame_age_ms=$frameAge;$data.gpu_job_age_ms=$jobAge
            }
        }
        Life-AddSample 'GPU_PMU' $At $data
        return
    }
    if($Line -match '^mali-probe: .+\bbusy=(\d+)%'){
        $busy=[int]$Matches[1]
        $v.GpuBusyCount++;$v.GpuBusySum+=$busy
        $data=@{gpu_busy_pct=$busy}
        if($Line -match '\bg3d=(-?\d+)mc'){
            $temp=([double][int]$Matches[1])/1000.0
            if($temp -ge -20 -and $temp -le 150){
                $script:LifeLastGpuC=$temp;$script:LifeLastGpuAt=$At
                $v.GpuThermalCount++;$v.GpuTempSum+=$temp
                $v.GpuTempPeak=if($null -eq $v.GpuTempPeak){$temp}else{[Math]::Max($v.GpuTempPeak,$temp)}
                $data.gpu_temp_c=$temp
            }
        }
        Life-AddSample 'GPU_UTIL' $At $data
        return
    }
    if($Line -match '^max77705: fg: t \d+ s, .*?current (-?\d+) mA, avg (-?\d+) mA'){
        $instant=[int]$Matches[1];$average=[int]$Matches[2]
        $v.FuelCount++;$v.FuelSignedSum+=$instant
        if($instant -lt 0){$v.DischargeCount++;$v.DischargeSum+=-$instant}
        Life-AddSample 'FUEL_GAUGE' $At @{fg_signed_ma=$instant;fg_avg_ma=$average}
        return
    }
    if($Line -match '^fps: (\d+) presented, .*?; (\d+) refreshes;'){
        $presented=[int]$Matches[1];$refresh=[int]$Matches[2]
        $v.FpsCount++;$v.Presented+=$presented;$v.Refresh+=$refresh
        Life-AddSample 'FRAME' $At @{fps_presented=$presented;fps_refresh=$refresh}
        return
    }
    if($Line -match '^fps: (\d+) refreshes missed while moving, the longest gap ([\d.]+) ms;'){
        $missed=[int]$Matches[1]
        $gap=[double]::Parse($Matches[2],[Globalization.CultureInfo]::InvariantCulture)
        $v.Missed+=$missed;$v.GapMax=[Math]::Max($gap,$v.GapMax)
        $data=@{fps_missed=$missed}
        if($Line -match '\bG3D (\d+) C(?:\s|$)'){
            $temp=[double][int]$Matches[1]
            $script:LifeLastGpuC=$temp;$script:LifeLastGpuAt=$At
            $v.GpuThermalCount++;$v.GpuTempSum+=$temp
            $v.GpuTempPeak=if($null -eq $v.GpuTempPeak){$temp}else{[Math]::Max($v.GpuTempPeak,$temp)}
            $data.gpu_temp_c=$temp
        }
        Life-AddSample 'FRAME_MISS' $At $data
        return
    }
    if($Line -match '^cputop: \d+ s:'){
        $v.TopCount++
        $data=@{}
        $procs=@(
            @{regex='backboardd\[\d+\] (\d+)%';col='backboard_pct';sum='BackboardSum';count='BackboardCount'},
            @{regex='SpringBoard\[\d+\] (\d+)%';col='springboard_pct';sum='SpringBoardSum';count='SpringBoardCount'},
            @{regex='locationd\[\d+\] (\d+)%';col='locationd_pct';sum='LocationdSum';count='LocationdCount'},
            @{regex='wifid\[\d+\] (\d+)%';col='wifid_pct';sum='WifidSum';count='WifidCount'},
            @{regex='gnssd\[\d+\] (\d+)%';col='gnssd_pct';sum='GnssdSum';count='GnssdCount'}
        )
        foreach($p in $procs){
            if($Line -match $p.regex){
                $pct=[int]$Matches[1]
                $data[$p.col]=$pct
                $sumName=$p.sum;$countName=$p.count
                $v.$sumName+=$pct;$v.$countName++
            }
        }
        Life-AddSample 'PROCESS_TOP' $At $data
        return
    }
    if($Line -match '^poster_cost: .*CollectionsPoster\[\d+\] cpu=([\d.]+)%'){
        $cpu=[double]::Parse($Matches[1],[Globalization.CultureInfo]::InvariantCulture)
        $v.PosterCount++;$v.PosterSum+=$cpu
        $data=@{poster_cpu_pct=$cpu}
        if($Line -match '\brss=(\d+)KiB'){
            $rss=[long]$Matches[1]
            $v.PosterPeakRSSKiB=if($null -eq $v.PosterPeakRSSKiB){$rss}else{[Math]::Max($v.PosterPeakRSSKiB,$rss)}
            $data.poster_rss_kib=$rss
        }
        Life-AddSample 'POSTER_CPU' $At $data
        return
    }
    # Diagnostic log activity is not a measured contribution to power.
    if($Line -match '^shub: hub says: Sensor_'){$v.SensorLogLines++;return}
    if($Line -match '^brcmfmac:|(?:^|\s)wifid\[\d+'){$v.WifiLogLines++;return}
    if($Line -match '(?:^|\s)(?:gnssd|locationd)\[\d+'){$v.LocationLogLines++;return}
    if($Line -match '^hi: ctx id=\S+ window=SBCoverSheetWindow '){$v.CoverWindowCount++;return}
    if($Line -match '^hi: p10_lock mode=\d+ display=([01]) cover_locked=([01]) stable_ms=(\d+)'){
        $display=[int]$Matches[1];$cover=[int]$Matches[2];$stable=[long]$Matches[3]
        if($display -eq 1 -and $cover -eq 1){$v.LockProven++}
        Life-AddSample 'P10_LOCK_PROOF' $At @{lock_display=$display;cover_locked=$cover;lock_stable_ms=$stable}
        return
    }
    if($Line -match '^hi: link=\d+ target=SBUIPreciseClockTimer .+p10_lock_clock_cut=(\d+)'){
        $cut=[long]$Matches[1];$v.ClockCount++;$v.P10ClockCut+=$cut
        Life-AddSample 'P10_CLOCK' $At @{p10_clock_cut=$cut}
        return
    }
    if($Line -match '^hi: link=\d+ target=UIViewInProcessAnimationManager .+p10_quiet_anim_cut=(\d+)'){
        $cut=[long]$Matches[1];$v.AnimCount++;$v.P10AnimCut+=$cut
        Life-AddSample 'P10_ANIMATION' $At @{p10_anim_cut=$cut}
        return
    }
    if($Line -match '^tmu: .+\bbattery=(\d+)(?:\s|$)'){
        $script:LifeBatteryC=[int]$Matches[1]
    }
}
function Life-CheckSafety([datetime]$At) {
    if($script:LifeLastCpuAt -ne [datetime]::MinValue -and
       ($At-$script:LifeLastCpuAt).TotalSeconds -le 15){
        if($null -ne $script:LifeLastBig -and $script:LifeLastBig -ge $BigStopC){
            throw "BIG CPU temperature $($script:LifeLastBig)C >= $($BigStopC)C; passive capture stopped"
        }
        if($null -ne $script:LifeLastLittle -and $script:LifeLastLittle -ge $LittleStopC){
            throw "LITTLE CPU temperature $($script:LifeLastLittle)C >= $($LittleStopC)C; passive capture stopped"
        }
    }
    if($script:LifeLastGpuAt -ne [datetime]::MinValue -and
       ($At-$script:LifeLastGpuAt).TotalSeconds -le 15 -and
       $null -ne $script:LifeLastGpuC -and $script:LifeLastGpuC -ge $G3DStopC){
        throw "G3D temperature $($script:LifeLastGpuC)C >= $($G3DStopC)C; passive capture stopped"
    }
    if($null -ne $script:LifeBatteryC -and $script:LifeBatteryC -ge $BatteryStopC){
        throw "Battery temperature $($script:LifeBatteryC)C >= $($BatteryStopC)C; passive capture stopped"
    }
}
function Life-Feed([string]$Line,[datetime]$At) {
    Life-ParseLine $Line $At
    if($null -ne $script:LifeLineObserver){
        & $script:LifeLineObserver $Line $At
    }
    Life-CheckStable $At
}
function Life-Pump {
    $n=0
    try{$n=$script:LifeSerial.Read($script:LifeBuffer,0,$script:LifeBuffer.Length)}
    catch [System.TimeoutException]{return}
    if($n -le 0){return}
    $script:LifeBytes += $n
    $script:LifeRaw.Write($script:LifeBuffer,0,$n)
    $script:LifePartial += [Text.Encoding]::ASCII.GetString($script:LifeBuffer,0,$n)
    $pos=$script:LifePartial.IndexOf([char]10)
    while($pos -ge 0){
        $line=$script:LifePartial.Substring(0,$pos).TrimEnd([char]13)
        $script:LifePartial=$script:LifePartial.Substring($pos+1)
        Life-Feed $line (Life-Now)
        $pos=$script:LifePartial.IndexOf([char]10)
    }
    if($script:LifePartial.Length -gt 65536){
        throw 'USB serial line longer than 64KB; corrupted telemetry, raw bytes retained'
    }
}
function Life-DrainBacklog {
    Life-Say 'Draining any historical boot/USB logs before the live read-only fence (not attributing them to screen phases)'
    $deadline=(Life-Now).AddSeconds(18)
    $lastBytes=$script:LifeBytes
    $lastChanged=Life-Now
    while((Life-Now) -lt $deadline){
        Life-Pump
        if($script:LifeBytes -ne $lastBytes){$lastBytes=$script:LifeBytes;$lastChanged=Life-Now}
        if(((Life-Now)-$lastChanged).TotalMilliseconds -ge 450){return}
    }
    Life-Say 'Backlog quiet timeout: exact echo+reply fence is still required; no historical samples counted'
}
function Life-SyncReadOnly {
    Life-DrainBacklog
    $command='sysctl kern.hakios.hi_display_requested'
    $payload=[Text.Encoding]::ASCII.GetBytes($command+"`r")
    $script:LifeFenceSent=$true
    $script:LifeSerial.Write($payload,0,$payload.Length)
    Life-Say 'TX READ-ONLY sysctl kern.hakios.hi_display_requested (live serial fence, NO policy write)'
    $deadline=(Life-Now).AddSeconds($AckTimeoutSeconds)
    while((Life-Now) -lt $deadline -and -not $script:LifeSynced){
        Life-Pump
        Life-CheckSafety (Life-Now)
    }
    if(-not $script:LifeSynced){
        throw 'P10 display sysctl reply absent; cannot assign live states safely. No power or screen controls changed.'
    }
}
function Life-Average([double]$Sum,[int]$Count) {
    if($Count -lt 1){return 'n/a'}
    return [Math]::Round($Sum/$Count,1).ToString([Globalization.CultureInfo]::InvariantCulture)
}
function Life-Report([string]$Stem,[datetime]$EndAt,[string]$StopReason) {
    if($null -ne $script:LifeCurrentSegment -and $null -eq $script:LifeCurrentSegment.End){
        $script:LifeCurrentSegment.End=$EndAt
    }
    $report=$Stem+'.lifecycle-report.txt'
    $csv=$Stem+'.samples.csv'
    $lines=New-Object 'System.Collections.Generic.List[string]'
    [void]$lines.Add('S9 P11 NATURAL SCREEN LIFECYCLE: PASSIVE HOST OBSERVATIONS')
    [void]$lines.Add('NO sysctl writes/buttons/GPU or CPU power changes. P9.1 and P10 policies left unchanged.')
    [void]$lines.Add('LIVE fence via read-only hi_display_requested: '+$script:LifeSynced)
    [void]$lines.Add('Source: serial receive times (approximate), device diagnostic 1s/5s/10s sampling.')
    [void]$lines.Add('OFF_STABLE starts after '+$OffStableSeconds+' s without a display-ON event.')
    [void]$lines.Add('Not all GPU/CPU/FPs samples cover full phase; sample counts are NOT state residency.')
    [void]$lines.Add('Fuel gauge signed current measures BATTERY, not external USB/bench current or watts.')
    [void]$lines.Add('ON_NOT_HOME is only a SpringBoard scene observation, NOT verified lock proof.')
    [void]$lines.Add('Missing telemetry = n/a; never interpret missing GPU data as 0C or GPU powered OFF.')
    [void]$lines.Add('STOP_REASON='+$StopReason)
    [void]$lines.Add('DISPLAY ON->OFF='+$script:LifeOnToOff+' OFF->ON='+$script:LifeOffToOn+
        ' OFF_STABLE->ON='+$script:LifeCompleteCycles+' total_samples='+$script:LifeSamples.Count)
    [void]$lines.Add('LEGACY_HI2_CUT_PHASES_SEEN='+$script:LifeHI2CutPhases+
        ' PANEL_DCS_SLEEP_EVENTS_SEEN='+$script:LifeUnexpectedPanelDCS)
    if($script:LifeHI2CutPhases -gt 0){
        [void]$lines.Add('WARNING: legacy HI2 auto crossover overlapped; do NOT claim P10 causal frame savings.')
    }
    if($script:LifeUnexpectedPanelDCS -gt 0){
        [void]$lines.Add('WARNING: panel DCS events observed; P9.1 mode0 may not be preserved; inspect physical screen.')
    }
    [void]$lines.Add('')
    [void]$lines.Add('CHRONOLOGICAL DISPLAY SEGMENTS')
    foreach($seg in $script:LifeSegments){
        $secs=[Math]::Max(0,($seg.End-$seg.Start).TotalSeconds)
        $stableSec=if($seg.State -eq 'OFF'){[Math]::Max(0,$secs-$OffStableSeconds)}else{0}
        [void]$lines.Add(('#'+$seg.Index+' '+$seg.State+' '+
            $seg.Start.ToString('HH:mm:ss.fff')+'Z..'+$seg.End.ToString('HH:mm:ss.fff')+'Z'+
            ' host_seconds='+[Math]::Round($secs,1)+
            ' eligible_off_stable_seconds='+[Math]::Round($stableSec,1)+' source='+$seg.Reason))
    }
    if($script:LifeSegments.Count -eq 0){[void]$lines.Add('NONE: live USB display fence not confirmed')}
    [void]$lines.Add('')
    [void]$lines.Add('TELEMETRY BY HOST-OBSERVED DISPLAY STATE (sample averages, not energy)')
    foreach($key in @('ON_HOME','ON_NOT_HOME','ON_UNKNOWN','OFF_EARLY','OFF_STABLE')){
        $v=$script:LifeBuckets[$key]
        [void]$lines.Add(('BUCKET '+$key+
            ' cpu_n='+$v.CpuCount+
            ' BIG_mean_C='+(Life-Average $v.BigSum $v.CpuCount)+
            ' LITTLE_mean_C='+(Life-Average $v.LittleSum $v.CpuCount)+
            ' BIG_peak_C='+$(if($null -eq $v.BigPeak){'n/a'}else{$v.BigPeak})+
            ' LITTLE_peak_C='+$(if($null -eq $v.LittlePeak){'n/a'}else{$v.LittlePeak})+
            ' BIG_asked_load_mean_pct='+(Life-Average $v.CpuBigLoadSum $v.CpuCount)+
            ' LITTLE_asked_load_mean_pct='+(Life-Average $v.CpuLittleLoadSum $v.CpuCount)+
            ' BIG_freq_mean_MHz='+(Life-Average $v.CpuBigMHzSum $v.CpuCount)+
            ' LITTLE_freq_mean_MHz='+(Life-Average $v.CpuLittleMHzSum $v.CpuCount)+
            ' BIG_online_min='+$(if($null -eq $v.CpuBigOnlineMin){'n/a'}else{$v.CpuBigOnlineMin})+
            ' LITTLE_online_min='+$(if($null -eq $v.CpuLittleOnlineMin){'n/a'}else{$v.CpuLittleOnlineMin})))
        [void]$lines.Add(('  gpu_pmu_n='+$v.GpuCount+
            ' GPU_domain_OFF_samples='+$v.GpuOff+' GPU_PLL_OFF_samples='+$v.PllOff+
            ' GPU_clock_mean_MHz='+(Life-Average $v.GpuClkSum $v.GpuCount)+
            ' gpu_busy_n='+$v.GpuBusyCount+
            ' gpu_busy_mean_pct='+(Life-Average $v.GpuBusySum $v.GpuBusyCount)+
            ' gpu_temp_n='+$v.GpuThermalCount+
            ' gpu_temp_mean_C='+(Life-Average $v.GpuTempSum $v.GpuThermalCount)+
            ' gpu_temp_peak_C='+$(if($null -eq $v.GpuTempPeak){'n/a'}else{$v.GpuTempPeak})+
            ' gpu_ages_n='+$v.GpuAgesCount+
            ' gpu_frame_age_mean_ms='+(Life-Average $v.GpuFrameAgeSum $v.GpuAgesCount)+
            ' gpu_job_age_mean_ms='+(Life-Average $v.GpuJobAgeSum $v.GpuAgesCount)))
        [void]$lines.Add(('  fg_n='+$v.FuelCount+
            ' fg_signed_current_mean_mA='+(Life-Average $v.FuelSignedSum $v.FuelCount)+
            ' fg_discharge_only_n='+$v.DischargeCount+
            ' fg_discharge_only_mean_mA='+(Life-Average $v.DischargeSum $v.DischargeCount)))
        [void]$lines.Add(('  fps_n='+$v.FpsCount+' presented='+$v.Presented+
            ' refresh='+$v.Refresh+' missed='+$v.Missed+
            ' max_gap_ms='+$v.GapMax+' top_n='+$v.TopCount+
            ' backboard_cpu_n='+$v.BackboardCount+
            ' backboard_cpu_mean_pct='+(Life-Average $v.BackboardSum $v.BackboardCount)+
            ' springboard_cpu_n='+$v.SpringBoardCount+
            ' springboard_cpu_mean_pct='+(Life-Average $v.SpringBoardSum $v.SpringBoardCount)+
            ' locationd_cpu_n='+$v.LocationdCount+
            ' locationd_cpu_mean_pct='+(Life-Average $v.LocationdSum $v.LocationdCount)+
            ' wifid_cpu_n='+$v.WifidCount+
            ' wifid_cpu_mean_pct='+(Life-Average $v.WifidSum $v.WifidCount)+
            ' gnssd_cpu_n='+$v.GnssdCount+
            ' gnssd_cpu_mean_pct='+(Life-Average $v.GnssdSum $v.GnssdCount)))
        [void]$lines.Add(('  poster_n='+$v.PosterCount+
            ' poster_cpu_mean_pct='+(Life-Average $v.PosterSum $v.PosterCount)+
            ' poster_rss_peak_KiB='+$(if($null -eq $v.PosterPeakRSSKiB){'n/a'}else{$v.PosterPeakRSSKiB})+
            ' sensor_log_lines='+$v.SensorLogLines+
            ' wifi_log_lines='+$v.WifiLogLines+
            ' location_log_lines='+$v.LocationLogLines+
            ' coversheet_window_observations='+$v.CoverWindowCount+
            ' p10_lock_proven_samples='+$v.LockProven+
            ' p10_clock_cut='+$v.P10ClockCut+
            ' p10_anim_cut='+$v.P10AnimCut))
    }
    [void]$lines.Add('')
    [void]$lines.Add('INTERPRETATION GUARDS')
    if($script:LifeCompleteCycles -eq 0){
        [void]$lines.Add('NO FULL OFF-STABLE->ON CYCLE: insufficient to assess full real-world wake sequence.')
    }
    if($script:LifeBuckets['OFF_STABLE'].GpuCount -lt 2){
        [void]$lines.Add('OFF-STABLE PMU SAMPLES <2: cannot establish sustained GPU domain/PLL OFF.')
    }
    if($script:LifeBuckets['OFF_STABLE'].FuelCount -lt 3){
        [void]$lines.Add('OFF-STABLE FUEL SAMPLES <3: insufficient independent battery current samples.')
    }
    [void]$lines.Add('Compare external meter separately at stable screen-OFF, not this USB-connected fuel gauge.')
    [IO.File]::WriteAllLines($report,$lines.ToArray(),[Text.UTF8Encoding]::new($false))
    if($script:LifeSamples.Count -gt 0){
        $script:LifeSamples.ToArray() | Export-Csv -LiteralPath $csv -Encoding UTF8 -NoTypeInformation
    }else{
        [IO.File]::WriteAllText($csv,'No synchronized samples; see raw log and events.')
    }
    Life-Say ('HOST REPORT: '+$report)
    Life-Say ('TIME-ALIGNED SAMPLES: '+$csv)
    foreach($line in $lines){Write-Host $line}
}
function Life-Run {
    if(-not $Execute){
        Write-Host 'P11 PREVIEW: passive natural ON->auto OFF->OFF-STABLE->manual physical wake.'
        Write-Host "Duration=$DurationSeconds s, OFF_STABLE=$OffStableSeconds s; stop BIG=$BigStopC LITTLE=$LittleStopC G3D=$G3DStopC battery=$BatteryStopC C."
        Write-Host 'Only ONE READ-ONLY sysctl query; no policy writes, synthetic key events, forced backlight or hardware gating.'
        Write-Host 'Connect after boot, optionally wake Lock Screen normally, let it self-blank; after >=30-45s OFF press power ON once.'
        Write-Host 'Requires P10 image with read-only kern.hakios.hi_display_requested; close PuTTY before -Execute.'
        Write-Host 'Output .log + .events.txt + .lifecycle-report.txt + .samples.csv.'
        return
    }
    if($env:OS -ne 'Windows_NT'){throw 'Windows PowerShell COM capture only'}
    if([IO.Ports.SerialPort]::GetPortNames() -notcontains $Port){throw "Port $Port not found"}
    $stem='putty-S9-lifecycle-'+(Get-Date -Format 'yyyyMMdd-HHmmss')
    $rawpath=if([string]::IsNullOrWhiteSpace($LogPath)){$stem+'.log'}else{$LogPath}
    $script:LifeLogFinal=[IO.Path]::GetFullPath($rawpath)
    $stem=[IO.Path]::Combine([IO.Path]::GetDirectoryName($script:LifeLogFinal),
                             [IO.Path]::GetFileNameWithoutExtension($script:LifeLogFinal))
    if(Test-Path -LiteralPath $script:LifeLogFinal){throw 'Existing raw log cannot be overwritten'}
    $failure=$null
    try{
        $script:LifeSerial=[IO.Ports.SerialPort]::new($Port,$BaudRate,[IO.Ports.Parity]::None,8,[IO.Ports.StopBits]::One)
        $script:LifeSerial.Handshake=[IO.Ports.Handshake]::None
        $script:LifeSerial.DtrEnable=$true
        $script:LifeSerial.RtsEnable=$false
        $script:LifeSerial.ReadTimeout=160
        $script:LifeSerial.WriteTimeout=4000
        $script:LifeSerial.ReadBufferSize=131072
        $script:LifeSerial.Open()
        $script:LifeRaw=[IO.FileStream]::new($script:LifeLogFinal,[IO.FileMode]::CreateNew,
            [IO.FileAccess]::Write,[IO.FileShare]::Read)
        Life-Say "COM $Port OPENED; streaming raw bytes, P9.1 hardware policies untouched"
        Life-SyncReadOnly
        Life-Say "PASSIVE CAPTURE $DurationSeconds s; let Lock Screen auto-blank, OFF idle >=$OffStableSeconds s, manually wake when desired"
        $deadline=(Life-Now).AddSeconds($DurationSeconds)
        while((Life-Now) -lt $deadline){
            Life-Pump
            $now=Life-Now
            Life-CheckStable $now
            Life-CheckSafety $now
        }
        Life-Say 'PASSIVE CAPTURE FINISHED; no screen/power settings changed, NO cleanup commands needed'
    }catch{
        $failure=$_.Exception.Message
        Life-Say ('STOPPED: '+$failure)
    }finally{
        $end=Life-Now
        if($null -ne $script:LifeRaw){
            try{$script:LifeRaw.Flush()}catch{}
            $script:LifeRaw.Dispose();$script:LifeRaw=$null
        }
        if($null -ne $script:LifeSerial){$script:LifeSerial.Dispose();$script:LifeSerial=$null}
        if($script:LifeLogFinal -and (Test-Path -LiteralPath $script:LifeLogFinal)){
            Life-Report $stem $end $(if($failure){$failure}else{'DURATION_COMPLETE'})
            $eventpath=$stem+'.events.txt'
            [IO.File]::WriteAllLines($eventpath,$script:LifeEvents.ToArray(),[Text.UTF8Encoding]::new($false))
            Write-Host ('RAW LOG: '+$script:LifeLogFinal)
            Write-Host ('EVENTS: '+$eventpath)
        }
    }
    if($failure){throw ('P11 passive capture incomplete: '+$failure+' . Raw and reports retained.')}
}
if(-not $LibraryOnly){Life-Run}
