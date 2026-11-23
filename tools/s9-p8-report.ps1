[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Log,
    [string]$CsvPath=''
)
Set-StrictMode -Version 2
$ErrorActionPreference='Stop'
if(-not(Test-Path -LiteralPath $Log)){throw "P8 report: file not found: $Log"}
$culture=[Globalization.CultureInfo]::InvariantCulture
$rows=New-Object 'System.Collections.Generic.List[object]'
$gpuRows=New-Object 'System.Collections.Generic.List[object]'
$psci=New-Object 'System.Collections.Generic.List[string]'
$pmu=New-Object 'System.Collections.Generic.List[string]'
$rails=New-Object 'System.Collections.Generic.List[string]'
$oldCpuOff=New-Object 'System.Collections.Generic.List[string]'
$oldCpuOn=New-Object 'System.Collections.Generic.List[string]'
$faults=New-Object 'System.Collections.Generic.List[string]'
$passiveCount=@{p6=0; p4=0; p7_gpu=0; fps=0; fuel=0; clock=0}
$guard=0;$capLower=0;$capReset=0
function Pair-Map([string]$line){
    $m=@{}
    foreach($match in [regex]::Matches($line,'(?<key>[a-zA-Z][a-zA-Z_0-9]*)=(?<val>[^\s]+)')){
        $m[$match.Groups['key'].Value]=$match.Groups['val'].Value
    }
    return $m
}
function Num($m,[string]$k,[long]$default=-1){
    if(-not $m.ContainsKey($k)){return $default}
    $out=0L
    $v=$m[$k]
    if($v -match '^0x[0-9a-fA-F]+$'){
        try{return [Convert]::ToInt64($v.Substring(2),16)}catch{return $default}
    }
    if([long]::TryParse($v,[Globalization.NumberStyles]::Integer,$culture,[ref]$out)){return $out}
    return $default
}
function Parse-P8([string]$text,[bool]$fromRing){
    $m=Pair-Map $text
    $up=Num $m $(if($fromRing){'up_ms'}else{'up'})
    # Live p8: up=123ms; retained p8_record: up_ms=123.
    if(-not $fromRing -and $m.ContainsKey('up') -and $m['up'] -match '^(\d+)ms$'){
        $up=[long]$Matches[1]
    }
    if($up -lt 0){return $null}
    $ph=Num $m $(if($fromRing){'ph'}else{'ph'})
    $idleLit=-1L;$idleBig=-1L;$capLit=-1L;$capBig=-1L
    if($m.ContainsKey('idle') -and $m['idle'] -match '^(\d+)/(\d+)$'){
        $idleLit=[long]$Matches[1];$idleBig=[long]$Matches[2]
    }
    if($m.ContainsKey('cap') -and $m['cap'] -match '^(\d+)/(\d+)$'){
        $capLit=[long]$Matches[1];$capBig=[long]$Matches[2]
    }
    $temps=@(-273000L,-273000L,-273000L)
    if($m.ContainsKey('temp_mc') -and $m['temp_mc'] -match '^(-?\d+)/(-?\d+)/(-?\d+)$'){
        $temps=@([long]$Matches[1],[long]$Matches[2],[long]$Matches[3])
    }
    $pmuMask=Num $m 'pmu_on'
    $pmuCfg=Num $m 'pmu_cfg'
    return [pscustomobject]@{
        up_ms=$up;phase=$ph;disp=(Num $m 'disp');
        lit_on=(Num $m 'lit_on');big_on=(Num $m 'big_on');
        idle_lit=$(if($idleLit -eq 255){'unavailable'}else{$idleLit});
        idle_big=$(if($idleBig -eq 255){'unavailable'}else{$idleBig});
        big_mc=$temps[0];lit_mc=$temps[1];g3d_mc=$temps[2];
        cap_lit_mhz=$capLit;cap_big_mhz=$capBig;
        pmu_valid=(Num $m 'pmu_valid');pmu_on_mask=$pmuMask;
        pmu_cfg_mask=$pmuCfg;usb_valid=(Num $m 'usb_valid');
        usb_delta=(Num $m 'usb');touch_valid=(Num $m 'touch_valid');
        touch_delta=(Num $m 'touch');source=$(if($fromRing){'ring'}else{'live'});
        physical_cpu_off='UNPROVEN'
    }
}
function Parse-GPU([string]$text,[bool]$fromRing){
    $m=Pair-Map $text
    $up=Num $m $(if($fromRing){'up_ms'}else{'up'})
    if(-not $fromRing -and $m.ContainsKey('up') -and $m['up'] -match '^(\d+)ms$'){
        $up=[long]$Matches[1]
    }
    if($up -lt 0){return $null}
    return [pscustomobject]@{
        up_ms=$up;phase=(Num $m 'phase');domain=(Num $m 'domain');
        pll=(Num $m 'pll');awake=(Num $m 'awake');
        off_ms=(Num $m $(if($fromRing){'off_ms'}else{'off_total_ms'}));
        off_count=(Num $m $(if($fromRing){'off'}else{'off'}));
        wake_count=(Num $m 'wake');wake_fail=(Num $m 'wake_fail');
        fault=(Num $m 'fault');gpu_safe=(Num $m 'gpu_safe');
        tz_ok=(Num $m 'tz_ok');
        PMU_physical_off=$(if((Num $m 'domain') -eq 0 -and (Num $m 'pll') -eq 0){'PMU_OFF_PLL_OFF'}else{'NOT_PROVEN_OFF'});
        source=$(if($fromRing){'ring'}else{'live'})
    }
}
foreach($line in [IO.File]::ReadLines([IO.Path]::GetFullPath($Log))){
    if($line -match '^p8: up=\d+ms '){
        $r=Parse-P8 $line $false
        if($null -ne $r){$rows.Add($r)}
    }elseif($line -match '^usb: kern\.hakios\.p8_record = .*?up_ms=\d+ '){
        $r=Parse-P8 $line $true
        if($null -ne $r){$rows.Add($r)}
    }
    if($line -match '^p7-gpu: up=\d+ms '){
        $gr=Parse-GPU $line $false
        if($null -ne $gr){$gpuRows.Add($gr)}
    }elseif($line -match '^usb: kern\.hakios\.p7_gpu_record = .*?up_ms=\d+ '){
        $gr=Parse-GPU $line $true
        if($null -ne $gr){$gpuRows.Add($gr)}
    }
    if($line -match '^usb: kern\.hakios\.p8_psci_stats = (.+)'){
        $psci.Add($Matches[1])
    }
    if($line -match '^usb: kern\.hakios\.p8_pmu_stats = (.+)'){$pmu.Add($Matches[1])}
    if($line -match '^usb: kern\.hakios\.p8_rail_stats = (.+)'){$rails.Add($Matches[1])}
    if($line -match '^GenericARMPE: sleep_cores: cpu \d+ taken off \(0\)'){$oldCpuOff.Add($line)}
    if($line -match '^GenericARMPE: sleep_cores: cpu \d+ started again \(0\)'){$oldCpuOn.Add($line)}
    if($line -match '^(?:GenericARMPE: sleep_cores: cpu \d+ NOT started again|psci: cpu \d+ .*no stage 3 in 5 s|p7-gpu: gate_watchdog=RESTORE_FAILED)'){
        $faults.Add($line)
    }
    if($line -match '^p8-safety: BIG CPU_OFF REFUSED'){$guard++}
    if($line -match '^p8: thermal_cap_big_khz=1469000'){$capLower++}
    if($line -match '^p8: thermal_cap_big_khz=0'){$capReset++}
    if($line -match '^p6-(?:wake|source|trans):'){$passiveCount.p6++}
    if($line -match '^p4-(?:cpu|core|thread|gl):'){$passiveCount.p4++}
    if($line -match '^p7-gpu: up='){$passiveCount.p7_gpu++}
    if($line -match '^fps: [0-9]'){$passiveCount.fps++}
    if($line -match '^max77705: fg:'){$passiveCount.fuel++}
    if($line -match '^hi: (?:phase|clock_proof|verified)'){$passiveCount.clock++}
}
Write-Output ('P8 unified report | '+(Get-Item -LiteralPath $Log).Name)
Write-Output ('P8 rows='+$rows.Count+' GPU PMU rows='+$gpuRows.Count+' PSCI_direct='+$psci.Count+
    ' PMU_direct='+$pmu.Count+' PMIC_ACPM_direct='+$rails.Count+
    ' independent_GPU_rows='+$passiveCount.p7_gpu)
