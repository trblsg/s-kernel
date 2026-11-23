#requires -Version 5.1
# P8 regression: existing real 051827 failure must NOT become a hardware PASS,
# p8 phase telemetry is parsed, CPU active path blocked BEFORE COM opens.
Set-StrictMode -Version 2
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$runner=Join-Path $root 's9-home-idle-serial-auto.ps1'
$report=Join-Path $root 's9-p8-report.ps1'
$good=Join-Path $PSScriptRoot 's9-p8-unified-fixture.log'
$bad=Join-Path $PSScriptRoot 's9-p8-reentry-negative-fixture.log'
$temp=Join-Path $env:TEMP ('s9-p8-fixture-'+[System.Guid]::NewGuid().ToString('N')+'.csv')
try{
    $positive=(& $report -Log $good -CsvPath $temp 2>&1) -join "`n"
    foreach($needle in @('P8 rows=7','GPU PMU rows=2','PSCI_direct=4','PMU_direct=1',
         'big_lit_missing=0','usb_calls_per_s~=494','stock_restored=1',
         'PHASE 87 n=1','PHASE 88 n=1','PHASE 89 n=1',
         'no active CPU_OFF attempted','CSV ')){
        if(-not $positive.Contains($needle)){throw 'P8 positive fixture missing '+$needle}
    }
    if(-not(Test-Path $temp)){throw 'P8 CSV missing'}
    $csv=Import-Csv $temp
    if(@($csv).Count -ne 7){throw 'P8 CSV missing rows'}
    $b=@($csv|Where-Object {$_.phase -eq '88' -and $_.cap_big_mhz -eq '1469'})
    $a=@($csv|Where-Object {($_.phase -eq '87' -or $_.phase -eq '89') -and $_.cap_big_mhz -eq '1794'})
    if($b.Count -ne 1 -or $a.Count -ne 2){throw 'P8 A/B/A phases or frequency ceilings not independently labelled'}
    if(@($csv|Where-Object{$_.physical_cpu_off -ne 'UNPROVEN'}).Count){
        throw 'P8 claimed physical CPU_OFF without proof'
    }
    $negative=(& $report -Log $bad 2>&1) -join "`n"
    foreach($needle in @('P8 CPU RE-ENTRY / POWER FAULT',
         'NO CPU HARDWARE POWER SUCCESS CLAIM','CPU_ON success events=0',
         'big_lit_missing=1')){
        if(-not $negative.Contains($needle)){throw 'P8 negative fixture missed '+$needle}
    }
    if($negative -match 'physical.+PASS'){throw 'False physical CPU OFF PASS'}
    $preview=(& $runner -P8Unified 6>&1 2>&1) -join "`n"
    foreach($needle in @('P8 UNIFIED SAFE LAB: PREVIEW',
        'no M3 CPU_OFF','A/B/A lower-only BIG frequency cap')){
        if(-not $preview.Contains($needle)){throw 'P8 preview missing '+$needle}
    }
    $dump=(& $runner -DumpP8History -P8DumpLimit 8 6>&1 2>&1) -join "`n"
    if(-not $dump.Contains('up to 8 read-only records')){
        throw 'P8 dump preview missing'
    }
    $replay=(& $runner -ReplayP8Log $good 2>&1) -join "`n"
    if(-not $replay.Contains('P8 rows=7')){throw 'P8 replay has no parser output'}
    # 2026-11-23 real COM125: userland p4-thread lines can swallow the start
    # of a genuine USB readback. Load the REAL host function via PowerShell
    # parser, without dot-sourcing the entire runner/opening COM.
    $tokens=$null;$parseErrors=$null
    $ast=[System.Management.Automation.Language.Parser]::ParseFile(
        $runner,[ref]$tokens,[ref]$parseErrors)
    if(@($parseErrors).Count){throw 'P8 serial host parser errors'}
    $fn=@($ast.FindAll({
        param($node)
        $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -eq 'Test-SysctlReadback'
    },$true))
    if($fn.Count -ne 1){throw 'Missing or duplicate live readback parser'}
    . ([scriptblock]::Create($fn[0].Extent.Text))
    $interleaved=Get-Content (Join-Path $PSScriptRoot 's9-p8-usb-interleaved-fixture.log') -Raw
    if(-not (Test-SysctlReadback $interleaved 'kern.hakios.p7_cpu_phase' '82') -or
       -not (Test-SysctlReadback $interleaved 'kern.hakios.p4_mode' '0')){
        throw 'P8 actual CPU phase / p4 cleanup readback lost to concurrent log prefix'
    }
    # Keep all THREE independent proofs: same command echo, kernel set(0)
    # and exact numeric readback; never accept partial numeric values.
    foreach($cmd in @('p7_cpu_phase=82','p4_mode=0')){
        if(-not $interleaved.Contains('usb> sysctl kern.hakios.'+$cmd) -or
           -not $interleaved.Contains('usb: sysctl kern.hakios.'+$cmd+': set (0)')){
            throw 'P8 interleaved USB fixture missing independent echo/ACK'
        }
    }
    $wrongCpu=$interleaved.Replace('usb: kern.hakios.p7_cpu_phase = 82',
                                   'usb: kern.hakios.p7_cpu_phase = 820')
    $wrongP4=$interleaved.Replace('usb: kern.hakios.p4_mode = 0',
                                  'usb: kern.hakios.p4_mode = 01')
    if((Test-SysctlReadback $wrongCpu 'kern.hakios.p7_cpu_phase' '82') -or
       (Test-SysctlReadback $wrongP4 'kern.hakios.p4_mode' '0') -or
       (Test-SysctlReadback $interleaved 'kern.hakios.p7_gpu_phase' '82')){
        throw 'P8 serial parser accepted a wrong, partial or unrelated numeric readback'
    }
    $liveHost=Get-Content $runner -Raw
    if(-not $liveHost.Contains('if($script:Recent.Contains($echo))') -or
       -not $liveHost.Contains('if($script:Recent.Contains($ack))') -or
       -not $liveHost.Contains('if($gotEcho -and $gotAck -and $gotReadback -and $gotExtra)')){
        throw 'P8 host lost the independent echo + kernel ACK + exact readback gate'
    }
    # Child PowerShell isolates the deliberately thrown error from parent Stop.
    $ErrorActionPreference='Continue'
    $rejected=(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $runner -P7PowerTest CPU -Execute 2>&1 | Out-String)
    if($LASTEXITCODE -eq 0 -or -not $rejected.Contains('P8 HARDWARE SAFETY')){
        throw 'Old destructive CPU A/B was not rejected before COM'
    }
    $rejected=(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $runner -P8Unified -ActiveCpuPark -Execute 2>&1 | Out-String)
    if($LASTEXITCODE -eq 0 -or -not $rejected.Contains('P8 HARDWARE SAFETY')){
        throw 'Active CPU park was not rejected before COM'
    }
    $ErrorActionPreference='Stop'
    $src=Get-Content (Join-Path $root '../xnu/files/GenericARMPE.cpp') -Raw
    $hdr=Get-Content (Join-Path $root '../drivers/power/exynos/hakios.p7_cpu_lab.h') -Raw
    $p8source=Get-Content (Join-Path $root '../drivers/power/exynos/hakios.p8_unified_lab.h') -Raw
    $p8runner=Get-Content (Join-Path $root 's9-p8-unified-serial.inc.ps1') -Raw
    $snapshot=Get-Content (Join-Path $root '../drivers/soc/exynos/hakios.p8_soc_snapshot.h') -Raw
    if(-not $src.Contains('p8-safety: BIG CPU_OFF REFUSED') -or
       -not $hdr.Contains('if(value==1)return EPERM;') -or
       -not $p8source.Contains('P8_RING 512u') -or
       -not $snapshot.Contains('off_attempts, off_returns') -or
       -not $p8source.Contains('s.off_returns') -or
       -not $p8runner.Contains('P8-Set-Phase 87') -or
       -not $p8runner.Contains('P8-Set-Phase 88') -or
       -not $p8runner.Contains('P8-Set-Phase 89')){
        throw 'P8 kernel-level two-layer CPU_OFF safety latch missing'
    }
    Write-Output 'P8 SAFE UNIFIED PASS: 512-row ABI, PSCI/PMU provenance, thermal cap, negative CPU_ON, interleaved USB readback and fail-closed checks.'
}finally{
    Remove-Item $temp -ErrorAction SilentlyContinue
}
