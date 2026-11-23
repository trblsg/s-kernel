# P8 one-build passive power/thermal/PSCI/PMU integrated scenario.
# Dot-sourced by s9-home-idle-serial-auto.ps1; shares safe COM ACK/readback,
# P7 display/quiet-Home proofs, thermal guards, kernel ring and log writer.
# No command here requests CPU_OFF, CPU_SUSPEND, CPD, SICD or PMIC writes.
function P8-Set-Phase([int]$Phase){
    if($Phase -lt 80 -or $Phase -gt 99){throw 'Invalid P8 phase'}
    foreach($prefix in @('p8_phase','p7_cpu_phase','p7_gpu_phase','p4_phase','p6_phase')){
        Send-Checked ("sysctl kern.hakios.$prefix=$Phase")
    }
    Say "P8 PHASE $Phase ACKED: identical marker P8/P7 CPU/GPU/P4/P6"
}
function P8-Assert-EightCores{
    if($null -eq $script:LastCpuSample -or
       $script:LastCpuSample.LittleOnline -ne 4 -or
       $script:LastCpuSample.BigOnline -ne 4 -or
       ([datetime]::UtcNow-$script:LastCpuSample.When).TotalSeconds -gt 12){
        throw 'P8 SAFETY: no fresh LITTLE=4/BIG=4; no active experiment is permitted'
    }
    if($script:ParkedCount -ne 0){
        throw 'P8 SAFETY: CPU scheduler park observed; no further active experiment'
    }
}
function P8-Read-PsciPmu{
    foreach($cpu in @(3,4,5,6)){
        Send-Checked ("sysctl kern.hakios.p8_psci_cpu=$cpu")
        Send-Checked 'sysctl kern.hakios.p8_psci_stats'
    }
    Send-Checked 'sysctl kern.hakios.p8_pmu_stats'
    Send-Checked 'sysctl kern.hakios.p8_rail_stats'
    Send-Checked 'sysctl kern.hakios.p8_stats'
    Send-Checked 'sysctl kern.hakios.p7_cpu_stats'
    Send-Checked 'sysctl kern.hakios.p7_gpu_stats'
}
function Run-P8Unified{
    Say 'P8 unified: same BOOT+SYSTEM, 10 source groups, CPU_OFF HARD BLOCKED; no destructive CPU power experiments.'
    # All initial writes are idempotent safety restores.
    Send-Checked 'sysctl kern.hakios.hi_policy=0'
    Send-Checked 'sysctl kern.hakios.p7_cpu_park=0'
    Send-Checked 'sysctl kern.hakios.g3d_lab_gate=0'
    Send-Checked 'sysctl kern.hakios.p8_big_cap_khz=0'
    Send-Checked 'sysctl kern.hakios.p7_cpu_mode=1' 'p7-cpu: mode=1'
    Send-Checked 'sysctl kern.hakios.p7_gpu_mode=1' 'p7-gpu: mode=1'
    Send-Checked 'sysctl kern.hakios.g3d_lab_rail_probe=1'
    Send-Checked 'sysctl kern.hakios.p4_mode=1'
    Send-Checked 'sysctl kern.hakios.p4_threads=1'
    Send-Checked 'sysctl kern.hakios.p6_mode=1'
    Send-Checked 'sysctl kern.hakios.p6_flags=31'
    Send-Checked 'sysctl kern.hakios.p610_mode=1'
    Send-Checked 'sysctl kern.hakios.p69_mode=1'
    Send-Checked 'sysctl kern.hakios.p8_flags=1023'
    Send-Checked 'sysctl kern.hakios.p8_mode=2'
    P8-Set-Phase 80
    Rest 'P8 read-only sensor and core warm-up' 16
    P8-Assert-EightCores
    if($null -eq $script:LastGpuSample -or
       ([datetime]::UtcNow-$script:LastGpuSample.When).TotalSeconds -gt 12){
        throw 'P8 SAFETY: no fresh G3D PMU readback'
    }
    $script:PowerThermalArmed=$true
    Check-Thermal
    P8-Read-PsciPmu
    Say 'P8 PREFLIGHT COMPLETE: 8 online, G3D telemetry fresh, PSCI and PMU explicit readbacks'
    P7-Open-QuietHome
    P8-Set-Phase 81
    Rest 'P8 81: quiet Home full passive baseline' $P8WindowSeconds
    P8-Assert-EightCores
    P8-Set-Phase 82
    Send-Checked 'sysctl kern.hakios.p4_threads=2'
    Rest 'P8 82: IRQ/USB/touch, CPU idle and per-thread diagnostics' $P8WindowSeconds
    Send-Checked 'sysctl kern.hakios.p4_threads=1'
    P8-Assert-EightCores
    P8-Set-Phase 83
    P7-SwitchDisplay 0 83
    Rest 'P8 83: display OFF, all 8 cores remain online, no CPU_OFF' ($P8WindowSeconds*2)
    P8-Assert-EightCores
    P8-Set-Phase 84
    P7-SwitchDisplay 1 84
    Rest 'P8 84: display ON wake/recovery control' $P8WindowSeconds
    P8-Assert-EightCores
    P8-Set-Phase 85
    Send-Checked 'sysctl kern.hakios.g3d_lab_gate=1'
    Rest 'P8 85: G3D shadow candidates only, no GPU gate' $P8WindowSeconds
    Send-Checked 'sysctl kern.hakios.g3d_lab_gate=0'
    P8-Assert-EightCores
    P8-Set-Phase 87
    Rest 'P8 87 A1: BIG stock ceiling' $P8WindowSeconds
    P8-Assert-EightCores
    P8-Set-Phase 88
    Send-Checked 'sysctl kern.hakios.p8_big_cap_khz=1469000'
    Rest 'P8 88 B: only lower BIG cap to 1469MHz' $P8WindowSeconds
    Send-Checked 'sysctl kern.hakios.p8_big_cap_khz=0'
    P8-Set-Phase 89
    Rest 'P8 89 A2: BIG stock ceiling restored' $P8WindowSeconds
    P8-Assert-EightCores
    P8-Set-Phase 90
    P8-Read-PsciPmu
    Send-Checked 'sysctl kern.hakios.p8_record'
    Send-Checked 'sysctl kern.hakios.p7_cpu_record'
    Send-Checked 'sysctl kern.hakios.p7_gpu_record'
    if($P8LeaveAuditRunning){
        P8-Set-Phase 91
        Say 'P8 OFFLINE READY: read-only P8+P7 rings retained after cleanup, USB can now be disconnected.'
    }else{
        Say 'P8 VERIFIED: CPU 8 online, no CPU_OFF, big cap restored, G3D shadow off.'
    }
    $script:Completed=$true
}
function Dump-P8-Rings{
    Say "P8 OFFLINE DUMP: freeze P8/P7 observer rings and retrieve up to $P8DumpLimit rows, no CPU or GPU power writes."
    foreach($c in @(
        'sysctl kern.hakios.p8_mode=0',
        'sysctl kern.hakios.p7_cpu_mode=0',
        'sysctl kern.hakios.p7_gpu_mode=0')){
        Send-Checked $c
    }
    Send-Checked 'sysctl kern.hakios.p8_stats'
    $match=[regex]::Match($script:Recent,'(?m)^usb: kern\.hakios\.p8_stats = [^\r\n]*samples=(\d+)')
    if(-not $match.Success){throw 'P8 dump missing ring sample count'}
    $count=[Math]::Min([int]$match.Groups[1].Value,$P8DumpLimit)
    Say ("P8 FROZEN ROWS "+$count)
    for($i=0;$i -lt $count;$i++){
        Send-Checked ("sysctl kern.hakios.p8_slot=$i")
        Send-Checked 'sysctl kern.hakios.p8_record'
    }
    Send-Checked 'sysctl kern.hakios.p7_gpu_stats'
    $gpuMatch=[regex]::Match($script:Recent,'(?m)^usb: kern\.hakios\.p7_gpu_stats = [^\r\n]*samples=(\d+)')
    if(-not $gpuMatch.Success){throw 'P8 dump missing long GPU PMU/PLL ring count'}
    $gpuCount=[Math]::Min([int]$gpuMatch.Groups[1].Value,$P8DumpLimit)
    Say ("P8 FROZEN G3D ROWS "+$gpuCount)
    for($i=0;$i -lt $gpuCount;$i++){
        Send-Checked ("sysctl kern.hakios.p7_gpu_slot=$i")
        Send-Checked 'sysctl kern.hakios.p7_gpu_record'
    }
    P8-Read-PsciPmu
    $script:Completed=$true
}
