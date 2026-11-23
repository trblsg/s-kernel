# P9 functional power fixes on the already paired P9 BOOT+SYSTEM.
# Direct COM reuses P8 echo+kernel ACK+exact readback. This is NOT P8
# diagnostics: P9 toggles actual panel DCS and a validated GPU power gate.
function P9-Restore-Policy {
    # GPU may be physically OFF; its own sysctl restores/validates SMC,
    # PLL, MMU, GPU core and TZ state BEFORE clearing the power gate.
    Send-Checked 'sysctl kern.hakios.p9_auto_gpu=0'
    Send-Checked 'sysctl kern.hakios.p9_wake_guard=0'
    Send-Checked 'sysctl kern.hakios.p9_touch_off_ms=0'
    Send-Checked 'sysctl kern.hakios.p9_usb_off_ms=0'
    Send-Checked 'sysctl kern.hakios.hi_policy=0'
    # The panel sysctl refuses a screen-OFF disable; wake visibly first.
    Send-Checked 'sysctl kern.hakios.p9_panel_mode=0'
    Send-Checked 'sysctl kern.hakios.g3d_lab_stats'
    Send-Checked 'sysctl kern.hakios.p9_panel_stats'
}

function Run-P9Fix {
    Say ('S9 P9 Screen-OFF POWER FIX mode='+$(if($P9Restore){'RESTORE'}else{'APPLY & VERIFY'}))
    if($P9Restore){
        # Restoring is permitted with P9 diagnostics disabled. The operator
        # should unlock/illuminate the panel before disabling its DCS path.
        Send-Checked 'sysctl kern.hakios.p9_panel_stats'
        if($script:Recent -match 'display_requested=0'){
            Say 'P9 restore: panel currently OFF, requesting ON before disable'
            $script:DisplayOn=$false
            Send-Checked 'button lock'
            Wait-For 'P9 restore visible display request ON' 30 {$script:DisplayOn}
        }
        P9-Restore-Policy
        $script:Completed=$true
        Say 'P9 policies RESTORED: GPU/Panel/Touch/USB/Clock/Wake guard OFF.'
        return
    }
    # Preflight is read-only/telemetry: no CPU_OFF, no voltage changes.
    Send-Checked 'sysctl kern.hakios.p7_cpu_mode=1'
    Send-Checked 'sysctl kern.hakios.p7_gpu_mode=1'
    Send-Checked 'sysctl kern.hakios.g3d_lab_rail_probe=1'
    Wait-For 'P9 fresh 4 LITTLE + 4 BIG online, display ON' 18 {
        $null -ne $script:LastCpuSample -and
        $script:LastCpuSample.LittleOnline -eq 4 -and
        $script:LastCpuSample.BigOnline -eq 4 -and
        $script:LastCpuSample.Display -eq 1
    }
    Wait-For 'P9 fresh G3D PMU safely ON' 18 {
        $null -ne $script:LastGpuSample -and
        $script:LastGpuSample.Domain -eq 1 -and
        $script:LastGpuSample.PLL -eq 1 -and
        $script:LastGpuSample.Safe -eq 1 -and
        $script:LastGpuSample.TzOk -eq 1 -and
        $script:LastGpuSample.Fault -eq 0
    }
    Send-Checked 'sysctl kern.hakios.p9_panel_stats'
    if($script:Recent -notmatch 'panel_ready=1'){
        throw 'P9 panel driver not ready: use matched P9 image with panel=1. No power policy enabled.'
    }
    if($null -ne $script:BigC -and $script:BigC -ge 75){
        throw "P9 preflight BIG $($script:BigC)C must cool below 75C before physical screen wake"
    }
    $script:PowerThermalArmed=$true
    Say 'P9 PREFLIGHT PASS: 8 online cores, G3D/PLL/TZ safe, Samsung panel initialized.'
    # No G3D power writes yet; enable lower-work policies while display ON.
    Send-Checked 'sysctl kern.hakios.p9_touch_off_ms=32'
    Send-Checked 'sysctl kern.hakios.p9_usb_off_ms=8'
    Send-Checked 'sysctl kern.hakios.p9_wake_guard=1'
    Send-Checked 'sysctl kern.hakios.hi_policy=2'
    Send-Checked 'sysctl kern.hakios.p9_panel_mode=1'
    Send-Checked 'sysctl kern.hakios.p9_auto_gpu=1'
    Say 'P9 ACTIVE: Samsung DCS sleep, G3D real OFF after 10s, touch32ms/USB8ms only OFF, Clock hidden-link, 4s BIG wake cap.'
    Rest 'P9 ON stable before first real OFF/ON' 10
    $script:P9PanelOffAck=$false
    $script:P9PanelOnAck=$false
    $script:DisplayOff=$false
    $offBefore=$script:LastGpuSample.OffCount
    $wakeBefore=$script:LastGpuSample.WakeCount
    Send-Checked 'button lock'
    Wait-For 'P9 actual display OFF request' 30 {$script:DisplayOff} -Thermal
    Wait-For 'P9 panel DCS 28+10 sent' 20 {$script:P9PanelOffAck} -Thermal
    Wait-For 'P9 G3D PMU domain=0 PLL=0, no GPU faults' 50 {
        $null -ne $script:LastGpuSample -and
        $script:LastGpuSample.Domain -eq 0 -and
        $script:LastGpuSample.PLL -eq 0 -and
        $script:LastGpuSample.OffCount -gt $offBefore -and
        $script:LastGpuSample.Fault -eq 0
    } -Thermal
    Rest 'P9 real panel+GPU sleep residency; USB console responsive' 20
    $script:DisplayOn=$false
    Send-Checked 'button lock'
    Wait-For 'P9 actual display ON request' 30 {$script:DisplayOn} -Thermal
    Wait-For 'P9 panel DCS 11+29 sent' 30 {$script:P9PanelOnAck} -Thermal
    Wait-For 'P9 G3D recovery domain=1 PLL=1 safe=1 TZ=1, 8 CPUs online' 35 {
        $null -ne $script:LastGpuSample -and $null -ne $script:LastCpuSample -and
        $script:LastGpuSample.Domain -eq 1 -and
        $script:LastGpuSample.PLL -eq 1 -and
        $script:LastGpuSample.Safe -eq 1 -and
        $script:LastGpuSample.TzOk -eq 1 -and
        $script:LastGpuSample.WakeCount -gt $wakeBefore -and
        $script:LastGpuSample.WakeFail -eq 0 -and
        $script:LastGpuSample.Fault -eq 0 -and
        $script:LastCpuSample.LittleOnline -eq 4 -and
        $script:LastCpuSample.BigOnline -eq 4
    } -Thermal
    Send-Checked 'sysctl kern.hakios.g3d_lab_stats'
    Send-Checked 'sysctl kern.hakios.p9_panel_stats'
    Say 'P9 REAL HARDWARE PASS: G3D physically OFF then recovered, panel DCS off/on, 8 CPU online.'
    # These are diagnostic-only ring switches, NOT the functional policies.
    Send-Checked 'sysctl kern.hakios.p7_cpu_mode=0'
    Send-Checked 'sysctl kern.hakios.p7_gpu_mode=0'
    Send-Checked 'sysctl kern.hakios.g3d_lab_rail_probe=0'
    $script:Completed=$true
    Say 'P9 POLICIES LEFT ENABLED after successful wake test. Use -P9Restore to undo; reboot returns defaults.'
}
