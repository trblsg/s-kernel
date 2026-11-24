# S9 P10 host-only Lock Screen source-timer A/B companion.
# NO display buttons, GPU, panel, clocks, CPU_OFF, hi_cmd or voltages changed.
# Use existing COM echo/ACK/readback harness; P9.1 stays as-is.
function P10-New-Stats([string]$Phase) {
    return [pscustomobject]@{
        Phase=$Phase
        ClockTicks=[long]0;ClockForward=[long]0;ClockCut=[long]0
        AnimationTicks=[long]0;AnimationForward=[long]0;AnimationCut=[long]0
        ClockSamples=0;AnimationSamples=0
        LockProofSamples=0;LockStableMs=[long]0
        AnimationProofSamples=0;AnimationQuietMs=[long]0
        FpsWindows=0;Presented=[long]0;Refresh=[long]0;Missed=[long]0
        MaxGapMs=[double]0;CaWindows=0;CaIgnore=[long]0;CaAccept=[long]0
        BigPeakC=[double]0;GpuPeakC=[double]0;BatteryPeakC=[double]0
    }
}
function P10-Start-Phase([string]$Phase) {
    $script:P10ActivePhase=$Phase
    $script:P10PhaseStats[$Phase]=P10-New-Stats $Phase
    Say ('P10 PHASE START '+$Phase+' window='+$P10WindowSeconds+'s: keep verified Lock Screen ON, do not touch, do not trigger notification.')
}
function Scan-P10-Line([string]$Line) {
    if($Line -match '^hi: p10_lock_policy=(\d+)'){
        $script:P10PolicySeen=[int]$Matches[1]
        Say ('P10 SpringBoard confirmed policy='+$script:P10PolicySeen)
    }
    if($Line -match '^GenericARMPE: the display off'){
        $script:P10ScreenOffSeen=$true
    }
    if(-not $script:P10ActivePhase -or
       -not $script:P10PhaseStats.Contains($script:P10ActivePhase)){return}
    $v=$script:P10PhaseStats[$script:P10ActivePhase]
    if($Line -match '^hi: p10_lock mode=(\d+) display=(\d+) cover_locked=(\d+) stable_ms=(\d+) all_anim_scanned=(\d+) anim_empty=(\d+) commits_empty=(\d+) anim_quiet_ms=(\d+)'){
        if([int]$Matches[2] -eq 1 -and [int]$Matches[3] -eq 1){
            $v.LockProofSamples++
            $v.LockStableMs=[Math]::Max($v.LockStableMs,[long]$Matches[4])
        }
        if([int]$Matches[5] -eq 1 -and [int]$Matches[6] -eq 1 -and
           [int]$Matches[7] -eq 1){
            $v.AnimationProofSamples++
            $v.AnimationQuietMs=[Math]::Max($v.AnimationQuietMs,[long]$Matches[8])
        }
    }
    if($Line -match '^hi: link=\d+ target=SBUIPreciseClockTimer action=_onDisplayLink: ticks_5s=(\d+) forwarded=(\d+) cut=(\d+) .*p10_lock_clock_cut=(\d+)'){
        $v.ClockSamples++
        $v.ClockTicks += [long]$Matches[1]
        $v.ClockForward += [long]$Matches[2]
        $v.ClockCut += [long]$Matches[4]
    }
    if($Line -match '^hi: link=\d+ target=UIViewInProcessAnimationManager action=_displayLinkFire: ticks_5s=(\d+) forwarded=(\d+) cut=(\d+) .*p10_quiet_anim_cut=(\d+)'){
        $v.AnimationSamples++
        $v.AnimationTicks += [long]$Matches[1]
        $v.AnimationForward += [long]$Matches[2]
        $v.AnimationCut += [long]$Matches[4]
    }
    if($Line -match '^fps: (\d+) presented, (\d+) drawn, (\d+) flips, .*?; (\d+) refreshes;'){
        $v.FpsWindows++
        $v.Presented += [long]$Matches[1]
        $v.Refresh += [long]$Matches[4]
    }
    if($Line -match '^fps: (\d+) refreshes missed while moving, the longest gap ([\d.]+) ms;'){
        $v.Missed += [long]$Matches[1]
        $gap=[double]::Parse($Matches[2],[Globalization.CultureInfo]::InvariantCulture)
        $v.MaxGapMs=[Math]::Max($v.MaxGapMs,$gap)
    }
    if($Line -match '^p10-ca: windowserver_render_calls=256 CA_ignore_true=(\d+) CA_ignore_false=(\d+) policy=(\d+)'){
        $v.CaWindows++
        $v.CaIgnore += [long]$Matches[1]
        $v.CaAccept += [long]$Matches[2]
    }
    if($null -ne $script:BigC){$v.BigPeakC=[Math]::Max($v.BigPeakC,[double]$script:BigC)}
    if($null -ne $script:G3DC){$v.GpuPeakC=[Math]::Max($v.GpuPeakC,[double]$script:G3DC)}
    if($script:BatteryC -gt 0){$v.BatteryPeakC=[Math]::Max($v.BatteryPeakC,[double]$script:BatteryC)}
}
function P10-Measure-Window([string]$Phase) {
    P10-Start-Phase $Phase
    if(-not $Execute){return}
    $end=[datetime]::UtcNow.AddSeconds($P10WindowSeconds)
    while([datetime]::UtcNow -lt $end){
        Pump
        Check-Thermal
        if($script:P10ScreenOffSeen -or $script:DisplayOff){
            throw "P10 ${Phase}: screen turned OFF. Keep real Lock Screen ON for the A/B sample, no simulated wake/frames; policy0 cleanup."
        }
    }
    $v=$script:P10PhaseStats[$Phase]
    Say ("P10 PHASE END $Phase clock_ticks=$($v.ClockTicks) clock_forward=$($v.ClockForward) " +
        "p10_clock_cut=$($v.ClockCut) anim_ticks=$($v.AnimationTicks) p10_anim_cut=$($v.AnimationCut) " +
        "cover_proof=$($v.LockProofSamples) stable_ms=$($v.LockStableMs) " +
        "fps_presented=$($v.Presented) fps_refresh=$($v.Refresh) missed=$($v.Missed) CA_windows=$($v.CaWindows)")
    $script:P10ActivePhase=''
}
function Run-P10Lock {
    Say ('P10 source-only Lock Screen A/B: '+$(if($P10Restore){'RESTORE'}else{'CAPTURE'})+
         '; P9.1 GPU/Panel/Wake policies are NOT modified')
    # P10 booted with hi=2 and new read-only display request. A P9-only
    # kernel must reject this BEFORE the host changes any power policy.
    if($P10Restore){
        Send-Checked 'sysctl kern.hakios.hi_lock_policy=0'
        $script:Completed=$true
        Say 'P10 RESTORE VERIFIED: policy=0; no P9.1 controls touched.'
        return
    }
    Send-Checked 'sysctl kern.hakios.hi_display_requested'
    if($script:Recent -notmatch '(?m)usb: kern\.hakios\.hi_display_requested = 1\r?$'){
        throw 'P10 preflight: GenericARMPE does not report display ON. Wake to visible LOCK screen manually before capture.'
    }
    Send-Checked 'sysctl kern.hakios.p9_panel_stats'
    if($script:Recent -notmatch 'mode=0 panel_sleep=0 panel_asleep=0 display_requested=1'){
        throw 'P10 preflight: P9.1 panel DCS bypass lost/unknown; no A/B. Preserve tear-free p9_panel_mode=0.'
    }
    Say 'P10 preflight: display ON, panel DCS disabled, P9.1 GPU gate preserved. No panel/CPU/GPU writes.'
    # Explicitly reset a possibly stale P10 policy from a prior session.
    Send-Checked 'sysctl kern.hakios.hi_lock_policy=0'
    $script:P10ScreenOffSeen=$false
    $script:DisplayOff=$false
    P10-Measure-Window 'A0'
    $script:P10PolicySeen=-1
    Send-Checked 'sysctl kern.hakios.hi_lock_policy=1'
    Wait-For 'P10 SpringBoard acknowledged exact Clock mode=1' 12 {
        $script:P10PolicySeen -eq 1
    } -Thermal
    P10-Measure-Window 'B1'
    Send-Checked 'sysctl kern.hakios.hi_lock_policy=0'
    P10-Measure-Window 'A1'
    if($P10Mode2){
        $script:P10PolicySeen=-1
        Send-Checked 'sysctl kern.hakios.hi_lock_policy=2'
        Wait-For 'P10 SpringBoard acknowledged guarded UIKit mode=2' 12 {
            $script:P10PolicySeen -eq 2
        } -Thermal
        P10-Measure-Window 'B2'
        Send-Checked 'sysctl kern.hakios.hi_lock_policy=0'
        P10-Measure-Window 'A2'
    }
    $clock=$script:P10PhaseStats['B1']
    if($clock.ClockCut -gt 0){
        Say ('P10 CALLBACK REDUCTION OBSERVED mode1: '+$clock.ClockCut+
            ' exact SBUIPreciseClockTimer callbacks cut. No generic CA frame suppression.')
    }else{
        Say 'P10 SOURCE CUT NOT PROVEN: no exact Clock callbacks cut. Read cover_proof, lock stability, ticks, and the raw log; DO NOT claim 60FPS fixed.'
    }
    if($P10Mode2){
        $anim=$script:P10PhaseStats['B2']
        if($anim.AnimationCut -gt 0){
            Say ('P10 mode2 guarded UIKit timer cut observed: '+$anim.AnimationCut)
        }else{
            Say 'P10 mode2 refused/idle: no extra UIKit callbacks cut. This can be SAFE because animations or CA commits still exist.'
        }
    }
    $script:Completed=$true
    Say 'P10 A/B/A'+$(if($P10Mode2){'/B/A'}else{''})+
        ' CAPTURE COMPLETE, hi_lock_policy=0 restored; P9.1 retained. Visible Lock Screen and current draw must be checked by the user.'
}
function Save-P10Report([string]$Stem){
    $file=$Stem+'.p10-report.txt'
    $lines=New-Object 'System.Collections.Generic.List[string]'
    [void]$lines.Add('S9 P10 LOCKSCREEN SOURCE-LEVEL A/B - HOST OBSERVATIONS')
    [void]$lines.Add('P9.1 panel DCS mode0 and GPU autonomous gate are NOT modified by P10.')
    [void]$lines.Add('A0/A1/A2 = policy0; B1 = Clock-only policy1; B2 = optional conservative animation policy2.')
    [void]$lines.Add('Counts represent observed LOG intervals, NOT measured screen watts or FPS savings.')
    [void]$lines.Add('A frame may still render 60 FPS because CollectionsPoster/backboardd requests persist.')
    [void]$lines.Add('TRACE: no suppression of generic CA/GL/present/DECON; CPU_OFF is forbidden.')
    foreach($name in @('A0','B1','A1','B2','A2')){
        if(-not $script:P10PhaseStats.Contains($name)){continue}
        $v=$script:P10PhaseStats[$name]
        [void]$lines.Add('')
        [void]$lines.Add(('PHASE '+$name+
            ' clock_ticks='+$v.ClockTicks+' clock_forward='+$v.ClockForward+
            ' p10_clock_cut='+$v.ClockCut+' clock_windows='+$v.ClockSamples+
            ' anim_ticks='+$v.AnimationTicks+' anim_forward='+$v.AnimationForward+
            ' p10_anim_cut='+$v.AnimationCut+' anim_windows='+$v.AnimationSamples))
        [void]$lines.Add(('  lock_proof_samples='+$v.LockProofSamples+
            ' stable_max_ms='+$v.LockStableMs+
            ' all_zero_anim_and_commits_samples='+$v.AnimationProofSamples+
            ' anim_quiet_ms='+$v.AnimationQuietMs))
        [void]$lines.Add(('  fps_windows='+$v.FpsWindows+
            ' presented='+$v.Presented+' refresh='+$v.Refresh+
            ' missed_refreshes='+$v.Missed+' max_gap_ms='+$v.MaxGapMs))
        [void]$lines.Add(('  CA_windows='+$v.CaWindows+
            ' original_CA_ignore_true='+$v.CaIgnore+
            ' original_CA_ignore_false='+$v.CaAccept+
            ' big_peak_c='+$v.BigPeakC+
            ' gpu_peak_c='+$v.GpuPeakC+
            ' battery_peak_c='+$v.BatteryPeakC))
    }
    if($script:P10PhaseStats.Contains('B1')){
        $v=$script:P10PhaseStats['B1']
        [void]$lines.Add('')
        [void]$lines.Add(('MODE1_MEASURED_CALLBACK_CUT='+
            $(if($v.ClockCut -gt 0){'YES'}else{'NO_OR_NO_LOCK_PROOF'})))
    }
    if($script:P10PhaseStats.Contains('B2')){
        $v=$script:P10PhaseStats['B2']
        [void]$lines.Add(('MODE2_MEASURED_CALLBACK_CUT='+
            $(if($v.AnimationCut -gt 0){'YES'}else{'NO_OR_ANIMATION_ACTIVE'})))
    }
    [void]$lines.Add('Note: missed refresh count is NOT proof of panel tearing; look at actual image.')
    [IO.File]::WriteAllLines($file,$lines.ToArray(),[Text.UTF8Encoding]::new($false))
    Say ('P10 A/B REPORT: '+$file)
    foreach($line in $lines){Write-Host $line}
}
