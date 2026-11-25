#requires -Version 5.1
# P12.5 OFF timer observation. No changes to hardware or scheduling policy.
[CmdletBinding()]
param(
 [ValidatePattern('^COM[0-9]+$')][string]$Port='COM125',
 [string]$ReconnectPort='',
 [ValidateSet('Trace','UsbCrossover','OfflineOnly','AggregateOnly')][string]$Scenario='Trace',
 [ValidateRange(30,90)][int]$TraceSeconds=60,
 [ValidateRange(12,40)][int]$AggregateSeconds=20,
 [string]$SymbolMap='',
 [switch]$Execute,[switch]$LibraryOnly
)
Set-StrictMode -Version 2
$ErrorActionPreference='Stop'
$script:P125Exec=$Execute;$script:P125Lib=$LibraryOnly
. (Join-Path $PSScriptRoot 's9-p12-dual-serial.ps1') -LibraryOnly -Port $Port
$Execute=$script:P125Exec;$LibraryOnly=$script:P125Lib
$script:P125HostEnabled=$true
$script:P125Dirty=$false
$script:P125Kinds=@('unused','IRQ_CALLBACK','ENTER','WAIT_ARM','WAIT_EXPIRE','DELAYED_READY','THREAD_CALL_INVOKE','MK_ARM','MK_SEND','IOKIT_ACTION','OTHER_QUEUE_CALLBACK','ASSIGN','MK_CANCEL','CANCEL','WAIT_STALE')
$script:P125Counters=@('irq27','queue_services','zero_cb','nonzero_cb','callbacks','other_queue','other_callbacks','scan_pause','scan_postpone','lock_skip_async_dequeue','ratelimited_break','empty','enter','reenter','local','requested_leeway_zero','effective_leeway_zero','requested_lt1ms','wait_armed','wait_stale','delayed_ready','thread_call_invoke','mk_send_attempt','table_drop','setpop','pop_lt1ms','raw_early','cancel')
$script:P125Report=New-Object 'System.Collections.Generic.List[string]'
$script:P125Symbols=@()
function P125-Hex([string]$Value){return [Convert]::ToUInt64($Value,16)}
function P125-Fields([string]$Line){
    $f=@{}
    foreach($m in [regex]::Matches($Line,'([a-zA-Z0-9_]+)=([^\s]+)')){$f[$m.Groups[1].Value]=$m.Groups[2].Value}
    return $f
}
function P125-Status {
    $f=P125-Fields (P12-Query 'p125_status')
    if($f.version -ne '1' -or $f.slots -ne '512' -or $f.counters -ne '28' -or $f.kinds -ne '15' -or $f.rows -ne '2'){throw 'P125 matched kernel ABI missing'}
    foreach($name in @('active','armed','start','end','now','hz','reason','down','up','display_on','anchor')){
        if(-not $f.ContainsKey($name)){throw "P125 status missing $name"}
    }
    return $f
}
function P125-Symbols([string]$Path,[string]$Anchor){
    if(-not $Path){return}
    $rows=@(Get-Content -LiteralPath $Path | ForEach-Object {
        if($_ -match '^([0-9a-fA-F]+)\s+[Tt]\s+(\S.*)$'){
            [pscustomobject]@{Address=(P125-Hex $Matches[1]);Name=$Matches[2]}
        }
    }|Sort-Object Address)
    $a=@($rows|Where-Object {$_.Name -in @('_hakios_p125_active','hakios_p125_active')})
    if($a.Count -ne 1 -or $a[0].Address -ne (P125-Hex $Anchor)){throw 'P125 symbol map anchor mismatch; use exact installer map'}
    $script:P125Symbols=$rows
    Life-Say "P125 symbol map anchor verified; text symbols=$($rows.Count)"
}
function P125-Symbol([uint64]$Address){
    if(-not $Address -or -not $script:P125Symbols.Count){return ''}
    $lo=0;$hi=$script:P125Symbols.Count-1;$best=-1
    while($lo -le $hi){
        $mid=[int][Math]::Floor(($lo+$hi)/2)
        if($script:P125Symbols[$mid].Address -le $Address){$best=$mid;$lo=$mid+1}else{$hi=$mid-1}
    }
    if($best -lt 0){return ''}
    $s=$script:P125Symbols[$best];$off=$Address-$s.Address
    if($off -gt 65536){return ''}
    if($off -eq 0){return $s.Name}
    return ($s.Name+'+0x'+$off.ToString('x'))
}
function P125-ReadSnapshot([string]$Label){
    $script:P12Phase=$Label
    $status=P125-Status
    if($status.armed -ne '0' -or $status.active -ne '0'){throw 'P125 table read requires stopped probe'}
    $rows=@{};$counts=@{};$coverage=@{}
    for($cpu=0;$cpu -lt 8;$cpu++){
        $c=P125-Fields (P12-Query "p125_counts$cpu")
        $v=P125-Fields (P12-Query "p125_coverage$cpu")
        if($c.version -ne '1' -or $v.version -ne '1' -or [int]$c.cpu -ne $cpu -or [int]$v.cpu -ne $cpu){throw 'P125 CPU snapshot mismatch'}
        $cs=@($c.counters.Split(',')|ForEach-Object {P125-Hex $_})
        $bs=@($v.bins.Split(',')|ForEach-Object {P125-Hex $_})
        $ts=@($v.totals.Split(',')|ForEach-Object {
            $a=$_.Split('/')
            if($a.Count -ne 2){throw 'P125 coverage shape'}
            [pscustomobject]@{Total=(P125-Hex $a[0]);Drop=(P125-Hex $a[1])}
        })
        $masks=@($v.pages.Split(',')|ForEach-Object {P125-Hex $_})
        if($cs.Count -ne 28 -or $bs.Count -ne 5 -or $ts.Count -ne 14 -or $masks.Count -ne 4){throw 'P125 counter shape mismatch'}
        $counts[$cpu]=$cs
        $coverage[$cpu]=[pscustomobject]@{Bins=$bs;Kinds=$ts;MapDrop=[uint64]$v.mapdrop}
        for($bank=0;$bank -lt 4;$bank++){
            for($bit=0;$bit -lt 64;$bit++){
                if(($masks[$bank] -band ([uint64]1 -shl $bit)) -eq 0){continue}
                $page=$cpu*256+$bank*64+$bit
                [void](P12-Query 'p125_select' "$page")
                $line=P12-Query 'p125_page'
                if($line -notmatch "^version=1 page=$page(?:\s|$)"){throw 'P125 stale page cursor'}
                $found=0
                foreach($m in [regex]::Matches($line,'(?:^| )r=([^\s]+)')){
                    $a=$m.Groups[1].Value.Split('/')
                    if($a.Count -ne 13){throw 'P125 row truncated'}
                    $n=@($a[0..11]|ForEach-Object {P125-Hex $_})
                    $slot=[int]$n[0];$kind=[int]$n[1]
                    if($slot -ge 512 -or [int][Math]::Floor($slot/2) -ne ($page%256) -or $kind -lt 1 -or $kind -gt 14){throw 'P125 row identity invalid'}
                    $key="$cpu/$slot"
                    if($rows.ContainsKey($key)){throw 'P125 duplicate slot'}
                    $rows[$key]=[pscustomobject]@{CPU=$cpu;Slot=$slot;Kind=$kind;Func=$n[2];Owner=$n[3];Aux=$n[4];Count=$n[5];Short=$n[6];ReqZero=$n[7];EffZero=$n[8];Flags=$n[9];Samples=$n[10];Wall=$n[11];Name=$a[12]}
                    $found++
                }
                if($found -lt 1 -or $found -gt 2){throw 'P125 page bitmap/row disagreement'}
            }
        }
    }
    $usb=@{}
    foreach($k in @('p6_usb_ticks','p6_usb_connected_ticks','p69_usb_disconnected_ticks')){$usb[$k]=[uint64](P12-Query $k)}
    return [pscustomobject]@{Status=$status;Rows=$rows;Counts=$counts;Coverage=$coverage;Usb=$usb}
}
function P125-Sub([uint64]$End,[uint64]$Begin){if($End -lt $Begin){throw 'P125 counter reset or reboot during capture'};return ($End-$Begin)}
function P125-Delta($Before,$After,[string]$Label,[int]$Mode,[string]$Stem){
    $st=$After.Status;$hz=[double]$st.hz
    if($hz -le 0 -or [uint64]$st.end -le [uint64]$st.start){throw 'P125 missing kernel interval; raw retained'}
    $seconds=([double]$st.end-[double]$st.start)/$hz
    $valid=($st.reason -eq '0' -and $st.display_on -eq '0' -and $st.active -eq '0' -and $st.armed -eq '0')
    if($Mode -eq 3 -and ($st.down -ne $st.start -or $st.up -ne $st.end -or $st.down -eq '0' -or $seconds -lt ($TraceSeconds-5))){$valid=$false}
    $data=New-Object 'System.Collections.Generic.List[object]'
    $cs=New-Object 'System.Collections.Generic.List[object]'
    $sum=@{}
    foreach($key in $Before.Rows.Keys){if(-not $After.Rows.ContainsKey($key)){throw 'P125 prior slot disappeared'}}
    foreach($key in $After.Rows.Keys){
        $r=$After.Rows[$key];$b=$Before.Rows[$key]
        if($null -ne $b -and ($b.Kind -ne $r.Kind -or $b.Func -ne $r.Func -or $b.Owner -ne $r.Owner -or $b.Aux -ne $r.Aux)){throw 'P125 slot reused; evidence invalid'}
        $d=@{}
        foreach($field in @('Count','Short','ReqZero','EffZero','Samples','Wall')){
            $old=if($null -eq $b){0}else{$b.$field}
            $d[$field]=P125-Sub $r.$field $old
        }
        if($d['Count'] -eq 0 -and $d['Samples'] -eq 0){continue}
        $mean=if($d['Samples']){[Math]::Round(1000000.0*$d['Wall']/$hz/$d['Samples'],3)}else{''}
        $row=[pscustomobject]@{phase=$Label;cpu=$r.CPU;slot=$r.Slot;kind=$script:P125Kinds[$r.Kind];kind_id=$r.Kind;func=('0x'+$r.Func.ToString('x'));symbol=(P125-Symbol $r.Func);owner=$r.Owner;aux=$r.Aux;name=$r.Name;calls=$d['Count'];calls_s=[Math]::Round($d['Count']/$seconds,2);short_lt1ms=$d['Short'];requested_leeway_zero=$d['ReqZero'];effective_leeway_zero=$d['EffZero'];cumulative_flags_or=('0x'+$r.Flags.ToString('x'));wall_sample_n=$d['Samples'];sampled_inclusive_mean_us=$mean}
        $data.Add($row)
        $k="$($r.CPU)/$($r.Kind)"
        if(-not $sum.ContainsKey($k)){$sum[$k]=[uint64]0}
        $sum[$k]+=$d['Count']
    }
    for($cpu=0;$cpu -lt 8;$cpu++){
        $c=@{};$o=[ordered]@{phase=$Label;cpu=$cpu;seconds=[Math]::Round($seconds,4)}
        for($i=0;$i -lt 28;$i++){
            $d=P125-Sub $After.Counts[$cpu][$i] $Before.Counts[$cpu][$i]
            $c[$script:P125Counters[$i]]=$d;$o[$script:P125Counters[$i]]=$d
        }
        if($c.queue_services -ne ($c.zero_cb+$c.nonzero_cb) -or $c.callbacks -lt $c.nonzero_cb){$valid=$false}
        $bins=@()
        for($i=0;$i -lt 5;$i++){$bins+=P125-Sub $After.Coverage[$cpu].Bins[$i] $Before.Coverage[$cpu].Bins[$i]}
        $o['requested_bins_lt100_500_1000_10000_ge10000us']=$bins -join ','
        $o['thread_map_drops_cumulative']=$After.Coverage[$cpu].MapDrop
        for($kind=1;$kind -le 14;$kind++){
            $a=$After.Coverage[$cpu].Kinds[$kind-1];$b=$Before.Coverage[$cpu].Kinds[$kind-1]
            $total=P125-Sub $a.Total $b.Total;$lost=P125-Sub $a.Drop $b.Drop
            $key="$cpu/$kind";$kept=if($sum.ContainsKey($key)){$sum[$key]}else{0}
            $o[("k{0}_total" -f $kind)]=$total;$o[("k{0}_drop" -f $kind)]=$lost
            if($total -ne ($lost+$kept)){$valid=$false}
        }
        if($Mode -ne 1 -and $o.k1_total -ne $c.callbacks){$valid=$false}
        $cs.Add([pscustomobject]$o)
    }
    $data|Sort-Object kind_id,@{Expression='calls';Descending=$true}|Export-Csv -NoTypeInformation -Encoding UTF8 -LiteralPath "$Stem.$Label.rows.csv"
    $cs|Export-Csv -NoTypeInformation -Encoding UTF8 -LiteralPath "$Stem.$Label.counts.csv"
    $big=@($cs|Where-Object {$_.cpu -in 3,4,5,6})
    $irq=($big|Measure-Object irq27 -Sum).Sum;$q=($big|Measure-Object queue_services -Sum).Sum
    $cb=($big|Measure-Object callbacks -Sum).Sum;$zero=($big|Measure-Object zero_cb -Sum).Sum
    $line="PHASE $Label mode=$Mode seconds=$([Math]::Round($seconds,3)) BIG_IRQ=$irq BIG_IRQ_s=$([Math]::Round($irq/$seconds,2)) BIG_QUEUE=$q BIG_CALLBACKS=$cb BIG_ZERO=$zero reason=$($st.reason) display_on=$($st.display_on) coverage_accounting=$valid"
    $script:P125Report.Add($line);Life-Say $line
    foreach($k in $After.Usb.Keys){$script:P125Report.Add("USB $Label $k delta=$(P125-Sub $After.Usb[$k] $Before.Usb[$k])")}
    foreach($kind in 1..14){
        $top=@($data|Where-Object {$_.kind_id -eq $kind}|Group-Object func,owner,aux|ForEach-Object {
            $r=$_.Group[0]
            [pscustomobject]@{calls=($_.Group|Measure-Object calls -Sum).Sum;func=$r.func;symbol=$r.symbol;owner=$r.owner;aux=$r.aux;name=$r.name}
        }|Sort-Object calls -Descending|Select-Object -First 8)
        foreach($r in $top){$script:P125Report.Add("TOP $Label $($script:P125Kinds[$kind]) calls=$($r.calls) func=$($r.func) symbol=$($r.symbol) owner=$($r.owner) aux=$($r.aux) name=$($r.name)")}
    }
    if(-not $valid){throw "P125 phase $Label incomplete: thermal/display/link/coverage evidence; reports retained"}
}
function P125-Open([string]$Name){
    $script:LifeSerial=[IO.Ports.SerialPort]::new($Name,115200,[IO.Ports.Parity]::None,8,[IO.Ports.StopBits]::One)
    $script:LifeSerial.DtrEnable=$true;$script:LifeSerial.ReadTimeout=160;$script:LifeSerial.WriteTimeout=4000
    $script:LifeSerial.ReadBufferSize=131072;$script:LifeSerial.Open()
}
function P125-PumpOff([int]$Seconds){
    $until=(Life-Now).AddSeconds($Seconds)
    while((Life-Now) -lt $until){
        Life-Pump;Life-CheckSafety (Life-Now);P12-EarlyThermalGuard
        if($script:LifeState -ne 'OFF'){throw 'P125 display woke; stop observation'}
    }
}
function P125-Offline {
    $target=if($ReconnectPort){$ReconnectPort}else{$Port}
    Life-Say 'P125 USB OFFLINE ARMED: disconnect USB now; do NOT wake screen'
    $until=(Life-Now).AddSeconds(20)
    while([IO.Ports.SerialPort]::GetPortNames() -contains $Port){
        if((Life-Now) -ge $until){throw 'P125 disconnect not observed; stop probe'}
        try{Life-Pump;Life-CheckSafety (Life-Now);P12-EarlyThermalGuard}
        catch [System.IO.IOException]{break}
        Start-Sleep -Milliseconds 100
    }
    $until=(Life-Now).AddSeconds(5)
    while([IO.Ports.SerialPort]::GetPortNames() -contains $Port -and (Life-Now) -lt $until){Start-Sleep -Milliseconds 200}
    if([IO.Ports.SerialPort]::GetPortNames() -contains $Port){throw 'P125 COM did not disappear'}
    if($null -ne $script:LifeCurrentSegment){$script:LifeCurrentSegment.End=Life-Now}
    $script:LifeCurrentSegment=$null
    try{$script:LifeSerial.Dispose()}catch{}
    $script:LifeSerial=$null
    Life-Say "P125 host COM disconnected; keep unplugged $TraceSeconds seconds"
    $until=(Life-Now).AddSeconds($TraceSeconds)
    while((Life-Now) -lt $until){
        if([IO.Ports.SerialPort]::GetPortNames() -contains $target){throw 'P125 reconnected early; offline phase invalid'}
        Start-Sleep -Milliseconds 250
    }
    Life-Say "P125 RECONNECT USB NOW to $target; keep screen OFF"
    $until=(Life-Now).AddSeconds(25)
    while([IO.Ports.SerialPort]::GetPortNames() -notcontains $target){
        if((Life-Now) -ge $until){throw 'P125 reconnect timeout; kernel probe lease self-expires'}
        Start-Sleep -Milliseconds 250
    }
    P125-Open $target
    $script:LifePartial='';$script:LifeSynced=$false;$script:LifeFenceSent=$false;$script:LifeFenceEcho=$false
    $script:LifeState='UNKNOWN';$script:LifeScene='UNKNOWN'
    Life-SyncReadOnly
    if($script:LifeState -ne 'OFF'){throw 'P125 reconnected while screen ON'}
}
function P125-Phase([string]$Label,[int]$Mode,[int]$Seconds,[string]$Stem,$Before){
    $script:P12Phase=$Label;$script:P125Dirty=$true
    [void](P12-Query 'p125_mode' "$Mode")
    $armed=P125-Status
    if([int]$armed.armed -ne $Mode){throw 'P125 mode arm not verified'}
    if($Mode -eq 3){P125-Offline}else{P125-PumpOff $Seconds}
    [void](P12-Query 'p125_mode' '0' -Cleanup)
    $script:P125Dirty=$false
    P125-PumpOff 2
    $after=P125-ReadSnapshot "$Label-END"
    P125-Delta $Before $after $Label $Mode $Stem
    return $after
}
function P125-Run {
    if(-not $Execute){
        Write-Host 'P12.5 preview: -Execute to measure OFF timer source. Trace=aggregate+detail; UsbCrossover=aggregate+A0/B/A1; OfflineOnly=B; AggregateOnly=aggregate. No automatic cooling wait.'
        return
    }
    if($env:OS -ne 'Windows_NT'){throw 'Windows serial runner only'}
    if([IO.Ports.SerialPort]::GetPortNames() -notcontains $Port){throw "COM port $Port unavailable"}
    if($ReconnectPort -and $ReconnectPort -notmatch '^COM[0-9]+$'){throw 'ReconnectPort must be COM number'}
    $stem=Join-Path (Get-Location).Path ('putty-S9-p125-timer-'+(Get-Date -Format 'yyyyMMdd-HHmmss'))
    $raw=$stem+'.log';$errorText=$null
    try{
        $script:LifeRaw=[IO.FileStream]::new($raw,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read)
        P125-Open $Port
        Life-SyncReadOnly
        $s=P125-Status
        if($s.armed -ne '0'){throw 'P125 previous probe still armed; bounded expiry required'}
        P125-Symbols $SymbolMap $s.anchor
        if($script:LifeState -eq 'ON'){
            Life-Say 'P125 press POWER ONCE to turn screen OFF now; no swipe'
            $until=(Life-Now).AddSeconds(25)
            while($script:LifeState -ne 'OFF' -and (Life-Now) -lt $until){Life-Pump;Life-CheckSafety (Life-Now)}
            if($script:LifeState -ne 'OFF'){throw 'P125 physical screen OFF not observed'}
        }
        P125-PumpOff 25
        $g=P12-Query 'g3d_lab_stats'
        if($g -notmatch ' domain=0 pll=0 ' -or $g -notmatch ' fault=0 '){throw 'P125 independent GPU OFF evidence missing'}
        $before=P125-ReadSnapshot 'BASE'
        if($Scenario -in @('Trace','UsbCrossover','AggregateOnly')){$before=P125-Phase 'AGG' 1 $AggregateSeconds $stem $before}
        if($Scenario -in @('Trace','UsbCrossover')){$before=P125-Phase 'A0' 2 $TraceSeconds $stem $before}
        if($Scenario -in @('UsbCrossover','OfflineOnly')){$before=P125-Phase 'B_OFFLINE' 3 $TraceSeconds $stem $before}
        if($Scenario -eq 'UsbCrossover'){$before=P125-Phase 'A1' 2 $TraceSeconds $stem $before}
        [void](P12-Query 'g3d_lab_stats')
    }catch{
        $errorText=$_.Exception.Message;Life-Say "P125 STOP $errorText"
    }finally{
        if($script:P125Dirty -and $null -ne $script:LifeSerial -and $script:LifeSerial.IsOpen){
            try{[void](P12-Query 'p125_mode' '0' -Cleanup);$script:P125Dirty=$false}catch{Life-Say 'P125 cleanup ACK missing; kernel lease <=180s'}
        }
        if($null -ne $script:LifeSerial){try{$script:LifeSerial.Dispose()}catch{};$script:LifeSerial=$null}
        if($null -ne $script:LifeRaw){$script:LifeRaw.Dispose();$script:LifeRaw=$null}
        if(Test-Path -LiteralPath $raw){
            $script:P125Report.Add('STOP='+$(if($errorText){$errorText}else{'COMPLETE'}))
            $script:P125Report.Add('Nested layers are not additive IRQ counts. Wall samples inclusive, not CPU time. USB link event duration not proof of physical cable or power. Names resolved only for matching live task generation; flags cumulative OR. USB poll deltas include snapshot traffic.')
            $script:P125Report|Set-Content -Encoding UTF8 -LiteralPath "$stem.p125-report.txt"
            $script:P12Results|Set-Content -Encoding UTF8 -LiteralPath "$stem.p125-snapshots.txt"
            $script:LifeEvents|Set-Content -Encoding UTF8 -LiteralPath "$stem.events.txt"
            $script:LifeSamples|Export-Csv -NoTypeInformation -Encoding UTF8 -LiteralPath "$stem.samples.csv"
            Write-Host "RAW LOG $raw"
        }
    }
    if($errorText){throw "P125 incomplete: $errorText"}
}
if(-not $LibraryOnly){P125-Run}
