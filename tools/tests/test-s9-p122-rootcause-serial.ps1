#requires -Version 5.1
Set-StrictMode -Version 2
$ErrorActionPreference='Stop'
$tools=Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$p=Join-Path $tools 's9-p122-rootcause-serial.ps1'
$parent=Join-Path $tools 's9-p12-dual-serial.ps1'
$life=Join-Path $tools 's9-screen-lifecycle-serial.ps1'
foreach($f in @($p,$parent,$life)){if(-not(Test-Path -LiteralPath $f)){throw "Missing P12.2 companion $f"}}
$preview=& powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $p 2>&1 | Out-String
if($LASTEXITCODE -ne 0 -or $preview -notmatch 'P12.2 PREVIEW' -or
   $preview -notmatch 'SAFE no-swipe' -or $preview -notmatch 'Minimal'){
    throw "P12.2 preview failure $preview"
}
$root=Get-Content -Raw -LiteralPath $p
$source=Get-Content -Raw -LiteralPath $parent
$mali=Get-Content -Raw -LiteralPath (Join-Path $tools '../drivers/gpu/mali/hakios.mali.c')
$eagl=Get-Content -Raw -LiteralPath (Join-Path $tools '../userland/src/eagl.c')
foreach($needle in @(
 'P12-CoolStart 12','P12-QuiesceHI2','P12-EarlyThermalGuard',
 'P122-Profile 0','P122-Profile 1','P122-Profile 2',
 'ROOT_MIN_A','ROOT_STAGE','ROOT_DETAIL','ROOT_MIN_B',
 'P122GpuSnapshots','p122-gl-window:',"P12-Query 'p122_diag_mode' '0' -Cleanup",
 'P122-Switch 0','P12-ObserveIdle','MicroGesture','P12HiDirty',
 'Life-CheckSafety $now','p122=1'
)){
 if(-not $root.Contains($needle)){throw "Missing P12.2 safety or source test $needle"}
}
foreach($needle in @('p122_diag_mode_sysctl','p122_gpu_stats_sysctl','p122_job_latency',
 'p122_gov_forced','p122_job_q_hist','p122_job_e_hist')){
 if(-not $mali.Contains($needle)){throw "Missing Mali observer $needle"}
}
foreach($needle in @('p122-gl: mode=%d ack=kernel_sysctl','p122-gl-window:',
 'p122_thread_cpu_us','CLOCK_THREAD_CPUTIME_ID','s9_p122_frame_add')){
 if(-not $eagl.Contains($needle)){throw "Missing GLES observer $needle"}
}
if($root -match "P12-Query 'mali_gov_frame' '[0-9]+'" -or
   $root -match "P12-Query 'p9_auto_gpu' '1'" -or
   $root -match "P12-Query 'p9_panel_mode' '1'" -or
   $root -match 'sleep_cores=1'){
 throw 'P12.2 profiler must NEVER change GPU, panel or CPU sleep policies'
}
. $p -LibraryOnly -WfiProbe
$start='version=2 mode=1 windows=100 busy_ns=1000000 elapsed_ns=10000000 frames=50 forced=40 level260_ns=3000000 level455_ns=7000000 level598_ns=0 other_ns=0 jobs=10 queue_us=2000 run_us=4000'
$end='version=2 mode=1 windows=200 busy_ns=3000000 elapsed_ns=20000000 frames=150 forced=120 level260_ns=5000000 level455_ns=15000000 level598_ns=0 other_ns=0 jobs=20 queue_us=8000 run_us=14000'
$delta=P122-Deltas $start $end
if($delta.Windows -ne 100 -or $delta.BusyPct -ne 20 -or
   $delta.ForcedPct -ne 80 -or $delta.Residency455Pct -ne 80 -or
   $delta.QueueAvgUs -ne 600 -or $delta.RunAvgUs -ne 1000){
    throw 'P12.2 GPU per-phase cumulative counter deltas wrong'
}
try{[void](P122-Deltas $end $start);throw 'P12.2 accepted counter reset'}
catch{if($_.Exception.Message -notmatch 'went backwards'){throw}}
if(-not $WfiProbe){throw 'P12.2 WfiProbe requested but lost during P12.1 import'}
$script:LifeSynced=$true
$base=[datetime]::Parse('2026-11-24T16:30:00Z').ToUniversalTime()
$script:P122Await=2
$script:P12Phase='ROOT_DETAIL'
Life-Feed 'p122-gl: mode=2 ack=kernel_sysctl mode0=minimal' $base
if(-not $script:P122Ack){throw 'Missing verified backboardd profile ACK'}
Life-Feed 'p122-gl-window: mode=2 frames=60 wall_mean_us=15000 wall_max_us=48000 p50bin=2 p95bin=3 p99bin=6 cpu_samples=4 cpu_sample_us=12000 wall_sample_us=49000 draws=1200 uploads=12 calls=18000 wall_not_GPU_time=1 thread_cpu_best_effort=1' $base.AddSeconds(1)
if($script:P122GlWindows.Count -ne 1 -or $script:P122GlWindows[0].CpuSamples -ne 4 -or
   $script:P122GlWindows[0].MeanUs -ne 15000 -or
   $script:P122GlWindows[0].Phase -ne 'ROOT_DETAIL'){
 throw 'P12.2 GL sampled-CPU, frame histogram and phase parsing failed'
}
$script:P12HiStopConfirmed=$true
Life-Feed 'hi: auto=cut1 up=12s' $base.AddSeconds(2)
if(-not $script:P122HiOverlap){throw 'P12.2 failed to abort HI2 rearm during causal test'}
$script:P122HiOverlap=$false
$script:LifeSerial=New-Object psobject
$script:LifeSerial | Add-Member -MemberType ScriptMethod -Name Write -Value {param($data,$offset,$len) $null=$data;$null=$offset;$null=$len}
function Life-Pump {
 if(-not $script:P12Command){return}
 $cmd=$script:P12Command
 $name=$cmd.Substring(('sysctl kern.hakios.').Length).Split('=')[0]
 $value=$(if($cmd.Contains('=')){$cmd.Split('=')[-1]}else{'0'})
 Life-Feed ('usb> '+$cmd) (Life-Now)
 Life-Feed ('usb: kern.hakios.'+$name+' = '+$value) (Life-Now)
 if($name -eq 'p122_diag_mode'){
    Life-Feed ('p122-gl: mode='+$value+' ack=kernel_sysctl') (Life-Now)
 }
}
$script:LifeLastCpuAt=Life-Now;$script:LifeLastGpuAt=Life-Now
$script:LifeLastBig=84;$script:LifeLastLittle=81;$script:LifeLastGpuC=80
try{Life-CheckSafety (Life-Now);throw 'P12.1 hard thermal guard missed'}
catch{if($_.Exception.Message -notmatch 'temperature'){throw}}
# The profiler MUST be restored even while the device is already hot;
# existing hot samples cannot mask a valid UART echo/readback.
[void](P12-Query 'p122_diag_mode' '0' -Cleanup)
try{[void](P12-Query 'p122_diag_mode' '1' -Cleanup);throw 'Unsafe nonzero cleanup accepted'}
catch{if($_.Exception.Message -notmatch 'Cleanup bypass'){throw}}
try{[void](P12-Query 'p122_diag_mode' '3');throw 'Out-of-range profiler mode accepted'}
catch{if($_.Exception.Message -notmatch 'only permits'){throw}}
try{[void](P12-Query 'mali_gov_frame' '260');throw 'Unauthorized GPU clock write accepted'}
catch{if($_.Exception.Message -notmatch 'disallowed'){throw}}
$script:LifeLastBig=67;$script:LifeLastLittle=66;$script:LifeLastGpuC=66
$script:P122Ack=$false
P122-Switch 1
if($script:P122CurrentMode -ne 1 -or -not $script:P122Ack -or -not $script:P122ModeDirty){
 throw 'P12.2 stage profiler mode did not require verified backboardd ACK'
}
P122-Switch 0
if($script:P122CurrentMode -ne 0 -or -not $script:P122Ack -or $script:P122ModeDirty){
 throw 'P12.2 minimal re-entry/rollback did not confirm ACK'
}
Write-Output 'P12.2 ROOT CAUSE PASS: safe default, reversible 0/1/2 and hot mode0 cleanup, live GL ACK, matched 60-frame CPU-wall data, HI2 interlock and power-write refusal'
