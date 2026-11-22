#requires -Version 5.1
# S9 P6 read-only parallel diagnostic report: MAX77705 + Scion + EAGL + thermal.
# This only reads a captured serial log; never connects to USB or writes a charger register.
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$Log)
Set-StrictMode -Version 2
$ErrorActionPreference='Stop'
$path=(Resolve-Path -LiteralPath $Log).Path
$phase=-1;$lineNo=0;$rejected=0
$ph=@();$gauge=@();$battery=@();$charger=@();$trans=@();$fps=@();$hash=@();$eagl=@();$bb=@();$temp=@()
$dtls=@{}
function Metric([string]$s,[string]$key) {
    $m=[regex]::Match($s,'(?<![\w])'+[regex]::Escape($key)+'=(-?\d+)')
    if(!$m.Success){throw "missing $key"}
    return [long]::Parse($m.Groups[1].Value,[Globalization.CultureInfo]::InvariantCulture)
}
function HexNum([string]$v) {
    if($v -match '^0x[0-9a-fA-F]+$'){return [Convert]::ToInt32($v.Substring(2),16)}
    if($v -match '^-?\d+$'){return [int]$v}
    throw "unsupported hex $v"
}
function Total($a,[string]$name) {
    $arr=@($a);if(!$arr.Count){return [double]0}
    return [double](($arr|Measure-Object -Property $name -Sum).Sum)
}
function Average($a,[string]$name) {
    $arr=@($a);if(!$arr.Count){return 'n/a'}
    return [Math]::Round([double](($arr|Measure-Object -Property $name -Average).Average),2)
}
foreach($s in [IO.File]::ReadLines($path)){
    $lineNo++
    try{
        if($s -match '^p6: PHASE=(\d+) up=(\d+)ms'){
            $phase=[int]$Matches[1]
            $ph += [pscustomobject]@{p=$phase;up=[long]$Matches[2];line=$lineNo}
        }elseif($s -match '^max77705: fg: t (\d+) s, (\d+)\.(\d+)%[,] (\d+) mAh, current (-?\d+) mA, avg (-?\d+) mA, QH (\d+)'){
            $gauge += [pscustomobject]@{
                p=$phase;up=[long]$Matches[1]
                soc=([double]$Matches[2]+([double]$Matches[3]/100))
                mah=[int]$Matches[4];current=[int]$Matches[5]
                avg=[int]$Matches[6];qh=[int]$Matches[7]}
        }elseif($s -match '^max77705: (now|every 5 minutes): (\d+)\.(\d+)%, (\d+) mV, (-?\d+) mA, (\d+)\.(\d+) C'){
            $battery += [pscustomobject]@{p=$phase;kind=$Matches[1]
                soc=[double]$Matches[2]+[double]$Matches[3]/100
                mv=[int]$Matches[4];current=[int]$Matches[5]
                temp=[double]$Matches[6]+[double]$Matches[7]/10
                line=$lineNo}
        }elseif($s -match '^max77705: charger (now|periodic|every 5 minutes): CNFG_00 mode (0x[0-9a-fA-F]+|-?\d+) WDTEN (-?\d+); CHGIN_DTLS (-?\d+); CHG_DTLS (0x[0-9a-fA-F]+|-?\d+) BAT_DTLS (-?\d+); BYP_DTLS (0x[0-9a-fA-F]+|-?\d+); fast charge (-?\d+) mA; input limit code (0x[0-9a-fA-F]+|-?\d+) \(CNFG_09\); up=(\d+)s'){
            $charger += [pscustomobject]@{p=$phase;kind=$Matches[1]
                mode=(HexNum $Matches[2]);wdt=[int]$Matches[3]
                chgin=[int]$Matches[4];chg=(HexNum $Matches[5])
                bat=[int]$Matches[6];byp=(HexNum $Matches[7])
                fast=[int]$Matches[8];limit=(HexNum $Matches[9]);up=[long]$Matches[10]}
        }elseif($s -match '^p610-trans: up=(\d+)ms phase=(\d+)'){
            $trans += [pscustomobject]@{p=[int]$Matches[2];up=[long]$Matches[1]
                from=(Metric $s 'from');to=(Metric $s 'to')
                gap=(Metric $s 'gap_ms');age=(Metric $s 'frame_age_ms')
                quiet=(Metric $s 'quiet_ms');touchAge=(Metric $s 'touch_age_ms')
                moving=(Metric $s 'moving');fingers=(Metric $s 'fingers')}
        }elseif($s -match '^fps: (\d+) presented, (\d+) drawn, (\d+) flips, (\d+) lost, (\d+) held;'){
            $fps += [pscustomobject]@{p=$phase;present=[int]$Matches[1];flip=[int]$Matches[3];lost=[int]$Matches[4]}
        }elseif($s -match '^p611-hash: .* phase=(\d+)'){
            $hash += [pscustomobject]@{p=[int]$Matches[1]
                checks=(Metric $s 'checked');skip=(Metric $s 'skip_seldom')
                hashSamples=(Metric $s 'samples');hashBytes=(Metric $s 'sample_bytes')
                hashUs=(Metric $s 'sample_us')}
        }elseif($s -match '^p612-eagl: .* phase=(\d+)'){
            $eagl += [pscustomobject]@{p=[int]$Matches[1]
                frames=(Metric $s 'frames');draws=(Metric $s 'draws')
                alternating=(Metric $s 'alternating');zero=(Metric $s 'api_zero')}
        }elseif($s -match '^poster_cost: \d+ s: backboardd\[\d+\] cpu=([\d.]+)%'){
            $bb += [pscustomobject]@{p=$phase;cpu=[double]::Parse($Matches[1],[Globalization.CultureInfo]::InvariantCulture)}
        }elseif($s -match '^cpufreq_gov: \d+ s:.*temp big ([\d.]+) lit ([\d.]+) C'){
            $temp += [pscustomobject]@{p=$phase;big=[double]::Parse($Matches[1],[Globalization.CultureInfo]::InvariantCulture)}
        }
    }catch{$rejected++}
}
Write-Output ('P6 PARALLEL REPORT: '+$Log)
Write-Output ("lines=$lineNo phases=$($ph.Count) fg=$($gauge.Count) battery=$($battery.Count) charger=$($charger.Count) gov_transition=$($trans.Count) p611=$($hash.Count) p612=$($eagl.Count) rejected=$rejected")
if(!$charger.Count){Write-Warning 'No decoded charger registers. BOOT needs max77705_diag=1 max77705_diag_s=30; WDT/CHGIN/input limit remain UNMEASURED.'}
if(!$gauge.Count){Write-Warning 'No frequent fuel gauge. BOOT needs max77705_fg_s=10; only 5-minute SOC/current evidence may exist.'}
if(!$ph.Count){Write-Warning 'No P6 phase markers; do not claim an on-device parallel test ran.'}
Write-Output ''
Write-Output 'CHARGER TIMELINE: up_seconds is true kernel uptime, not wall clock. -1 means no valid register read.'
foreach($x in $charger){
    $mode=if($x.chg -ge 0){if($x.chg -le 2){'CHARGING_STATE'}elseif($x.chg -le 4){'FULL_STATE'}else{'NOT_CHARGING_STATE'}}else{'UNKNOWN'}
    Write-Output ("charger up=$($x.up)s phase=$($x.p) kind=$($x.kind) WDTEN=$($x.wdt) CHGIN_DTLS=$($x.chgin) CHG_DTLS=$($x.chg) ($mode) BAT_DTLS=$($x.bat) CNFG_00_mode=$($x.mode) CNFG_09_limit_code=$($x.limit) fast_charge_setting_mA=$($x.fast)")
}
Write-Output ''
Write-Output 'GAUGE PHASES: gauge current positive charges battery, negative discharges. QH is RAW unsigned register, scale/sign not inferred.'
foreach($p in @(@($gauge|Select-Object -ExpandProperty p -Unique) + @($ph|Select-Object -ExpandProperty p -Unique)|Select-Object -Unique)){
    $a=@($gauge|Where-Object p -eq $p)
    $f=@($fps|Where-Object p -eq $p)
    $b=@($bb|Where-Object p -eq $p)
    $t=@($temp|Where-Object p -eq $p)
    $h=@($hash|Where-Object p -eq $p)
    $e=@($eagl|Where-Object p -eq $p)
    $tr=@($trans|Where-Object p -eq $p)
    $c=@($charger|Where-Object p -eq $p)
    $line="phase=$p fg_n=$($a.Count) charger_n=$($c.Count) p610_transitions=$($tr.Count) p611_groups=$($h.Count) p612_groups=$($e.Count)"
    if($a.Count){
        $first=$a[0];$last=$a[-1]
        $line+=" fg_span_s=$($last.up-$first.up) soc_first=$($first.soc) soc_last=$($last.soc) capacity_delta_mAh=$($last.mah-$first.mah) current_avg_mA=$(Average $a 'current') avgCurrent_avg_mA=$(Average $a 'avg') QH_first_raw=$($first.qh) QH_last_raw=$($last.qh)"
    }
    if($f.Count){$line+=" fps_mean=$(Average $f 'present') fb_flips=$(Total $f 'flip') fb_lost=$(Total $f 'lost')"}
    if($b.Count){$line+=" backboardd_avg_one_core_pct=$(Average $b 'cpu')"}
    if($t.Count){$line+=" big_temp_C=$(Average $t 'big')"}
    if($h.Count){$line+=" checksum_samples=$(Total $h 'hashSamples') hash_sample_total_us=$(Total $h 'hashUs') skipped_seldom=$(Total $h 'skip')"}
    if($e.Count){$line+=" eagl_frames=$(Total $e 'frames') eagl_alternating=$(Total $e 'alternating')"}
    Write-Output $line
    foreach($v in $tr){
        Write-Output ("  scion_transition up=$($v.up)ms from=$($v.from) to=$($v.to) gap=$($v.gap)ms frame_age=$($v.age)ms quiet=$($v.quiet)ms touch_age=$($v.touchAge)ms moving=$($v.moving) fingers=$($v.fingers)")
    }
}
Write-Output ''
Write-Output 'GUARDRAILS: external USB ammeter readings are NOT stored in serial. 0.7A USB is not 0.7A into the battery. No optical panel scanout, total system watts, CPU WFI or actual GPU gate proved. Thermal 46C guard uses existing serial telemetry and is NOT a hardware over-temperature protection.'
