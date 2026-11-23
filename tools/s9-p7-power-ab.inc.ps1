#requires -Version 5.1
# S9 P7 source-preserving power experiments for the existing matched BOOT+SYSTEM.
# Dot-sourced by s9-home-idle-serial-auto.ps1. No new firmware interfaces.
# G3D and CPU tests are intentionally separate; neither runs Clock A/B.
function P7-Set-Phase([int]$Phase){
    if($Phase -lt 0 -or $Phase -gt 99){throw 'Invalid phase'}
    Send-Checked ("sysctl kern.hakios.p7_cpu_phase="+$Phase)
    Send-Checked ("sysctl kern.hakios.p7_gpu_phase="+$Phase)
    Say ("P7 PHASE $Phase ACKED (CPU+GPU; marking only)")
}
function P7-Parse-G3DStats([string]$Text){
    # Pure parser: the USB reply must include every PMU/safety field.
    # Called by both actual COM experiment and offline unit replay.
    $matching=[regex]::Matches($Text,'(?m)^usb: kern\.hakios\.g3d_lab_stats = ([^\r\n]+)')
    if($matching.Count -eq 0){throw 'G3D readback absent'}
    $state=@{}
    $body=$matching[$matching.Count-1].Groups[1].Value
    foreach($match in [regex]::Matches($body,'([a-z][a-z0-9_]*)=(\d+)')){
        $state[$match.Groups[1].Value]=[long]$match.Groups[2].Value
    }
    foreach($key in @('mode','requested_mv','domain','pll','gpu_safe','tz_ok',
                      'fault','off_count','off_total_ms','wake_count','wake_fail','rail_valid','rail_last_mv')){
        if(-not $state.ContainsKey($key)){throw "Incomplete G3D status: $key absent from $body"}
    }
    return $state
}
function P7-Read-G3DStats{
    Send-Checked 'sysctl kern.hakios.g3d_lab_stats'
    return P7-Parse-G3DStats $script:Recent
}
function P7-Assert-Safe-G3D([hashtable]$S){
    foreach($field in @('domain','pll','gpu_safe','tz_ok')){
        if($S[$field] -ne 1){throw "GPU safety baseline: $field=$($S[$field]) expected=1"}
    }
    if($S.mode -ne 0 -or $S.fault -ne 0 -or $S.wake_fail -ne 0){
        throw "GPU safety baseline: mode=$($S.mode) fault=$($S.fault) wake_fail=$($S.wake_fail) (no automatic reset)"
    }
    if($S.requested_mv -ne 950 -or $S.rail_valid -ne 1 -or $S.rail_last_mv -ne 950){
        throw "GPU BUCK6 950 mV not independently verified: requested=$($S.requested_mv), measured=$($S.rail_last_mv), rail_valid=$($S.rail_valid)"
    }
}
function P7-CpuSample([int]$Phase,[int]$Disp){
    return $null -ne $script:LastCpuSample -and
        $script:LastCpuSample.Phase -eq $Phase -and
        $script:LastCpuSample.Display -eq $Disp -and
        ([datetime]::UtcNow-$script:LastCpuSample.When).TotalSeconds -lt 12
}
function P7-GpuSample([int]$Phase){
    return $null -ne $script:LastGpuSample -and
        $script:LastGpuSample.Phase -eq $Phase -and
        ([datetime]::UtcNow-$script:LastGpuSample.When).TotalSeconds -lt 12
}
function P7-SwitchDisplay([int]$Target,[int]$Phase){
    if($Target -ne 0 -and $Target -ne 1){throw 'Invalid display target'}
    if(P7-CpuSample $Phase $Target){
        Say ("Display already at requested state disp=$Target from fresh CPU sample")
        return
    }
    $script:DisplayOff=$false;$script:DisplayOn=$false
    Send-Checked 'button lock'
    if($Target -eq 0){
        Wait-For 'GenericARMPE display OFF request' 34 {$script:DisplayOff} -Thermal
    }else{
        Wait-For 'GenericARMPE display ON request' 40 {$script:DisplayOn} -Thermal
    }
    Wait-For ("fresh P7 CPU disp=$Target phase=$Phase") 24 {
        P7-CpuSample $Phase $Target
    } -Thermal
    Say ("Display request $Target verified by BOTH GenericARMPE and fresh CPU P7")
}
function P7-VerifyCpuRestored{
    foreach($id in $script:ParkedCPUIds.Keys){
        if(-not $script:RestoredCPUIds.ContainsKey($id)){
            throw "CPU $id was parked but has no exact successful started-again log"
        }
    }
    if($script:ParkedCPUIds.Count -eq 0){throw 'No CPU_OFF ever confirmed'}
    Wait-For 'P7 all BIG cores online again after park=0' 28 {
        $null -ne $script:LastCpuSample -and
        $script:LastCpuSample.BigOnline -eq 4 -and
        $script:LastCpuSample.LittleOnline -eq 4 -and
        ([datetime]::UtcNow-$script:LastCpuSample.When).TotalSeconds -lt 12
    } -Thermal
    Say ("P7 CPU_ON VERIFIED: parked and restored same IDs {0}, BIG=4/4 LITTLE=4/4" -f (($script:ParkedCPUIds.Keys|Sort-Object) -join ','))
}
function P7-Start-PowerPreflight{
    Say "P7 $P7PowerTest real power A/B/A: same firmware; never run HI2 Clock A/B."
    Send-Checked 'sysctl kern.hakios.hi_policy=0'
    Send-Checked 'sysctl kern.hakios.p7_cpu_park=0' 'p7-cpu: park_requested=0'
    Send-Checked 'sysctl kern.hakios.p7_cpu_mode=1' 'p7-cpu: mode=1'
    Send-Checked 'sysctl kern.hakios.p7_gpu_mode=1' 'p7-gpu: mode=1'
    Send-Checked 'sysctl kern.hakios.g3d_lab_rail_probe=1' 'g3d-lab: 30-second read-only PMIC sampler=1'
    Send-Checked 'sysctl kern.hakios.p7_cpu_stats'
    Send-Checked 'sysctl kern.hakios.p7_gpu_stats'
    # Never silently reset a fault or alter voltage: the intervention
    # must begin with healthy domain/PLL/TZ and verified BUCK6 voltage.
    Rest 'P7 sensor/PMU warm-up for first fresh 5s snapshot' 12
    if($null -eq $script:LastCpuSample -or $null -eq $script:LastGpuSample){
        throw 'P7 did not provide fresh CPU and G3D PMU samples'
    }
    if($null -eq $script:BigC -or $null -eq $script:LittleC -or
       $null -eq $script:G3DC -or
       $script:CpuThermalWhen -eq [datetime]::MinValue){
        throw 'P7 thermal sensors missing BIG/LITTLE/G3D values; real power test blocked'
    }
    if(([datetime]::UtcNow-$script:CpuThermalWhen).TotalSeconds -gt 12 -or
       ([datetime]::UtcNow-$script:LastGpuSample.When).TotalSeconds -gt 12){
        throw 'P7 thermal readings are stale before hardware power experiment'
    }
    $script:PowerThermalArmed=$true
    Check-Thermal
    $s=P7-Read-G3DStats
    P7-Assert-Safe-G3D $s
    Say ("P7 PREFLIGHT SAFE: G3D PMU=ON PLL=ON BUCK6=950mV DTZPC=1 no faults; BIG=$($script:BigC)C LITTLE=$($script:LittleC)C G3D=$($script:G3DC)C")
}
function P7-Open-QuietHome{
    # Shared CPU/G3D baseline. IMPORTANT: never open Settings via
    # hi_cmd=300/301 during standalone P7 tests. SpringBoard only permits
    # those after the HI2 auto crossover reaches FINISHED. The real
    # 2026-11-23 CPU log recorded auto_state=aborted and a refusal.
    # The secondary Home route is proven on the same flashed P7 image.
    # A HOME+one left-swipe is the existing tested navigation sequence.
    # We additionally require real foreground Home and >=15s without
    # jobs/frames to avoid power-gating while animating.
    Send-Checked 'button home'
    Rest 'SpringBoard foreground settle' 6
    Send-Checked 'swipe l 1 350 1000'
    Rest 'Home secondary animation settle' 8
    Send-Checked 'sysctl kern.hakios.hi_cmd=2' 'hi: mark=secondary'
    Wait-For 'quiet Home: frontmost Home, no GPU frame or job for >=15 seconds' 65 {
        $null -ne $script:LastGpuSample -and
        $script:SceneHome -and -not $script:SceneSettings -and
        $script:LastGpuSample.Domain -eq 1 -and
        $script:LastGpuSample.FrameAge -ge 15000 -and
        $script:LastGpuSample.JobAge -ge 15000 -and
        ([datetime]::UtcNow-$script:LastGpuSample.When).TotalSeconds -lt 12 -and
        ([datetime]::UtcNow-$script:ClockLastObserved).TotalSeconds -ge 15
    } -Thermal
    Say 'QUIET HOME VERIFIED: 15s frame/job inactivity and no Clock 10Hz; exact page number is not exposed by current SpringBoard hook.'
}
function P7-Run-G3DAB{
    P7-Open-QuietHome
    P7-Set-Phase 50
    Rest 'A1 G3D alwaysON: quiet Home reference' $PowerWindowSeconds
    Check-Thermal
    $before=P7-Read-G3DStats
    P7-Assert-Safe-G3D $before
    if(-not (P7-GpuSample 50)){throw 'A1 G3D reference PMU sampler is stale'}
    if($script:LastGpuSample.JobAge -lt 15000 -or $script:LastGpuSample.FrameAge -lt 15000){
        throw 'A1 GPU busy: unsafe to enable idle power-gating'
    }
    $script:G3DOffCountBaseline=[int]$before.off_count
    $script:G3DOffTotalBaseline=[long]$before.off_total_ms
    $script:G3DOffSamples=0;$script:G3DGateSamples=0
    $script:G3DLastOffTotal=0L;$script:G3DOffCountObserved=0
    P7-Set-Phase 51
    # Existing driver enforces 950 mV, no DVS, GPU-safe and TZ-safe.
    # Under gate=3 its 120-second independent kernel watchdog expires
    # even if the Windows COM disappears (not a guarantee against deadlock).
    Send-Checked 'sysctl kern.hakios.g3d_lab_idle_ms=10000'
    Send-Checked 'sysctl kern.hakios.g3d_lab_gate=3' 'g3d-lab: gate mode=3'
    Wait-For 'G3D PMU domain OFF and PLL OFF, >=2 independent 5s samples' 37 {
        $script:G3DOffSamples -ge 2 -and
        (P7-GpuSample 51) -and $script:LastGpuSample.Domain -eq 0 -and
        $script:LastGpuSample.PLL -eq 0
    } -Thermal
    # From gate=3 to cleanup < 120s, even when the OFF proof needs 37s.
    Rest 'B G3D physical OFF residence, thermal and CPU comparison' $PowerWindowSeconds
    $off=$script:G3DLastOffTotal-$script:G3DOffTotalBaseline
    if($script:G3DOffCountObserved -le $script:G3DOffCountBaseline -or
       $script:G3DOffSamples -lt 2 -or $off -lt 15000){
        throw "GPU gating inconclusive: off_samples=$($script:G3DOffSamples), off_count=$($script:G3DOffCountObserved), delta_residency=$off ms"
    }
    if($script:G3DGateSamples -lt 6 -or
       $script:G3DOffSamples -lt [Math]::Ceiling($script:G3DGateSamples/2.0)){
        throw "G3D truly OFF, but less than 50 percent of B window was observed OFF; cannot compare sustained base power"
    }
    Say ("P7 G3D OFF VERIFIED: off_samples={0}/{1}, off_count {2}->{3}, off_residency_delta={4}ms" -f
         $script:G3DOffSamples,$script:G3DGateSamples,
         $script:G3DOffCountBaseline,$script:G3DOffCountObserved,$off)
    Send-Checked 'sysctl kern.hakios.g3d_lab_gate=0' 'g3d-lab: gate mode=0'
    Confirm-G3DRestored
    $after=P7-Read-G3DStats
    P7-Assert-Safe-G3D $after
    if($after.off_count -le $before.off_count -or
       $after.off_total_ms -lt $before.off_total_ms+15000 -or
       $after.wake_count -le $before.wake_count){
        throw 'G3D OFF/ON counters did not confirm actual saved off residency and wake'
    }
    Say ("G3D ON VERIFIED counters: off_count=$($after.off_count), off_total_ms=$($after.off_total_ms), wakes=$($after.wake_count), wake_fail=$($after.wake_fail)")
    P7-Set-Phase 52
    Rest 'A2 G3D restored alwaysON: same quiet Home reference' $PowerWindowSeconds
    $final=P7-Read-G3DStats
    P7-Assert-Safe-G3D $final
    Say 'P7 G3D A/B/A COMPLETED with PMU+PLL OFF proof, actual off-residency and verified wake'
}
function P7-Run-CPUAB{
    Say 'P7 CPU A/B/A: use quiet secondary Home, never HI2-gated Settings URL'
    P7-Open-QuietHome
    if(-not $script:SceneHome -or $script:SceneSettings){
        throw 'CPU A/B aborted: secondary Home foreground proof was lost'
    }
    $g=P7-Read-G3DStats
    P7-Assert-Safe-G3D $g
    if($null -eq $script:LastCpuSample -or $script:LastCpuSample.Display -ne 1 -or
       $script:LastCpuSample.LittleOnline -ne 4 -or $script:LastCpuSample.BigOnline -ne 4){
        throw 'CPU A/B requires screen ON and all eight cores online before phase A1'
    }
    P7-Set-Phase 60
    Wait-For 'screen ON, 4 LITTLE and 4 BIG online before CPU A/B' 24 {
        (P7-CpuSample 60 1) -and $script:LastCpuSample.LittleOnline -eq 4 -and
        $script:LastCpuSample.BigOnline -eq 4
    } -Thermal
    Say 'P7 CPU BASELINE VERIFIED: quiet secondary Home, G3D remains ON, disp=1, LITTLE=4 BIG=4'
    P7-SwitchDisplay 0 60
    Rest 'A1 CPU screen-OFF, BIG all online reference' $PowerWindowSeconds
    if($script:CpuBaselineSamples -lt 2 -or
       -not (P7-CpuSample 60 0) -or $script:LastCpuSample.BigOnline -ne 4 -or
       $script:LastCpuSample.LittleOnline -ne 4){
        throw 'A1 CPU baseline not genuine OFF with 8 online cores'
    }
    P7-SwitchDisplay 1 60
    Rest 'Screen ON and full CPU restore settle' 12
    $script:ParkedCPUIds=@{};$script:RestoredCPUIds=@{}
    $script:ParkedCount=0;$script:UnparkedCount=0
    # If boot sleep_cores=1 missing, the kernel returns ENODEV. Never
    # silently test with all cores online and label it CPU_OFF.
    Send-Checked 'sysctl kern.hakios.p7_cpu_park=1' 'p7-cpu: park_requested=1'
    P7-Set-Phase 61
    P7-SwitchDisplay 0 61
    Wait-For 'physical BIG CPU_OFF (success ID + fewer online BIG cores)' 50 {
        $script:ParkedCPUIds.Count -gt 0 -and (P7-CpuSample 61 0) -and
        $script:LastCpuSample.BigOnline -lt 4 -and
        $script:LastCpuSample.LittleOnline -eq 4
    } -Thermal
    Rest 'B CPU screen-OFF with selected BIG cores physically OFF' $PowerWindowSeconds
    if($script:CpuOffSamples -lt 2 -or
       -not (P7-CpuSample 61 0) -or $script:LastCpuSample.BigOnline -ge 4){
        throw 'B CPU_OFF phase lost hardware proof before wake'
    }
    Say ("P7 CPU_OFF VERIFIED parked BIG cores IDs: "+(($script:ParkedCPUIds.Keys|Sort-Object) -join ','))
    P7-SwitchDisplay 1 61
    Send-Checked 'sysctl kern.hakios.p7_cpu_park=0' 'p7-cpu: park_requested=0'
    Wait-For 'every parked BIG core reports successful CPU_ON' 45 {
        if($script:ParkedCPUIds.Count -eq 0){return $false}
        foreach($id in $script:ParkedCPUIds.Keys){
            if(-not $script:RestoredCPUIds.ContainsKey($id)){return $false}
        }
        return $true
    } -Thermal
    P7-VerifyCpuRestored
    P7-Set-Phase 62
    P7-SwitchDisplay 0 62
    Rest 'A2 CPU screen-OFF restored all BIG cores reference' $PowerWindowSeconds
    if($script:CpuRecoverySamples -lt 2 -or
       -not (P7-CpuSample 62 0) -or $script:LastCpuSample.BigOnline -ne 4){
        throw 'A2 CPU restored baseline did not show all BIG cores online'
    }
    P7-SwitchDisplay 1 62
    P7-VerifyCpuRestored
    $g=P7-Read-G3DStats
    P7-Assert-Safe-G3D $g
    Say 'P7 CPU A/B/A G3D CONTROL VERIFIED: G3D gate=0, PMU=ON PLL=ON, no GPU/TZ faults'
    Say ("P7 CPU A/B/A COMPLETED: OFF A1={0} online8 samples; OFF B={1} BIG CPU_OFF samples; OFF A2={2} online8 samples; fully unparked" -f
        $script:CpuBaselineSamples,$script:CpuOffSamples,$script:CpuRecoverySamples)
}
function Run-P7PowerScenario{
    if(-not $P7PowerTest){throw 'No P7 power experiment selected'}
    if($P7Unified -or $ActiveG3DGate -or $ActiveCpuPark -or $DumpP7History -or $LeaveP7AuditRunning){
        throw 'P7PowerTest is standalone: do not combine with full HI2, old active flags or offline history.'
    }
    P7-Start-PowerPreflight
    if($P7PowerTest -eq 'G3D'){P7-Run-G3DAB}
    elseif($P7PowerTest -eq 'CPU'){P7-Run-CPUAB}
    else{throw "Unrecognized P7 hardware mode: $P7PowerTest"}
    Send-Checked 'sysctl kern.hakios.p7_cpu_stats'
    Send-Checked 'sysctl kern.hakios.p7_gpu_stats'
    Send-Checked 'sysctl kern.hakios.g3d_lab_stats'
    $script:PowerExperimentComplete=$true
    $script:Completed=$true
    Say ("P7 $P7PowerTest FULL A/B/A FINISHED; each observed transition confirmed by device feedback")
}
