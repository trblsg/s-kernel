#requires -Version 5.1
Set-StrictMode -Version 2
$ErrorActionPreference='Stop'
$dir=Split-Path -Parent $MyInvocation.MyCommand.Path
$scriptFile=Join-Path (Split-Path -Parent $dir) 's9-screen-lifecycle-serial.ps1'
if(-not (Test-Path -LiteralPath $scriptFile)){throw 'P11 lifecycle script missing'}

$preview=& powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $scriptFile -Port COM125 2>&1 | Out-String
if($LASTEXITCODE -ne 0 -or $preview -notmatch 'passive natural' -or
   $preview -notmatch 'ONE READ-ONLY' -or $preview -notmatch 'OFF_STABLE=25'){
    throw "P11 preview failed: $preview"
}
$body=[IO.File]::ReadAllText($scriptFile)
if(-not $body.Contains('$payload=[Text.Encoding]::ASCII.GetBytes($command+"`r")') -or
   $body.Contains('$payload=[Text.Encoding]::ASCII.GetBytes($command+"\r")')){
    throw 'P11 must send actual carriage return (backtick-r), never backslash-r'
}
if($body -match "sysctl kern\.hakios\.(?:hi_lock_policy|p9_panel_mode|p9_auto_gpu|p7_cpu_park|g3d_lab_gate)\s*=" -or
   $body -match "Send-Checked 'button" -or $body -match "sysctl kern\.hakios\.hi_cmd="){
    throw 'P11 passive capture contains forbidden power/source mutation'
}

