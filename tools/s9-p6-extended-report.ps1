#requires -Version 5.1
# P6.6-9 read-only, phase-tagged offline telemetry; never modifies raw log.
param([Parameter(Mandatory=$true)][string]$Log)
Set-StrictMode -Version 2
$ErrorActionPreference='Stop'
$lines=[IO.File]::ReadAllLines((Resolve-Path -LiteralPath $Log).Path)
$ph=@();$p66=@();$p67=@();$p67path=@();$p68=@();$p69=@();$p6src=@()
$temp=@();$events=@();$bad=0;$active=-1
function Tok([string]$line,[string]$key,[string]$fallback='0'){
    $m=[regex]::Match($line,'(?<![A-Za-z0-9_])'+[regex]::Escape($key)+'=([^\s]+)')
    if($m.Success){return $m.Groups[1].Value}
    return $fallback
}
function Num([string]$line,[string]$key,[string]$fallback='0'){
    $v=Tok $line $key $fallback
    if($v -notmatch '^\d+$'){throw "interleaved/non-numeric $key"}
    return [long]::Parse($v,[Globalization.CultureInfo]::InvariantCulture)
}
function Sum($arr,[string]$key){$a=@($arr);if(!$a.Count){return [double]0};return [double](($a|Measure-Object -Property $key -Sum).Sum)}
function Avg($arr,[string]$key){$a=@($arr);if(!$a.Count){return 'n/a'};return [math]::Round([double](($a|Measure-Object -Property $key -Average).Average),2)}
function Rate([double]$numer,[double]$denom){if($denom -le 0){return 'n/a'};return [math]::Round($numer/$denom,2)}
foreach($line in $lines){
    try {
        if($line -match '^p6: PHASE=(\d+) up=(\d+)ms'){
            $active=[int]$Matches[1];$ph += [pscustomobject]@{p=$active;up=[long]$Matches[2]}
        }elseif($line -match '^p66-ca: up=[0-9.]+s phase=(\d+)'){
            $p66 += [pscustomobject]@{p=[int]$Matches[1];n=(Num $line 'n')
                draws=(Num $line 'draws');clears=(Num $line 'clears');uploads=(Num $line 'uploads')
                checks=(Num $line 'checks');same=(Num $line 'same');changed=(Num $line 'changed')
                skipped=(Num $line 'skipped');single=(Num $line 'single_target');multi=(Num $line 'multi_target')
                unknown=(Num $line 'target_unknown');stable=(Num $line 'stable_target')
                switched=(Num $line 'switched_target');candidate=(Num $line 'input_candidates')
                unchecked=(Num $line 'unchecked_frames');longgap=(Num $line 'long_gap')
                notshown=(Num $line 'not_presented');streak=(Num $line 'max_streak')}
        }elseif($line -match '^p67-cost: up=[0-9.]+s phase=(\d+)'){
            $p67 += [pscustomobject]@{p=[int]$Matches[1];n=(Num $line 'n')
                calls=(Num $line 'calls');wall=(Num $line 'total_wall_us')
                draw=(Num $line 'draw_us');texture=(Num $line 'texture_us')
                target=(Num $line 'target_us');shader=(Num $line 'shader_us')
                state=(Num $line 'state_us');other=(Num $line 'other_us')
                topSlot=(Tok $line 'top_slot');topUs=(Num $line 'top_us')}
        }elseif($line -match '^p67-path: up=[0-9.]+s phase=(\d+)'){
            $p67path += [pscustomobject]@{p=[int]$Matches[1]
                bind=(Num $line 'bind_samples');images=(Num $line 'bind_images')
                checks=(Num $line 'bind_checks');dirty=(Num $line 'bind_dirty')
                native=(Num $line 'bind_native_us');lookup=(Num $line 'bind_lookup_us')
                checksum=(Num $line 'bind_check_us');upload=(Num $line 'bind_upload_us')
                draw=(Num $line 'draw_samples');drawNative=(Num $line 'draw_native_us')
                drawOther=(Num $line 'draw_other_us');flush=(Num $line 'flush_raw_us')}
        }elseif($line -match '^p68-g3d: up=(\d+)ms phase=(\d+)'){
            $p68 += [pscustomobject]@{p=[int]$Matches[2];up=[long]$Matches[1]
                domain=(Num $line 'domain');pll=(Num $line 'pll')
                mhz=(Num $line 'clock_hint_mhz')
                validJob=(Num $line 'job_valid');validFrame=(Num $line 'frame_valid')
                jobAge=(Num $line 'job_age_ms');frameAge=(Num $line 'frame_age_ms')
                quiet=(Num $line 'quiet10s');shadow=(Num $line 'could_shadow')
                off=(Num $line 'actual_off');fault=(Num $line 'fault')}
        }elseif($line -match '^p69-source: up=(\d+)s phase=(\d+)'){
            $h=Tok $line 'delta_valid' '0x0'
            $present=Tok $line 'present_mask' '0x0'
            $p69 += [pscustomobject]@{p=[int]$Matches[2];up=[long]$Matches[1]
                span=(Num $line 'span_s')
                usbDisc=(Num $line 'usb_disconnected');usbStopped=(Num $line 'usb_stopped')
                touchLow=(Num $line 'touch_low');touchReads=(Num $line 'touch_reads')
                keyPolls=(Num $line 'key_polls');keyDown=(Num $line 'key_down')
                keyUp=(Num $line 'key_up');keyHeld=(Num $line 'key_held')
                valid=[Convert]::ToUInt32($h.Replace('0x',''),16)
                present=[Convert]::ToUInt32($present.Replace('0x',''),16)}
        }elseif($line -match '^p6-source: up=(\d+)s phase=(\d+)'){
            $h=Tok $line 'delta_valid' '0x0'
            $p6src += [pscustomobject]@{p=[int]$Matches[2]
                usb=(Num $line 'usb_ticks');conn=(Num $line 'usb_connected')
                poll=(Num $line 'touch_poll');irq=(Num $line 'touch_irq')
                wifi=(Num $line 'wifi_irq')
                valid=[Convert]::ToUInt32($h.Replace('0x',''),16)}
        }elseif($line -match '^cpufreq_gov: \d+ s:.*?temp big ([0-9.]+) lit ([0-9.]+) C'){
            $temp += [pscustomobject]@{p=$active;big=[double]$Matches[1];lit=[double]$Matches[2]}
        }elseif($line -match '^exynos_keys: power (down|up)'){
            $events += [pscustomobject]@{p=$active;type=('power_'+$Matches[1])}
        }elseif($line -match 'GenericARMPE: the display (off|on)'){
            $events += [pscustomobject]@{p=$active;type=('display_'+$Matches[1])}
        }
    }catch{$bad++}
}
Write-Output ('P6.6-9 EXTENDED REPORT: '+$Log)
Write-Output ("raw_lines=$($lines.Count) phase_markers=$($ph.Count) skipped_interleaved=$bad p66=$($p66.Count) p67=$($p67.Count) p67path=$($p67path.Count) p68=$($p68.Count) p69=$($p69.Count)")
if(!$ph.Count){Write-Warning 'No P6 phase markers. Verify matching image and COM sysctl ACK.'}
foreach($phase in @($ph|Select-Object -ExpandProperty p -Unique)){
    $a=@($p66|Where-Object p -eq $phase)
    $b=@($p67|Where-Object p -eq $phase)
    $c=@($p67path|Where-Object p -eq $phase)
    $d=@($p68|Where-Object p -eq $phase)
    $e=@($p69|Where-Object p -eq $phase)
    $f=@($p6src|Where-Object p -eq $phase)
    $t=@($temp|Where-Object p -eq $phase)
    $k=@($events|Where-Object p -eq $phase)
    Write-Output ''
    Write-Output ("PHASE=$phase P66groups=$($a.Count) P67groups=$($b.Count) P67paths=$($c.Count) P68samples=$($d.Count) P69samples=$($e.Count)")
    if($t.Count){Write-Output ("  THERMAL: bigAvgC=$(Avg $t 'big') litAvgC=$(Avg $t 'lit') samples=$($t.Count)")}
    Write-Output ("  SCREEN/KEY: key_down=$(@($k|Where-Object type -eq 'power_down').Count) key_up=$(@($k|Where-Object type -eq 'power_up').Count) display_off=$(@($k|Where-Object type -eq 'display_off').Count) display_on=$(@($k|Where-Object type -eq 'display_on').Count)")
    if($a.Count){
        Write-Output ("  P6.6 TARGETS: frames=$(Sum $a 'n') draws=$(Sum $a 'draws') single=$(Sum $a 'single') multiple=$(Sum $a 'multi') unknown=$(Sum $a 'unknown') stable=$(Sum $a 'stable') switched=$(Sum $a 'switched')")
        Write-Output ("  P6.6 INPUTS: checks=$(Sum $a 'checks') same=$(Sum $a 'same') changed=$(Sum $a 'changed') skipped=$(Sum $a 'skipped') unchecked_frames=$(Sum $a 'unchecked') candidates=$(Sum $a 'candidate') longest_candidate_streak=$(($a|Measure-Object streak -Maximum).Maximum) not_shown=$(Sum $a 'notshown') long_gaps=$(Sum $a 'longgap') OUTPUT_PIXELS_UNVERIFIED skip_applied=0")
    }
    if($b.Count){
        $n=Sum $b 'n'
        Write-Output ("  P6.7 CPU-SLOTS: frames=$n calls=$(Sum $b 'calls') total_wall_us/frame=$(Rate (Sum $b 'wall') $n) draw_us/frame=$(Rate (Sum $b 'draw') $n) texture_us/frame=$(Rate (Sum $b 'texture') $n) target_us/frame=$(Rate (Sum $b 'target') $n) shader_us/frame=$(Rate (Sum $b 'shader') $n) state_us/frame=$(Rate (Sum $b 'state') $n)")
        foreach($top in @($b|Group-Object topSlot|Sort-Object {($_.Group|Measure-Object topUs -Sum).Sum} -Descending|Select-Object -First 3)){
            Write-Output ("  P6.7 TOP-SLOT: slot=$($top.Name) total_top_us=$(Sum $top.Group 'topUs') groups=$($top.Count)")
        }
    }
    if($c.Count){
        Write-Output ("  P6.7 PATH: bind_samples=$(Sum $c 'bind') bind_images=$(Sum $c 'images') checks=$(Sum $c 'checks') dirty=$(Sum $c 'dirty') native_us=$(Sum $c 'native') lookup_us=$(Sum $c 'lookup') check_us=$(Sum $c 'checksum') upload_us=$(Sum $c 'upload') draw_samples=$(Sum $c 'draw') draw_native_us=$(Sum $c 'drawNative') draw_other_us=$(Sum $c 'drawOther') flush_raw_us=$(Sum $c 'flush') samples_1in16=1")
    }
    if($d.Count){
        Write-Output ("  P6.8 PMU: on=$(@($d|Where-Object domain -eq 1).Count)/$($d.Count) off=$(@($d|Where-Object domain -eq 0).Count)/$($d.Count) quiet10s=$(Sum $d 'quiet') quiet_but_powered=$(Sum $d 'shadow') pll_on=$(@($d|Where-Object pll -eq 1).Count) clock_hint_avg_mhz=$(Avg $d 'mhz') fault_samples=$(@($d|Where-Object fault -gt 0).Count)")
        Write-Output ("  P6.8 PROOF: valid_ages=$(@($d|Where-Object {$_.validJob -eq 1 -and $_.validFrame -eq 1}).Count)/$($d.Count) PMU_5s_snapshots_only not_watts")
    }
    if($e.Count){
        $names=@('usbDisc','usbStopped','touchLow','touchReads','keyPolls','keyDown','keyUp','keyHeld')
        $pieces=@()
        for($i=0;$i -lt 8;$i++){
            $subset=@($e|Where-Object {($_.valid -band (1 -shl $i)) -ne 0 -and $_.span -gt 0})
            $span=Sum $subset 'span'
            $val=Sum $subset $names[$i]
            $hz=Rate $val $span
            $pieces+=($names[$i]+'='+$val+'/'+$subset.Count+'@'+$hz+'Hz')
        }
        Write-Output ('  P6.9 SOURCE: '+($pieces -join ' '))
        Write-Output '  P6.9 PROOF: counters are actual callback/GPIO polls and debounced key transitions, not CPU WFI exits.'
    }
    if($f.Count -and $phase -eq 15){
        $valid=@($f|Where-Object {($_.valid -band 0x3f) -eq 0x3f})
        Write-Output ("  P6.9 LEGACY: usb_ticks=$(Sum $valid 'usb') usb_connected=$(Sum $valid 'conn') touch_pin_polls=$(Sum $valid 'poll') touch_irq=$(Sum $valid 'irq') wifi_irq=$(Sum $valid 'wifi') windows=$($valid.Count)")
    }
}
Write-Output 'P6.6 candidate != same pixels; P6.7 CPU wall != GPU execution; P6.8 PMU sample != watts; P6.9 poll != core wakeup. All read-only.'
