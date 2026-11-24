#requires -Version 5.1
Set-StrictMode -Version 2
$ErrorActionPreference='Stop'
$root=Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$main=Join-Path $root 'tools/s9-home-idle-serial-auto.ps1'
$inc=Join-Path $root 'tools/s9-p10-lockscreen-serial.inc.ps1'
if(-not (Test-Path $main) -or -not (Test-Path $inc)){throw 'P10 host files absent'}
function Run-Preview([string[]]$arguments,[bool]$wantSuccess){
    # Invalid preview cases intentionally print on stderr. In PS 5.1
    # Strict Stop can promote native stderr to a terminating error.
    $oldPreference=$ErrorActionPreference
    $ErrorActionPreference='Continue'
    try{
        $output=& powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $main @arguments 2>&1 | Out-String
        $exit=$LASTEXITCODE
    }finally{$ErrorActionPreference=$oldPreference}
    if($wantSuccess -and $exit -ne 0){throw "P10 PREVIEW failed exit=$exit $output"}
    if(-not $wantSuccess -and $exit -eq 0){throw 'Invalid P10 combination was accepted'}
    return $output
}
$good=Run-Preview @('-P10Lock','-Port','COM125') $true
if($good -notmatch 'P10 auto A/B:' -or $good -notmatch 'P9.1 panel DCS=0'){
    throw 'P10 preview missing A/B and P9.1 no-DCS policy'
}
$both=Run-Preview @('-P10Lock','-P10Mode2','-P10WindowSeconds','18') $true
if($both -notmatch 'optional mode2=True'){throw 'P10 mode2 preview missing'}
$restore=Run-Preview @('-P10Restore') $true
if($restore -notmatch 'P10 restore sets only'){throw 'P10 restore preview missing'}
$mix=Run-Preview @('-P10Lock','-P9Fix') $false
if($mix -notmatch 'cannot be combined'){throw 'P9/P10 mixed operations not rejected'}
$badMode=Run-Preview @('-P10Mode2') $false
if($badMode -notmatch 'requires -P10Lock'){throw 'P10 mode2 without A/B not rejected'}
$p9=Run-Preview @('-P9Fix') $true
if($p9 -notmatch 'panel DCS OFF'){throw 'Old P9 preview regressed'}

$raw=Get-Content -LiteralPath $main -Raw
if($raw -notmatch 'if\(\$P10Lock -or \$P10Restore\)\{\s*\$p10Companion=' -or
   $raw -notmatch '\$script:P10Allowed=' -or
   $raw -notmatch 'hi_lock_policy=0' -or
   $raw -notmatch 'Save-P10Report -Stem \$stem' -or
   $raw -notmatch 'if\(-not \(\$P10Lock -or \$P10Restore\)\)'){
    throw 'P10 branch/allowlist/final cleanup/P9 compatibility missing'
}
$body=Get-Content -LiteralPath $inc -Raw
foreach($marker in @('hi_display_requested','mode=0 panel_sleep=0 panel_asleep=0',
                     'P10ScreenOffSeen','ClockProof','P10-Measure-Window',
                     'P10 SpringBoard acknowledged','P10 CALLBACK REDUCTION OBSERVED',
                     'P10 SOURCE CUT NOT PROVEN','P10-Start-Phase',
                     'p10_lock_clock_cut','p10_quiet_anim_cut')){
    # The proof key is actually LockProofSamples, not a shortcut from
    # a successful kernel write.
    if($marker -eq 'ClockProof'){continue}
    if(-not $body.Contains($marker)){throw "P10 parser missing $marker"}
}
if($body -match "Send-Checked 'button lock'" -or
   $body -match "Send-Checked 'sysctl kern.hakios.p9_auto_gpu=" -or
   $body -match "Send-Checked 'sysctl kern.hakios.p9_panel_mode="){
    throw 'P10 touches device screen or physical GPU/panel power!'
}

# Offline fixture exercises the SAME parser used by the serial scenario.
$script:P10ActivePhase='B1'
$script:P10PhaseStats=[ordered]@{}
$script:P10PolicySeen=-1
$script:P10ScreenOffSeen=$false
$script:BigC=$null
$script:G3DC=$null
$script:BatteryC=0
$script:Events=New-Object 'System.Collections.Generic.List[string]'
function Say([string]$Message){$script:Events.Add($Message)}
. $inc
$script:P10PhaseStats['B1']=P10-New-Stats 'B1'
$fixtures=@(
    'hi: p10_lock_policy=1 target_clock=SBUIPreciseClockTimer clock_min_forward_hz=1',
    'hi: p10_lock mode=1 display=1 cover_locked=1 stable_ms=6500 all_anim_scanned=0 anim_empty=0 commits_empty=0 anim_quiet_ms=0 exact_sources_only=1 CA_GL_present_unchanged=1',
    'hi: link=2 target=SBUIPreciseClockTimer action=_onDisplayLink: ticks_5s=76 forwarded=6 cut=70 fps_req=15 verified_cut=0 p9_hidden_clock_cut=0 p10_lock_clock_cut=70 p10_quiet_anim_cut=0',
    'hi: link=3 target=SBHClockApplicationIconImageView action=_timerFired: ticks_5s=50 forwarded=50 cut=0 fps_req=10 verified_cut=0 p9_hidden_clock_cut=0 p10_lock_clock_cut=0 p10_quiet_anim_cut=0',
    'hi: link=4 target=UIViewInProcessAnimationManager action=_displayLinkFire: ticks_5s=100 forwarded=100 cut=0 fps_req=60 verified_cut=0 p9_hidden_clock_cut=0 p10_lock_clock_cut=0 p10_quiet_anim_cut=0',
    'fps: 60 presented, 60 drawn, 60 flips, 0 lost, 0 held; drawing 8 us a frame, 18 at most; waiting for the flip before 4 us, 13 at most; 60 refreshes; 0 partial;',
    'fps: 0 refreshes missed while moving, the longest gap 16.8 ms; zc: 60 in place',
    'p10-ca: windowserver_render_calls=256 CA_ignore_true=100 CA_ignore_false=156 policy=1 render_update_always_forwarded=1 GL_present_unchanged=1'
)
foreach($line in $fixtures){Scan-P10-Line $line}
$v=$script:P10PhaseStats['B1']
if($script:P10PolicySeen -ne 1 -or $v.ClockTicks -ne 76 -or
   $v.ClockForward -ne 6 -or $v.ClockCut -ne 70 -or
   $v.LockProofSamples -ne 1 -or $v.LockStableMs -ne 6500 -or
   $v.AnimationCut -ne 0 -or $v.AnimationTicks -ne 100 -or
   $v.Presented -ne 60 -or $v.Refresh -ne 60 -or
   $v.CaIgnore -ne 100 -or $v.CaAccept -ne 156){
    throw "P10 parser fixture mismatch: $($v|Out-String)"
}
Scan-P10-Line 'GenericARMPE: the display off'
if(-not $script:P10ScreenOffSeen){throw 'P10 physical display OFF safety event not recorded'}
Write-Output 'P10 SERIAL HOST PASS: P9 compatible, P10 source-only A/B/A and mode2 preview, invalid pairs blocked, exact callback/fps/CA fixture parsed, screen-OFF fail-closed.'
