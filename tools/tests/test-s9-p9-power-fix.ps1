#requires -Version 5.1
Set-StrictMode -Version 2
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$driver=Split-Path -Parent $root
$runner=Join-Path $root 's9-home-idle-serial-auto.ps1'
$p9=Join-Path $root 's9-p9-power-fix.inc.ps1'
foreach($file in @($runner,$p9)){
  $t=$null;$errors=$null
  [System.Management.Automation.Language.Parser]::ParseFile($file,[ref]$t,[ref]$errors)|Out-Null
  if(@($errors).Count){throw "PowerShell parser errors in $file"}
}
$preview=(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $runner -P9Fix -Port COM125 2>&1|Out-String)
$restore=(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $runner -P9Restore -Port COM125 2>&1|Out-String)
if($preview -notmatch 'P9 SOURCE FIX: PREVIEW' -or
   $preview -notmatch 'NEVER CPU_OFF' -or
   $preview -notmatch 'GPU auto OFF' -or
   $preview -notmatch 'panel DCS OFF' -or
   $restore -notmatch 'P9 RESTORE: PREVIEW'){
    throw 'P9 preview or rollback description missing'
}
$ErrorActionPreference='Continue'
$reject=(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $runner -P9Fix -P8Unified 2>&1|Out-String)
if($LASTEXITCODE -eq 0 -or $reject -notmatch 'standalone'){
    throw 'P9/P8 simultaneous hardware session was not rejected'
}
$reject=(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $runner -P9Fix -P9Restore 2>&1|Out-String)
if($LASTEXITCODE -eq 0 -or $reject -notmatch 'never both'){
    throw 'Conflicting P9 apply and rollback accepted'
}
$ErrorActionPreference='Stop'
$files=@{
 gpu=Join-Path $driver 'drivers/gpu/mali/hakios.mali.c'
 lab=Join-Path $driver 'drivers/gpu/mali/hakios.mali_lab.h'
 cpu=Join-Path $driver 'drivers/power/exynos/hakios.exynos_cpufreq.c'
 panel=Join-Path $driver 'drivers/display/panel/hakios.s6e3ha8.c'
 pe=Join-Path $driver 'xnu/files/GenericARMPE.cpp'
 touch=Join-Path $driver 'drivers/input/touch/hakios.s6sy761.c'
 usb=Join-Path $driver 'drivers/usb/hakios.dwc3.c'
 sb=Join-Path $driver 'userland/src/s9_home_idle.inc'
}
$s=@{};foreach($name in $files.Keys){$s[$name]=Get-Content -LiteralPath $files[$name] -Raw}
foreach($text in @('gate_mode == 2 && hakios_display_requested_on() != 0',
                    'g3d_sleep = 1;','g3d_lab_gate = 2;',
                    'p9_auto_gpu = 1;','g3d_lab_monitor_start()')){
  if(-not $s.gpu.Contains($text) -and -not $s.lab.Contains($text)){
    throw "P9 GPU real-off gate missing: $text"
  }
}
foreach($text in @('!g3d_awake && !g3d_power_on()','p9_auto_gpu = 0;',
                    'g3d_lab_gpu_safe','g3d_lab_protection_ok')){
    if(-not $s.lab.Contains($text)){throw "P9 GPU recovery missing $text"}
}
if(-not $s.pe.Contains('gPanelSleep || hakios_p9_panel_auto_on()') -or
   -not $s.panel.Contains('p9_panel_mode_sysctl') -or
   -not $s.panel.Contains('sleep_in[] = { 0x10 }') -or
   -not $s.panel.Contains('sleep_out[] = { 0x11 }')){
    throw 'P9 panel source display and DCS power integration broken'
}
if(-not $s.touch.Contains('p9_touch_off_ms_sysctl') -or
   -not $s.touch.Contains('hakios_display_requested_on() == 0') -or
   -not $s.usb.Contains('ring_head==ring_tail && hakios_display_requested_on()==0')){
   throw 'P9 touch/USB idle-only polling policy missing'
}
if(-not $s.cpu.Contains('max=MIN(max,1469000u)') -or
   -not $s.cpu.Contains('p9_wake_guard_sysctl') -or
   -not $s.pe.Contains('p8-safety: BIG CPU_OFF REFUSED')){
    throw 'P9 wake cap or P7 M3 CPU_OFF hardware interlock missing'
}
if(-not $s.sb.Contains('s9_p9_hidden_clock_skip') -or
   -not $s.sb.Contains('wallpaper_home_showing()') -or
   -not $s.sb.Contains('hi_p9_hidden_clock_enabled')){
    throw 'P9 guarded Clock source-level fix missing'
}
$safe=Get-Content $p9 -Raw
if(-not $safe.Contains('P9 REAL HARDWARE PASS')){
    throw 'P9 physical OFF+ON validation not part of runtime script'
}
# Hardware regression at 20:17:29 UTC: panel 0x29 showed an image
# BEFORE the first post-idle GPU submit. The safer host version MUST
# never enable DCS sleep automatically. No kernel rebuild required.
if($safe.Contains("Send-Checked 'sysctl kern.hakios.p9_panel_mode=1'") -or
   $safe.Contains("Wait-For 'P9 panel DCS 28+10 sent'") -or
   $safe.Contains("Wait-For 'P9 panel DCS 11+29 sent'")){
    throw 'P9 wake-regression: host still enables dangerous panel DCS sleep'
}
if(-not $safe.Contains("Send-Checked 'sysctl kern.hakios.p9_panel_mode=0'") -or
   -not $safe.Contains('mode=0 panel_sleep=0 panel_asleep=0 display_requested=1') -or
   -not $safe.Contains('P9 PANEL BYPASS VERIFIED') -or
   -not $safe.Contains('$script:P9PanelOffAck -or $script:P9PanelOnAck')){
    throw 'P9 wake-regression: missing panel-DCS OFF guard or fail-closed verification'
}
if($safe.IndexOf("Send-Checked 'sysctl kern.hakios.p9_panel_mode=0'") -gt
   $safe.IndexOf("Send-Checked 'sysctl kern.hakios.p9_auto_gpu=0'")){
    throw 'P9 wake-regression: rollback must disarm panel DCS first'
}
Write-Output 'P9 SOURCE FIX PASS: GPU screen-verified physical OFF, panel DCS runtime bypass, touch/USB, wake cap, Clock allowlist, P7 CPU_OFF EPERM, COM rollback and previews.'
