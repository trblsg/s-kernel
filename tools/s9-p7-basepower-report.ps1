#requires -Version 5.1
<#
 P7 per-phase SoC power audit. CPU scheduler idle != WFI/C2/CPD.
 The live lines from the new kernel ring workers are the primary source.
#>
[CmdletBinding()]
param([Parameter(Mandatory)][string]$Log)
Set-StrictMode -Version 2
$ErrorActionPreference='Stop'
if(-not (Test-Path -LiteralPath $Log)){throw "No P7 log: $Log"}
$states=@{}
$clock=@{unknown=0;visible=0;detached=0;hidden=0;outside=0;verifiedCuts=0}
function Slot([int]$phase){
    if(-not $states.ContainsKey($phase)){
        $states[$phase]=[pscustomobject]@{
          phase=$phase;cpu=0;gpu=0;lit_idle=0;big_idle=0;lit_on=0;big_on=0
          usb=0;usbSamples=0;touch=0;touchSamples=0;off=0;pll=0
          gpuClock=0;jobAge=0;offMax=0;gpuTemp=0
          wakeFails=0;faults=0;first=0;last=0
        }
    }
    return $states[$phase]
}
$replayed=0;$gateArmed=0;$gateRestored=0;$parkOff=0;$parkOn=0;$invalid=0
foreach($line in [System.IO.File]::ReadLines((Resolve-Path -LiteralPath $Log).Path)){
    if($line -match '^hi: clock_proof .+ state=(unknown|visible|detached|hidden|outside) '){$clock[$Matches[1]]++}
    if($line -match '^hi: link=\d+ target=SBHClockApplicationIconImageView .+ verified_cut=(\d+)'){$clock.verifiedCuts += [int]$Matches[1]}
    if($line -match '^p7-gpu: gate_watchdog=ARMED'){$gateArmed++}
    if($line -match '^p7-gpu: gate_watchdog=RESTORED'){$gateRestored++}
    if($line -match '^GenericARMPE: sleep_cores: cpu \d+ taken off'){$parkOff++}
    if($line -match '^GenericARMPE: sleep_cores: cpu \d+ started again'){$parkOn++}
    if($line -match '^usb: kern.hakios.p7_(cpu|gpu)_record = seq='){$replayed++}
    if($line -match '^p7-cpu: up=(\d+)ms phase=(\d+) disp=(\d+) idle_lit=(\d+)% idle_big=(\d+)% lit_online=(\d+) big_online=(\d+) mhz=([\d/]+) exit=\S+ usb_valid=(\d+) usb_delta=(\d+) usb_connected=(\d+) touch_valid=(\d+) touch_delta=(\d+)'){
        $m=$Matches;$p=Slot ([int]$m[2]);$p.cpu++
        $p.lit_idle += [int]$m[4];$p.big_idle +=[int]$m[5]
        $p.lit_on += [int]$m[6];$p.big_on += [int]$m[7]
        if([int]$m[9] -eq 1){$p.usb += [int]$m[10];$p.usbSamples++}
        if([int]$m[12] -eq 1){$p.touch += [int]$m[13];$p.touchSamples++}
        if($p.first -eq 0){$p.first=[int64]$m[1]}
        $p.last=[int64]$m[1]
    }elseif($line -match '^p7-gpu: up=(\d+)ms phase=(\d+) domain=(\d+) pll=(\d+) awake=(\d+) clock=(\d+)MHz frame_age_ms=(\d+) job_age_ms=(\d+) off_total_ms=(\d+) off=(\d+) wake=(\d+) wake_fail=(\d+) gate=(\d+) fault=(\d+) gpu_safe=(\d+) tz_ok=(\d+) rail_valid=(\d+) rail_mv=(\d+) g3d_mc=(-?\d+)'){
        $m=$Matches;$p=Slot ([int]$m[2]);$p.gpu++
        if([int]$m[3] -eq 0){$p.off++}
        if([int]$m[4] -ne 0){$p.pll++}
        $p.gpuClock += [int]$m[6];$p.jobAge +=[int64]$m[8]
        $p.offMax=[Math]::Max($p.offMax,[int64]$m[9])
        $p.gpuTemp += [int]$m[19]
        $p.wakeFails=[Math]::Max($p.wakeFails,[int]$m[12])
        $p.faults=[Math]::Max($p.faults,[int]$m[14])
        if($p.first -eq 0){$p.first=[int64]$m[1]}
        $p.last=[int64]$m[1]
    }elseif($line -match '^usb: kern.hakios.p7_cpu_record = seq=(\d+) up_ms=(\d+) phase=(\d+) disp=(\d+) lit_idle=(\d+) big_idle=(\d+) lit_on=(\d+) big_on=(\d+) mhz=([\d/]+) exit=\S+ usb_valid=(\d+) usb_delta=(\d+) usb_connected=(\d+) touch_valid=(\d+) touch_delta=(\d+)'){
        $m=$Matches;$p=Slot ([int]$m[3]);$p.cpu++
        $p.lit_idle +=[int]$m[5];$p.big_idle +=[int]$m[6]
        $p.lit_on +=[int]$m[7];$p.big_on +=[int]$m[8]
        if([int]$m[10] -eq 1){$p.usb +=[int]$m[11];$p.usbSamples++}
        if([int]$m[13] -eq 1){$p.touch +=[int]$m[14];$p.touchSamples++}
        if($p.first -eq 0){$p.first=[int64]$m[2]}
        $p.last=[int64]$m[2]
    }elseif($line -match '^usb: kern.hakios.p7_gpu_record = seq=(\d+) up_ms=(\d+) phase=(\d+) domain=(\d+) pll=(\d+) awake=(\d+) clock=(\d+) rail_valid=(\d+) rail_mv=(\d+) frame_age=(\d+) job_age=(\d+) off_ms=(\d+) off=(\d+) wake=(\d+) fail=(\d+) gate=(\d+) fault=(\d+) gpu_safe=(\d+) tz_ok=(\d+) g3d_mc=(-?\d+)'){
        $m=$Matches;$p=Slot ([int]$m[3]);$p.gpu++
        if([int]$m[4] -eq 0){$p.off++}
        if([int]$m[5] -ne 0){$p.pll++}
        $p.gpuClock +=[int]$m[7];$p.jobAge +=[int64]$m[11]
        $p.offMax=[Math]::Max($p.offMax,[int64]$m[12])
        $p.gpuTemp +=[int]$m[20]
        $p.wakeFails=[Math]::Max($p.wakeFails,[int]$m[15])
        $p.faults=[Math]::Max($p.faults,[int]$m[17])
        if($p.first -eq 0){$p.first=[int64]$m[2]}
        $p.last=[int64]$m[2]
    }elseif($line -match '^p7-(cpu|gpu): up='){$invalid++}
}
function Mean([double]$s,[int]$n){
    if($n -le 0){return 'n/a'}
    return ('{0:N1}' -f ($s/$n))
}
Write-Output ('S9 P7 base power: {0} labelled phases; ring rows retrieved={1} invalid live lines={2}' -f $states.Count,$replayed,$invalid)
Write-Output ('Clock owner: unknown={0} visible={1} detached={2} hidden={3} outside={4} strictly verified cuts={5}' -f $clock.unknown,$clock.visible,$clock.detached,$clock.hidden,$clock.outside,$clock.verifiedCuts)
Write-Output ('G3D watchdog armed={0} restored={1}; BIG CPU_OFF={2} CPU_ON={3}' -f $gateArmed,$gateRestored,$parkOff,$parkOn)
foreach($p in $states.Values|Sort-Object phase){
    $usbRate=if($p.usbSamples){Mean $p.usb ($p.usbSamples*5)}else{'unmeasured'}
    $touchRate=if($p.touchSamples){Mean $p.touch ($p.touchSamples*5)}else{'unmeasured'}
    Write-Output ('phase={0} up_ms={1}..{2} CPU_n={3} lit_idle={4}% big_idle={5}% online_lit={6} online_big={7} USB_calls/s={8} touch_polls/s={9} GPU_n={10} G3D_off={11}/{10} PLL_on={12}/{10} clock_MHz={13} job_age_ms={14} off_total_max_ms={15} g3d_C={16} wake_fail_max={17} fault_max={18}' -f
        $p.phase,$p.first,$p.last,$p.cpu,(Mean $p.lit_idle $p.cpu),
        (Mean $p.big_idle $p.cpu),(Mean $p.lit_on $p.cpu),
        (Mean $p.big_on $p.cpu),$usbRate,$touchRate,
        $p.gpu,$p.off,$p.pll,(Mean $p.gpuClock $p.gpu),
        (Mean $p.jobAge $p.gpu),$p.offMax,(Mean $p.gpuTemp ($p.gpu*1000)),
        $p.wakeFails,$p.faults)
}
Write-Output 'Note: CPU scheduler-idle is not C2/PSCI_CPU_SUSPEND/CPD/SICD; USB and touch counters are driver callbacks, not actual WFI wakes.'
Write-Output 'PMU/PLL are real register readbacks but NOT electrical watts; compare with external meter and battery-only ring data.'