Write-Output ('legacy source windows: p4='+$passiveCount.p4+
    ' p6='+$passiveCount.p6+' fps='+$passiveCount.fps+
    ' fuel_gauge='+$passiveCount.fuel+' clock='+$passiveCount.clock)
Write-Output 'EVIDENCE RULE: XNU taken_off != PSCI AFFINITY_OFF != PMU power_OFF; stage1 is caller-written and NOT proof of target re-entry.'
Write-Output ('P7 CPU scheduler OFF events='+$oldCpuOff.Count+
    ' CPU_ON success events='+$oldCpuOn.Count+
    ' P8 kernel safety refusals='+$guard)
Write-Output ('Thermal cap A/B observed: 1469MHz_set='+$capLower+
    ' stock_restored='+$capReset)
if($faults.Count -gt 0){
    Write-Output ('P8 CPU RE-ENTRY / POWER FAULT: '+$faults.Count+' markers: FAIL, NO CPU HARDWARE POWER SUCCESS CLAIM')
    foreach($f in ($faults | Select-Object -First 6)){Write-Output ('  FAULT '+$f)}
}elseif($oldCpuOff.Count -gt 0){
    Write-Output 'P8 LEGACY PARK: SCHEDULER_OFF_ONLY. Physical CPU power state UNPROVEN without PSCI+PMU OFF and a successful same-core stage3.'
}else{
    Write-Output 'P8 CPU hardware: no active CPU_OFF attempted; re-entry safety preserved.'
}
if($psci.Count){
    foreach($line in $psci){
        $m=Pair-Map $line
        $cpu=Num $m 'cpu';$st=Num $m 'stage';$aff=Num $m 'affinity'
        if($cpu -ge 3 -and $cpu -le 6){
            Write-Output ("PSCI cpu=$cpu stage=$st affinity=$aff "+$(if($aff -eq 0){'(firmware ON)'}elseif($aff -eq 1){'(firmware OFF)'}else{'(UNKNOWN)'}))
        }
    }
}else{Write-Output 'PSCI direct AFFINITY_INFO: NOT MEASURED in this log'}
if($rows.Count){
    $unique=@($rows | Sort-Object up_ms,source -Unique)
    foreach($group in ($unique | Group-Object phase |Sort-Object { [int]$_.Name })){
        $q=@($group.Group | Sort-Object up_ms)
        $b=@($q|Where-Object{$_.big_mc -gt -273000})
        $l=@($q|Where-Object{$_.lit_mc -gt -273000})
        $g=@($q|Where-Object{$_.g3d_mc -gt -273000})
        $maxBig=$(if($b.Count){[math]::Round((($b|Measure-Object big_mc -Maximum).Maximum)/1000,1)}else{'unmeasured'})
        $maxLit=$(if($l.Count){[math]::Round((($l|Measure-Object lit_mc -Maximum).Maximum)/1000,1)}else{'unmeasured'})
        $maxG=$(if($g.Count){[math]::Round((($g|Measure-Object g3d_mc -Maximum).Maximum)/1000,1)}else{'unmeasured'})
        $onlineBad=@($q|Where-Object{$_.big_on -ne 4 -or $_.lit_on -ne 4})
        $pmuValid=@($q|Where-Object{$_.pmu_valid -eq 1})
        $usbValid=@($q|Where-Object{$_.usb_valid -eq 1})
        $bUsb=0L;$elapsedMs=0L
        for($i=1;$i -lt $q.Count;$i++){
            if($q[$i].usb_valid -eq 1 -and $q[$i-1].phase -eq $q[$i].phase){
                $dt=$q[$i].up_ms-$q[$i-1].up_ms
                if($dt -gt 0 -and $dt -le 40000){
                    $bUsb+=$q[$i].usb_delta
                    $elapsedMs+=$dt
                }
            }
        }
        $usbrate=if($elapsedMs -gt 0){
            [math]::Round($bUsb/($elapsedMs/1000),1)
        }else{'unmeasured'}
        Write-Output ('PHASE '+$group.Name+' n='+$q.Count+
            ' up_ms='+$q[0].up_ms+'..'+$q[-1].up_ms+
            ' big_lit_missing='+$onlineBad.Count+
            ' BIGmaxC='+$maxBig+' LITTLEmaxC='+$maxLit+' G3DmaxC='+$maxG+
            ' PMU_n='+$pmuValid.Count+' usb_calls_per_s~='+$usbrate)
    }
}else{Write-Output 'P8 unified rows: none; if this is a pre-P8 image, only historical fault-marker analysis is possible.'}
if($gpuRows.Count){
    foreach($grp in ($gpuRows|Group-Object phase|Sort-Object { [int]$_.Name })){
        $q=@($grp.Group|Sort-Object up_ms)
        $offs=@($q|Where-Object{$_.PMU_physical_off -eq 'PMU_OFF_PLL_OFF'})
        $faulty=@($q|Where-Object{$_.fault -gt 0 -or $_.wake_fail -gt 0 -or $_.tz_ok -eq 0})
        Write-Output ('GPU PHASE '+$grp.Name+' n='+$q.Count+' PMU_OFF_PLL_OFF='+$offs.Count+
          ' faults='+$faulty.Count+' off_ms_last='+$q[-1].off_ms+
          ' source=PMU_PLL_not_electrical_watts')
    }
}else{Write-Output 'GPU PMU/PLL history: NOT MEASURED in this log'}
if(-not[string]::IsNullOrWhiteSpace($CsvPath)){
    $dest=[IO.Path]::GetFullPath($CsvPath)
    if($rows.Count){
        $rows|Sort-Object up_ms,source -Unique|Export-Csv -LiteralPath $dest -NoTypeInformation -Encoding UTF8
    }else{
        'up_ms,phase,disp,lit_on,big_on,idle_lit,idle_big,physical_cpu_off'|Set-Content -LiteralPath $dest -Encoding UTF8
    }
    Write-Output ('CSV '+$dest)
    $gpuDest=if($dest -match '\.p8-data\.csv$'){
        $dest -replace '\.p8-data\.csv$','.p8-gpu.csv'
    }else{$dest -replace '\.csv$','-gpu.csv'}
    if($gpuRows.Count){
        $gpuRows|Sort-Object up_ms,source -Unique|Export-Csv -LiteralPath $gpuDest -NoTypeInformation -Encoding UTF8
    }else{
        'up_ms,phase,domain,pll,PMU_physical_off'|Set-Content -LiteralPath $gpuDest -Encoding UTF8
    }
    Write-Output ('GPU CSV '+$gpuDest)
}