# Import just the parser in the same process, without opening COM.
. $scriptFile -LibraryOnly -OffStableSeconds 25
$base=[datetime]::Parse('2026-11-24T12:00:00Z').ToUniversalTime()
function At([int]$Seconds){return $base.AddSeconds($Seconds)}
$script:LifeFenceSent=$true
# An old USB response before our command ECHO must be ignored.
Life-Feed 'usb: kern.hakios.hi_display_requested = 1' (At 0)
if($script:LifeSynced){throw 'Premature fence readback accepted before host command echo'}
Life-Feed 'usb> sysctl kern.hakios.hi_display_requested' (At 0)
Life-Feed 'usb: kern.hakios.hi_display_requested = 1' (At 1)
if(-not $script:LifeSynced -or $script:LifeState -ne 'ON'){throw 'Read-only anchor or initial ON missing'}
Life-Feed 'hi: scene home=1 frontmost_class=none bundle=unknown app_state=0 settings_confirmed=0' (At 1)
Life-Feed 'cpufreq_gov: 10 s: lit 1053 MHz asked load 10% (avg 5%, 4 online, 1794 max, measured 1049) | big 650 MHz asked load 23% (avg 10%, 4 online, 1794 max, measured 649) | temp big 66.0 lit 60.0 C | boost' (At 2)
# This is the previously missed OFF event immediately after preflight.
Life-Feed 'GenericARMPE: the display off (iOS power request)' (At 3)
if($script:LifeState -ne 'OFF' -or $script:LifeOnToOff -ne 1){throw 'ON->OFF transition lost after preflight'}
Life-Feed 'cpufreq_gov: 11 s: lit 1053 MHz asked load 0% (avg 0%, 4 online, 1794 max, measured 1049) | big 650 MHz asked load 3% (avg 8%, 4 online, 1794 max, measured 649) | temp big 65.0 lit 61.0 C | boost' (At 4)
Life-Feed 'shub: device silent' (At 29)
if(-not $script:LifeOffStableAnnounced){throw 'Stable OFF event not emitted at 25s'}
Life-Feed 'mali-power: up=30000ms domain=0 status=0x000 config=0x0 awake=0 sleep=1 pll_en=0 pll_stable=0 mux=0 clock=0MHz frame_age_ms=60000 job_age_ms=60000' (At 30)
Life-Feed 'mali-probe: up=31000ms windows=10 busy=0% frame_windows=0 floor_forced=0 thermal_windows=0 framefloor=450MHz clock=0MHz level=0 g3d=58000mc' (At 31)
Life-Feed 'max77705: fg: t 32 s, 99.50%, 2100 mAh, current -450 mA, avg -455 mA, QH 2700' (At 32)
Life-Feed 'cpufreq_gov: 12 s: lit 1053 MHz asked load 0% (avg 0%, 4 online, 1794 max, measured 1049) | big 650 MHz asked load 1% (avg 2%, 4 online, 1794 max, measured 649) | temp big 61.0 lit 59.0 C | boost' (At 33)
Life-Feed 'cputop: 40 s: 12% in all: backboardd[18] 7% SpringBoard[8] 2% gnssd[9] 1% locationd[52] 2% wifid[15] 1%' (At 34)
Life-Feed 'shub: hub says: Sensor_ACC--93,248,4072,20,-1' (At 35)
Life-Feed 'brcmfmac: lx: msgbuf: wifi scan' (At 36)
Life-Feed '2026-11-24 06:11:16.355 locationd[52:2508] scan request' (At 37)
Life-Feed 'GenericARMPE: the display on (iOS power request)' (At 45)
Life-Feed 'hi: scene home=0 frontmost_class=none bundle=unknown app_state=0 settings_confirmed=0' (At 45)
Life-Feed 'hi: ctx id=0xa2b5a69d window=SBCoverSheetWindow commits_5s=19 current_commit=245' (At 46)
Life-Feed 'hi: p10_lock mode=1 display=1 cover_locked=1 stable_ms=6500 all_anim_scanned=1 anim_empty=1 commits_empty=1 anim_quiet_ms=9000' (At 47)
Life-Feed 'poster_cost: 190 s: CollectionsPoster[154] cpu=0.0% rss=46688KiB vm=399036MiB faults=0 pageins=0' (At 47)
Life-Feed 'hi: link=2 target=SBUIPreciseClockTimer action=_onDisplayLink: ticks_5s=76 forwarded=6 cut=70 fps_req=15 verified_cut=0 p9_hidden_clock_cut=0 p10_lock_clock_cut=70 p10_quiet_anim_cut=0' (At 48)
Life-Feed 'fps: 10 presented, 10 drawn, 10 flips, 0 lost, 0 held; drawing 10 us a frame; 60 refreshes; 0 partial' (At 49)
Life-Feed 'fps: 0 refreshes missed while moving, the longest gap 0.0 ms; zc: 10 in place; gpu: 2% busy, 2000 us a frame shown, 60 jobs, 455 MHz (mali_gov), 10 switches, G3D 65 C' (At 50)
Life-Feed 'hi: auto=cut1 up=212s source=3 gate=1 duration_s=20 input_safe=1 home=1' (At 50)
if($script:LifeCompleteCycles -ne 1 -or $script:LifeOffToOn -ne 1 -or
   $script:LifeSegments.Count -ne 3){
    throw 'P11 host did not retain natural ON->OFF-STABLE->ON cycle'
}
$off=$script:LifeBuckets['OFF_STABLE']
$on=$script:LifeBuckets['ON_NOT_HOME']
if($off.GpuCount -ne 1 -or $off.GpuOff -ne 1 -or $off.PllOff -ne 1 -or
   $off.GpuBusyCount -ne 1 -or $off.FuelCount -ne 1 -or
   $off.DischargeSum -ne 450 -or $off.TopCount -ne 1 -or
   $off.BackboardSum -ne 7 -or $off.GpuTempPeak -ne 58 -or
   $off.LocationdCount -ne 1 -or $off.LocationdSum -ne 2 -or
   $off.WifidCount -ne 1 -or $off.GnssdCount -ne 1 -or
   $off.SensorLogLines -ne 1 -or $off.WifiLogLines -ne 1 -or
   $off.LocationLogLines -ne 1){
    throw "OFF_STABLE parser/PMU/current/cputop error: $($off|Out-String)"
}
if($on.CoverWindowCount -ne 1 -or $on.LockProven -ne 1 -or
   $on.ClockCount -ne 1 -or $on.P10ClockCut -ne 70 -or
   $on.FpsCount -ne 1 -or $on.Presented -ne 10 -or
   $on.Refresh -ne 60 -or $on.GpuTempPeak -ne 65 -or
   $on.PosterPeakRSSKiB -ne 46688 -or $script:LifeHI2CutPhases -ne 1){
    throw "ON_NOT_HOME parser/lock/clock/FPS error: $($on|Out-String)"
}
$stem=Join-Path $dir ('p11-fixture-'+[guid]::NewGuid().ToString('N'))
try{
    Life-Report $stem (At 51) 'OFFLINE_TEST' | Out-Null
    $report=Get-Content -LiteralPath ($stem+'.lifecycle-report.txt') -Raw
    $csv=Import-Csv -LiteralPath ($stem+'.samples.csv')
    if($report -notmatch 'OFF_STABLE->ON=1' -or
       $report -notmatch 'GPU_domain_OFF_samples=1' -or
       $report -notmatch 'fg_discharge_only_mean_mA=450' -or
       $report -notmatch 'LEGACY_HI2_CUT_PHASES_SEEN=1' -or
       $report -notmatch 'locationd_cpu_mean_pct=2' -or
       $report -notmatch 'poster_rss_peak_KiB=46688' -or
       $csv.Count -lt 7){
        throw 'P11 generated report and per-source CSV do not match fixture'
    }
}finally{
    # Parenthesize the two expressions: PowerShell otherwise can join them
    # into one literal string and leak transient fixture files in the repo.
    $fixtureFiles=@(($stem+'.lifecycle-report.txt'),($stem+'.samples.csv'))
    foreach($file in $fixtureFiles){
        if([IO.File]::Exists($file)){[IO.File]::Delete($file)}
    }
}
# Overheat guard is passive: abort capture, never write GPU/CPU/voltage.
$script:LifeLastBig=83.0
$script:LifeLastCpuAt=(At 52)
try{
    Life-CheckSafety (At 52)
    throw 'BIG thermal guard failed to stop'
}catch{
    if($_.Exception.Message -notmatch 'BIG CPU temperature 83C'){throw}
}
Write-Output 'P11 NATURAL LIFECYCLE PASS: live fence, OFF race retained, natural OFF-STABLE and wake, PMU/thermal/fuel/process/FPS/P10 parsers, report+CSV, passive thermal abort.'
