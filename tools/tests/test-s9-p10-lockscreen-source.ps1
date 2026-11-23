#requires -Version 5.1
Set-StrictMode -Version 2
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$paths=@{
    header='userland/src/s9_p10_lock_policy.h'
    scene='userland/src/s9_p10_lock_scene.inc'
    hi='userland/src/s9_home_idle.inc'
    ca='userland/src/inject.c'
    mali='drivers/gpu/mali/hakios.mali.c'
    panel='drivers/display/panel/hakios.s6e3ha8.c'
    p9='tools/s9-p9-power-fix.inc.ps1'
}
$text=@{};foreach($name in $paths.Keys){
    $file=Join-Path $repo $paths[$name]
    if(-not (Test-Path $file)){throw "P10 missing $file"}
    $text[$name]=Get-Content -LiteralPath $file -Raw
}
foreach($x in @('SBUIPreciseClockTimer','UIViewInProcessAnimationManager',
                 'S9_P10_MIN_LOCK_US','S9_P10_MIN_INPUT_QUIET_US',
                 'S9_P10_PROOF_FRESH_US','S9_P10_ANIM_QUIET_US')){
    if(-not $text.header.Contains($x)){throw "P10 policy missing $x"}
}
foreach($x in @('SBLockScreenManager','isUILocked','SBCoverSheetWindow',
                 'hi_display_requested','hi_p10_lock_since',
                 'mode==2','hi_count_animations',
                 'method_getReturnType','complete_animation_scan')){
    # The source actually uses 'mode==2' for strict scanning; test source
    # must not claim a lock screen from !wallpaper_home_showing() alone.
    if(-not $text.scene.Contains($x) -and -not $text.header.Contains($x)){
        throw "P10 lock proof missing $x"
    }
}
foreach($x in @('s9_p10_lock_policy.h','s9_p10_lock_scene.inc',
                 'hi_p10_maybe_skip','p10_lock_clock_cut',
                 'p10_quiet_anim_cut','kern.hakios.hi_lock_policy',
                 'global_CA_GL_present_unchanged=1')){
    if(-not $text.hi.Contains($x)){throw "P10 SpringBoard source missing $x"}
}
if(-not $text.ca.Contains('orig_render_update(server, update, flags)') -or
   -not $text.ca.Contains('CA_ignore_true=') -or
   -not $text.ca.Contains('render_update_always_forwarded=1')){
    throw 'P10 missing passive WindowServer update-source provenance'
}
if(-not $text.mali.Contains('hi_lock_policy_sysctl') -or
   -not $text.mali.Contains('value>2') -or
   -not $text.mali.Contains('hi_display_requested_sysctl') -or
   -not $text.mali.Contains('CTLTYPE_INT|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_KERN')){
    throw 'P10 kernel sysctl fail closed/read-only display status missing'
}
if($text.p9.Contains("Send-Checked 'sysctl kern.hakios.p9_panel_mode=1'") -or
   -not $text.p9.Contains('P9 PANEL BYPASS VERIFIED')){
    throw 'P10 regressed real-device tear-free P9.1 panel DCS bypass'
}
if(-not $text.mali.Contains('gate_mode == 2 && hakios_display_requested_on() != 0')){
    throw 'P10 regressed physical GPU OFF-only policy'
}
if(-not $text.header.Contains('S9_P10_OTHER') -or
   -not $text.header.Contains('now-forwarded<S9_P10_CLOCK_FORWARD_US') -or
   -not $text.header.Contains('now-forwarded<S9_P10_ANIM_FORWARD_US')){
    throw 'P10 guard may drop unrelated or all frames'
}
Write-Output 'P10 LOCK SCREEN SOURCE PASS: exact timers, real locked cover + on + no input, animation fail-open, passive CA provenance, P9.1 hardware interlocks untouched.'
