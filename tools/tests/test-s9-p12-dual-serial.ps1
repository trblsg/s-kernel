#requires -Version 5.1
Set-StrictMode -Version 2
$ErrorActionPreference='Stop'
$dir=Split-Path -Parent $MyInvocation.MyCommand.Path
$tools=Split-Path -Parent $dir
$p=Join-Path $tools 's9-p12-dual-serial.ps1'
$life=Join-Path $tools 's9-screen-lifecycle-serial.ps1'
foreach($f in @($p,$life)){if(-not(Test-Path $f)){throw "P12 test missing $f"}}
$preview=& powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $p 2>&1 | Out-String
if($LASTEXITCODE -ne 0 -or $preview -notmatch 'P12 PREVIEW' -or
    $preview -notmatch 'NO forced screen ON' -or
    $preview -notmatch 'p12_cpu_mode=0/1'){
    throw "P12 preview failure: $preview"
}
$body=Get-Content -Raw -LiteralPath $p
$cpu=Get-Content -Raw -LiteralPath (Join-Path $tools '../drivers/power/exynos/hakios.exynos_cpufreq.c')
$gpu=Get-Content -Raw -LiteralPath (Join-Path $tools '../drivers/gpu/mali/hakios.mali_lab.h')
$gmain=Get-Content -Raw -LiteralPath (Join-Path $tools '../drivers/gpu/mali/hakios.mali.c')
$xnu=Get-Content -Raw -LiteralPath (Join-Path $tools '../xnu/files/cpu.c')
$bb=Get-Content -Raw -LiteralPath (Join-Path $tools '../userland/src/inject.c')
if($body -notmatch 'P12-ObserveIdle' -or $body -notmatch 'P12-Observe .A0.' -or
   $body -notmatch 'P12-Observe .B.' -or $body -notmatch 'P12-Observe .A1.' -or
   $body -notmatch "P12-Query 'g3d_lab_stats'" -or
   $body -notmatch "P12-Query 'p12_cpu_mode' '0'" -or
   $body -notmatch 'Life-SyncReadOnly'){
    throw 'P12 source A/B/idle loop or fail-closed restore missing'
}
if($body -match "P12-Query 'p9_auto_gpu' '1'" -or
   $body -match "P12-Query 'p9_panel_mode' '1'" -or
   $body -match "P12-Query 'p7_cpu_park' '1'" -or
   $body -match 'button lock'){
    throw 'P12 host must not touch GPU/CPU power, panel DCS, or synthetic lock'
}
if($cpu -notmatch 's9_p12_cpu_eligible' -or
   $cpu -notmatch 'p12_cpu_mode_sysctl' -or
   $cpu -notmatch 'p12_wfi_stats_sysctl' -or
   $cpu -notmatch 'p12_late_until' -or
   $cpu -notmatch 'p12_frame_streak'){
    throw 'P12 kernel CPU eligibility and WFI safe instrumentation missing'
}
if($gpu -notmatch 'p12_gpu_boot_enable' -or
   $gpu -notmatch 'g3d_lab_gpu_safe &&' -or
   $gpu -notmatch 'g3d_lab_protection_ok' -or
   $gpu -notmatch 'g3d_lab_idle_ms = 10000;' -or
   $gpu -notmatch 'p12_gpu_boot_stats_sysctl' -or
   $gmain -notmatch 'p12_gpu_boot_auto == 1' -or
   $gmain -notmatch 'if \(p12_gpu_boot_auto\) p12_gpu_boot_enable'){
    throw 'P12 GPU boot P9 safety path missing'
}
if($xnu -notmatch 'hakios_p12_wfi_probe_enable' -or
   $xnu -notmatch '__builtin_arm_wfi\(\)' -or
   $xnu -match 'psci_cpu_suspend\(.*p12' -or
   $bb -notmatch 'p12-ca: forwarded=' -or
   $bb -notmatch 'orig_render_update\(server, update, flags\)'){
    throw 'P12 WFI passive or CA forwarded-only instrumentation missing'
}
# Parser tests do NOT need a real USB port.
. $p -LibraryOnly
$base=[datetime]::Parse('2026-11-24T12:00:00Z').ToUniversalTime()
$script:P12Command='sysctl kern.hakios.p12_cpu_mode=1'
Life-Feed 'usb: kern.hakios.p12_cpu_mode = 0' $base
if($null -ne $script:P12Reply){throw 'P12 readback accepted without matched command echo'}
Life-Feed 'usb> sysctl kern.hakios.p12_cpu_mode=1' $base.AddSeconds(1)
Life-Feed 'usb: kern.hakios.p12_cpu_mode = 1' $base.AddSeconds(1)
if(-not $script:P12Echo -or $script:P12Reply -ne '1'){
    throw "P12 echo+readback parser failed: echo=$($script:P12Echo), reply=$($script:P12Reply)"
}
$script:LifeSynced=$true
$script:P12Phase='B'
Life-Feed 'p12-ca: forwarded=256 mean_render_us=470 max_render_us=1240 skipped=0 GL_present_unchanged=1' $base.AddSeconds(2)
if(-not $script:P12CA.ContainsKey('B') -or
   $script:P12CA['B'].MeanSum -ne 470 -or $script:P12CA['B'].Max -ne 1240){
    throw 'P12 CA cost observer dropped actual timing'
}
Write-Output 'P12 DUAL SOURCE PASS: source A/B/idle, GPU fail-closed boot, WFI observational, CA forwarding, USB echo+readback.'
