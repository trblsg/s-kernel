#requires -Version 5.1
# P12.5 OFF timer observation. No changes to hardware or scheduling policy.
[CmdletBinding()]
param(
 [ValidatePattern('^COM[0-9]+$')][string]$Port='COM125',
 [string]$ReconnectPort='',
 [ValidateSet('Trace','UsbCrossover','OfflineOnly','AggregateOnly','FrozenSummary','FrozenDump','DriverTrials','EnergyTrials','EnergyOffline')][string]$Scenario='Trace',
 [ValidateRange(30,90)][int]$TraceSeconds=60,
 [ValidateRange(12,40)][int]$AggregateSeconds=20,
 [string]$SymbolMap='',
 [string]$ResumeSnapshot='',
 [ValidateRange(50,500)][int]$PagePaceMs=80,
 [ValidateRange(15,60)][int]$DriverSeconds=30,
 [string]$DriverModules='COMBINED',
 [ValidateRange(15,300)][int]$EnergySeconds=30,
 [string]$EnergyModules='USB,ABOX,BT,WIFI,TICKER',
 [switch]$AllowLowerFloor,
 [switch]$Execute,[switch]$LibraryOnly
)
Set-StrictMode -Version 2
$ErrorActionPreference='Stop'
$script:P125Exec=$Execute;$script:P125Lib=$LibraryOnly
. (Join-Path $PSScriptRoot 's9-p12-dual-serial.ps1') -LibraryOnly -Port $Port
$Execute=$script:P125Exec;$LibraryOnly=$script:P125Lib
$script:P125HostEnabled=$true
$script:P126HostEnabled=$true
$script:P127HostEnabled=$true
$script:P127Dirty=$false
$script:P126Dirty=$false
$script:P125Dirty=$false
$script:P125Stem=''
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
function P125-CheckFrozenIdentity($Old,$New){
    foreach($k in @('version','active','armed','start','end','hz','reason','anchor','slots','rows','counters','kinds')){
        if($Old.Status[$k] -ne $New.Status[$k]){throw "P125 resume identity mismatch $k"}
    }
    for($cpu=0;$cpu -lt 8;$cpu++){
        if(($Old.Counts[$cpu] -join ',') -ne ($New.Counts[$cpu] -join ',') -or
           ($Old.PageMasks[$cpu] -join ',') -ne ($New.PageMasks[$cpu] -join ',') -or
           ($Old.Coverage[$cpu].Bins -join ',') -ne ($New.Coverage[$cpu].Bins -join ',')){
            throw "P125 resume counters changed CPU=$cpu"
        }
        for($i=0;$i -lt 14;$i++){
            if($Old.Coverage[$cpu].Kinds[$i].Total -ne $New.Coverage[$cpu].Kinds[$i].Total -or
               $Old.Coverage[$cpu].Kinds[$i].Drop -ne $New.Coverage[$cpu].Kinds[$i].Drop){throw 'P125 resume coverage changed'}
        }
    }
}
function P125-ReadSnapshot([string]$Label,[switch]$CountsOnly){
    $script:P12Phase=$Label
    $status=P125-Status
    if($status.armed -ne '0' -or $status.active -ne '0'){throw 'P125 table read requires stopped probe'}
    $rows=@{};$counts=@{};$coverage=@{};$pageMasks=@{}
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
        $pageMasks[$cpu]=$masks
        $coverage[$cpu]=[pscustomobject]@{Bins=$bs;Kinds=$ts;MapDrop=[uint64]$v.mapdrop}
    }
    $usb=@{}
    foreach($k in @('p6_usb_ticks','p6_usb_connected_ticks','p69_usb_disconnected_ticks')){$usb[$k]=[uint64](P12-Query $k)}
    $snapshot=[pscustomobject]@{Status=$status;Rows=$rows;Counts=$counts;Coverage=$coverage;Usb=$usb;PageMasks=$pageMasks}
    if($script:P125Stem){
        # Write the frozen scalar evidence before any page request. Even if
        # COM drops later, do not lose IRQ/queue/deadline counters.
        $snapshot|Export-Clixml -Depth 8 -LiteralPath "$script:P125Stem.$Label.frozen.clixml"
    }
    if($CountsOnly){return $snapshot}
    if(-not $status.ContainsKey('page_lookup') -or $status.page_lookup -ne 'scalar'){
        throw 'P125 unsafe legacy page resolver; use FrozenSummary or matched P12.5.1 kernel'
    }
    $saved=@{}
    if($ResumeSnapshot -and $Label -eq 'RECOVERED'){
        if($ResumeSnapshot -notmatch '\.frozen\.clixml$'){throw 'ResumeSnapshot must be a frozen.clixml checkpoint'}
        $old=Import-Clixml -LiteralPath $ResumeSnapshot
        P125-CheckFrozenIdentity $old $snapshot
        $pageFile=$ResumeSnapshot -replace '\.frozen\.clixml$','.pages.txt'
        if(Test-Path -LiteralPath $pageFile){
            foreach($line in Get-Content -LiteralPath $pageFile){
                if($line -notmatch '^version=1 page=(\d+) '){throw 'P125 invalid saved page'}
                $n=[int]$Matches[1]
                if($saved.ContainsKey($n)){throw 'P125 duplicate saved page'}
                $saved[$n]=$line
            }
        }
        Life-Say "P125 resume identity verified; saved pages=$($saved.Count)"
    }
    $reused=0
    for($cpu=0;$cpu -lt 8;$cpu++){
        $masks=$pageMasks[$cpu]
        for($bank=0;$bank -lt 4;$bank++){
            for($bit=0;$bit -lt 64;$bit++){
                if(($masks[$bank] -band ([uint64]1 -shl $bit)) -eq 0){continue}
                $page=$cpu*256+$bank*64+$bit
                Life-Say "P125 frozen page CPU=$cpu page=$page"
                if($saved.ContainsKey($page)){
                    $line=$saved[$page];$reused++
                }else{
                    [void](P12-Query 'p125_select' "$page")
                    Start-Sleep -Milliseconds $PagePaceMs
                    $line=P12-Query 'p125_page'
                    Start-Sleep -Milliseconds $PagePaceMs
                }
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
                if($script:P125Stem){Add-Content -Encoding UTF8 -LiteralPath "$script:P125Stem.$Label.pages.txt" -Value $line}
            }
        }
    }
    if($reused -ne $saved.Count){throw 'P125 saved pages outside frozen bitmap'}
    if($script:P125Stem){$snapshot|Export-Clixml -Depth 8 -LiteralPath "$script:P125Stem.$Label.complete.clixml"}
    return $snapshot
}
function P125-Sub([uint64]$End,[uint64]$Begin){if($End -lt $Begin){throw 'P125 counter reset or reboot during capture'};return ($End-$Begin)}
function P125-Delta($Before,$After,[string]$Label,[int]$Mode,[string]$Stem,[switch]$OfflineAggregate){
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
        if($Mode -ne 1 -and -not $OfflineAggregate -and $o.k1_total -ne $c.callbacks){$valid=$false}
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
function P125-Phase([string]$Label,[int]$Mode,[int]$Seconds,[string]$Stem,$Before,[switch]$CountsOnly,[switch]$OfflineAggregate){
    $script:P12Phase=$Label;$script:P125Dirty=$true
    [void](P12-Query 'p125_mode' "$Mode")
    $armed=P125-Status
    if([int]$armed.armed -ne $Mode){throw 'P125 mode arm not verified'}
    if($Mode -eq 3){P125-Offline}else{P125-PumpOff $Seconds}
    [void](P12-Query 'p125_mode' '0' -Cleanup)
    $script:P125Dirty=$false
    P125-PumpOff 2
    $after=P125-ReadSnapshot "$Label-END" -CountsOnly:$CountsOnly
    if($Scenario -eq 'DriverTrials'){
        $diag=P12-Query 'p126_diag'
        $script:P125Report.Add("P126 DIAG $Label cumulative $diag")
    }
    P125-Delta $Before $after $Label $Mode $Stem -OfflineAggregate:$OfflineAggregate
    return $after
}
function P126-Status{
    $v=P125-Fields (P12-Query 'p126_status')
    if($v.version -ne '1' -or $v.lease_s -ne '120' -or $v.watchdog_ms -ne '100'){throw 'P126 matched ABI required'}
    foreach($k in @('mode','reason','haptic','wifi_worker','gnss','touch','touch_leeway','shub')){if(-not $v.ContainsKey($k)){throw "P126 status missing $k"}}
    return $v
}
function P126-DriverTrials([string]$Stem){
    $modules=@{HAPTIC=1;WIFI=2;GNSS=4;TOUCH=8;LEEWAY=16;SHUB=32;COMBINED=15}
    $fields=@{HAPTIC='haptic';WIFI='wifi_worker';GNSS='gnss';TOUCH='touch';LEEWAY='touch_leeway';SHUB='shub'}
    $selected=@($DriverModules.Split(',')|ForEach-Object {$_.Trim().ToUpperInvariant()})
    if(@($selected|Where-Object {-not $modules.ContainsKey($_)}).Count -or
       @($selected|Select-Object -Unique).Count -ne $selected.Count){throw 'Invalid/duplicate DriverModules'}
    $v=P126-Status
    if($v.mode -ne '0'){throw 'P126 prior policy still active; do not overlap trials'}
    $before=P125-ReadSnapshot 'P126-BASE' -CountsOnly
    $before=P125-Phase 'P126-A0' 1 $DriverSeconds $Stem $before -CountsOnly
    foreach($name in $selected){
        $mask=$modules[$name]
        $pre=P126-Status
        if($pre.mode -ne '0'){throw 'P126 baseline mode not stock'}
        $script:P126Dirty=$true
        [void](P12-Query 'p126_mode' "$mask")
        $armed=P126-Status
        if([int]$armed.mode -ne $mask){throw 'P126 arm not verified'}
        P125-PumpOff 2
        $after=P125-Phase "P126-$name-B" 1 $DriverSeconds $Stem $before -CountsOnly
        $post=P126-Status
        if([int]$post.mode -ne $mask -or $post.reason -ne '0'){throw "P126 $name lease revoked; B invalid"}
        $hits=@()
        foreach($field in $fields.Values){
            $delta=P125-Sub ([uint64]$post[$field]) ([uint64]$pre[$field])
            $hits+="$field=$delta"
        }
        [void](P12-Query 'p126_mode' '0' -Cleanup)
        $script:P126Dirty=$false
        $restored=P126-Status
        if($restored.mode -ne '0'){throw 'P126 restore failed'}
        P125-PumpOff 2
        $before=P125-Phase "P126-$name-A1" 1 $DriverSeconds $Stem $after -CountsOnly
        $applied=if($name -eq 'COMBINED'){@($fields.Values|Where-Object {[uint64]$post[$_] -gt [uint64]$pre[$_]}).Count -gt 0}else{[uint64]$post[$fields[$name]] -gt [uint64]$pre[$fields[$name]]}
        $result=if($applied){'COMPLETE'}else{'NO_ELIGIBLE_PATH'}
        $line="P126 MODULE $name $result applied=$applied hits=$($hits -join ',')"
        $script:P125Report.Add($line);Life-Say $line
    }
}
function P127-Status {
    $s=P125-Fields (P12-Query 'p127_status')
    foreach($k in @('version','mode','reason','lease_s','little_request','little_cur','ufs_safe','ufs_busy','floor_hits','floor_bypass')){if(-not $s.ContainsKey($k)){throw "P127 missing status $k"}}
    if($s.version -ne '1'){throw 'P127 matched ABI required'}
    return $s
}
function P127-Read {
    $w=@{}
    for($cpu=0;$cpu -lt 8;$cpu++){
        $v=P125-Fields (P12-Query "p127_wfi$cpu")
        if($v.version -ne '1' -or [int]$v.cpu -ne $cpu){throw 'P127 WFI ABI mismatch'}
        $bins=@($v.bins.Split(',')|ForEach-Object {
            $x=$_.Split('/');if($x.Count -ne 2){throw 'P127 WFI pair invalid'}
            [pscustomobject]@{N=(P125-Hex $x[0]);Ticks=(P125-Hex $x[1])}
        })
        if($bins.Count -ne 7){throw 'P127 WFI bin count'}
        $w[$cpu]=[pscustomobject]@{Bins=$bins;SGI=(P125-Hex $v.sgi);PPI=(P125-Hex $v.ppi);SPI=(P125-Hex $v.spi)}
    }
    $seq=P125-Fields (P12-Query 'p127_fg_seq')
    if($seq.version -ne '1' -or $seq.slots -ne '64'){throw 'P127 FG ABI mismatch'}
    return [pscustomobject]@{Wfi=$w;Seq=[uint64]$seq.seq;Status=(P127-Status);Metrics=(P125-Fields (P12-Query 'p127_metrics'));Wifi=(P125-Fields (P12-Query 'p127_wifi'));BT=(P125-Fields (P12-Query 'p127_bt'));Ticker=(P125-Fields (P12-Query 'p127_ticker'));Bus=(P12-Query 'p127_bus');Shub=(P12-Query 'p127_shub');Diag=(P12-Query 'p126_diag')}
}
function P127-Delta($Before,$After,$Timer,[string]$Label,[string]$Stem){
    $hz=[double]$Timer.hz;$seconds=([double]$Timer.end-[double]$Timer.start)/$hz
    if($seconds -le 0){throw 'P127 invalid kernel interval'}
    $labels=@('lt100us','100_499us','500_999us','1_1.999ms','2_3.499ms','3.5_9.999ms','ge10ms')
    $rows=@()
    for($cpu=0;$cpu -lt 8;$cpu++){
        $a=$After.Wfi[$cpu];$b=$Before.Wfi[$cpu]
        $n=0.0;$t=0.0
        for($i=0;$i -lt 7;$i++){
            $n+=P125-Sub $a.Bins[$i].N $b.Bins[$i].N
            $t+=P125-Sub $a.Bins[$i].Ticks $b.Bins[$i].Ticks
        }
        for($i=0;$i -lt 7;$i++){
            $count=P125-Sub $a.Bins[$i].N $b.Bins[$i].N
            $ticks=P125-Sub $a.Bins[$i].Ticks $b.Bins[$i].Ticks
            $rows+=[pscustomobject]@{phase=$Label;cpu=$cpu;seconds=$seconds;bin=$labels[$i];count=$count;ticks=$ticks;wfi_seconds=$ticks/$hz;count_pct=$(if($n){100*$count/$n}else{0});wfi_time_pct=$(if($t){100*$ticks/$t}else{0});window_pct=100*$ticks/$hz/$seconds;sgi=(P125-Sub $a.SGI $b.SGI);ppi=(P125-Sub $a.PPI $b.PPI);spi=(P125-Sub $a.SPI $b.SPI)}
        }
    }
    $rows|Export-Csv -NoTypeInformation -Encoding UTF8 -LiteralPath "$Stem.$Label.wfi.csv"
    $big=@($rows|Where-Object {$_.cpu -in 3,4,5,6})
    $all=($big|Measure-Object count -Sum).Sum;$long=($big|Where-Object {$_.bin -in '3.5_9.999ms','ge10ms'}|Measure-Object count -Sum).Sum
    $time=($big|Measure-Object ticks -Sum).Sum;$lt=($big|Where-Object {$_.bin -in '3.5_9.999ms','ge10ms'}|Measure-Object ticks -Sum).Sum
    $line="P127 WFI $Label BIG_count=$all ge3.5ms_count=$long BIG_ticks=$time ge3.5ms_ticks=$lt"
    $script:P125Report.Add($line);Life-Say $line
    foreach($group in @('Metrics','Wifi','BT','Ticker')){
        $m=@()
        foreach($k in $After.$group.Keys){
            if($k -eq 'version'){continue}
            $m+="$k=$(P125-Sub ([uint64]$After.$group[$k]) ([uint64]$Before.$group[$k]))"
        }
        $script:P125Report.Add("P127 $group $Label delta $($m -join ' ')")
    }
    $script:P125Report.Add("P127 BUS $Label $($After.Bus)")
    $script:P125Report.Add("P127 SHUB $Label $($After.Shub)")
    $script:P125Report.Add("P127 DIAG $Label $($After.Diag)")
    $num=P125-Sub $After.Seq $Before.Seq
    if($num -gt 64){throw 'P127 gauge ring overwritten'}
    $pages=@{};$samples=@{}
    for($i=[uint64]($Before.Seq+1);$i -le $After.Seq;$i++){
        $page=[int][Math]::Floor((($i-1)%64)/4)
        $pages[$page]=$true
    }
    foreach($page in ($pages.Keys|Sort-Object)){
        $line=P12-Query "p127_fg$page"
        if($line -notmatch "^version=1 page=$page(?: |$)"){throw 'P127 gauge page mismatch'}
        foreach($m in [regex]::Matches($line,'(?:^| )r=(\d+)/(\d+)/(-?\d+)/(-?\d+)/(\d+)/(\d+)/(\d+)/(\d+)')){
            $seq=[uint64]$m.Groups[1].Value
            if($seq -le $Before.Seq -or $seq -gt $After.Seq){continue}
            if($samples.ContainsKey($seq)){throw 'P127 duplicate gauge sample'}
            $ticks=[uint64]$m.Groups[2].Value
            $ua=[int]$m.Groups[3].Value;$uv=[uint32]$m.Groups[5].Value
            $samples[$seq]=[pscustomobject]@{phase=$Label;seq=$seq;ticks=$ticks;seconds_from_start=([double]$ticks-[double]$Timer.start)/$hz;in_window=($ticks -ge [uint64]$Timer.start -and $ticks -le [uint64]$Timer.end);current_ua=$ua;avg_ua=[int]$m.Groups[4].Value;voltage_uv=$uv;external=[int]$m.Groups[6].Value;charger_state=[int]$m.Groups[7].Value;qh_raw=[uint32]$m.Groups[8].Value;net_battery_w=[double]$ua*[double]$uv/1e12}
        }
        Start-Sleep -Milliseconds $PagePaceMs
    }
    if($samples.Count -ne $num){throw 'P127 gauge sequence gap'}
    if($num){$samples.Values|Sort-Object seq|Export-Csv -NoTypeInformation -Encoding UTF8 -LiteralPath "$Stem.$Label.fg.csv"}
    $script:P125Report.Add("P127 FG $Label samples=$num; signed NET battery power, not whole-system watts when external=1")
}
function P127-Window([string]$Label,[int]$Mask,[int]$Seconds,[string]$Stem,$BeforeTimer,[switch]$Offline){
    $initial=P127-Status
    if($initial.mode -ne '0'){throw 'P127 old mode still active'}
    $before=P127-Read
    $script:P127Dirty=$true
    [void](P12-Query 'p127_mode' "$Mask")
    $armed=P127-Status
    if([int]$armed.mode -ne $Mask -or [int]$armed.lease_s -lt ($Seconds+12)){throw 'P127 lease arm refused or too short'}
    P125-PumpOff 2
    $timerMode=if($Offline){3}else{1}
    if($Offline){$script:TraceSeconds=$Seconds}
    $afterTimer=P125-Phase $Label $timerMode $Seconds $Stem $BeforeTimer -CountsOnly -OfflineAggregate:$Offline
    $after=P127-Read
    if([int]$after.Status.mode -ne $Mask -or $after.Status.reason -ne '0'){throw "P127 $Label lease revoked; phase invalid"}
    P127-Delta $before $after $afterTimer.Status $Label $Stem
    [void](P12-Query 'p127_mode' '0' -Cleanup)
    P125-PumpOff 2
    $restored=P127-Status
    if($restored.mode -ne '0'){throw 'P127 restore ACK missing'}
    if((($Mask -shr 4) -band 7) -and [int]$restored.little_cur -lt 1053000){throw 'P127 LITTLE stock floor readback missing'}
    $m=P125-Fields (P12-Query 'p127_metrics')
    $w=P125-Fields (P12-Query 'p127_wifi')
    $bt=P125-Fields (P12-Query 'p127_bt')
    if((P125-Sub ([uint64]$bt.attach) ([uint64]$before.BT.attach)) -ne (P125-Sub ([uint64]$bt.detach) ([uint64]$before.BT.detach))){throw 'P127 UART IRQ restore counts disagree'}
    if([uint64]$m.abox_fault -gt [uint64]$before.Metrics.abox_fault -or [uint64]$m.psr_fault -gt [uint64]$before.Metrics.psr_fault){throw 'P127 hardware transition fault; stop suite'}
    if((P125-Sub ([uint64]$m.abox_stop) ([uint64]$before.Metrics.abox_stop)) -ne (P125-Sub ([uint64]$m.abox_resume) ([uint64]$before.Metrics.abox_resume))){throw 'P127 RDMA restore counts disagree'}
    if((P125-Sub ([uint64]$w.attach) ([uint64]$before.Wifi.attach)) -ne (P125-Sub ([uint64]$w.detach) ([uint64]$before.Wifi.detach))){throw 'P127 INTx restore counts disagree'}
    if((P125-Sub ([uint64]$m.psr_mask) ([uint64]$before.Metrics.psr_mask)) -gt (P125-Sub ([uint64]$m.psr_unmask) ([uint64]$before.Metrics.psr_unmask))){throw 'P127 PSR unmask not proven'}
    $script:P127Dirty=$false
    $script:P125Report.Add("P127 RESTORE $Label mode=0 $($restored.little_cur) kHz")
    return [pscustomobject]@{Timer=$afterTimer;Pre=$before;Post=$after;Restored=$restored}
}
function P127-RunEnergy([string]$Stem){
    $defs=@{USB=3;ABOX=5;PSR=9;WIFI=129;BT=257;TICKER=513;LITTLE949=17;LITTLE832=33;LITTLE715=49;LITTLE598=65;LITTLE455=81}
    $selected=@($EnergyModules.Split(',')|ForEach-Object {$_.Trim().ToUpperInvariant()})
    if($Scenario -eq 'EnergyTrials'){
        if(@($selected|Where-Object {-not $defs.ContainsKey($_)}).Count -or @($selected|Select-Object -Unique).Count -ne $selected.Count){throw 'P127 invalid/duplicate EnergyModules'}
        if(@($selected|Where-Object {$_ -in 'LITTLE832','LITTLE715','LITTLE598','LITTLE455'}).Count -and -not $AllowLowerFloor){throw 'P127 lower OPP needs prior step review and -AllowLowerFloor'}
        if($EnergySeconds -gt 60 -and @($selected|Where-Object {$_ -ne 'USB'}).Count){throw 'P127 new hardware transitions limited to <=60s until functional qualification'}
    }
    if($Scenario -eq 'EnergyOffline' -and $EnergySeconds -lt 60){throw 'P127 offline requires 60..300s'}
    if((P126-Status).mode -ne '0' -or (P127-Status).mode -ne '0'){throw 'P127 prior policy active'}
    $base=P125-ReadSnapshot 'P127-BASE' -CountsOnly
    $a=P127-Window 'P127-A0' 1 $EnergySeconds $Stem $base
    if($Scenario -eq 'EnergyOffline'){
        $b=P127-Window 'P127-OFFLINE-B' 1 $EnergySeconds $Stem $a.Timer -Offline
        [void](P127-Window 'P127-OFFLINE-A1' 1 $EnergySeconds $Stem $b.Timer)
        return
    }
    foreach($name in $selected){
        $b=P127-Window "P127-$name-B" $defs[$name] $EnergySeconds $Stem $a.Timer
        $eligible=switch($name){
            USB {[uint64]$b.Post.Metrics.usb_8ms -gt [uint64]$b.Pre.Metrics.usb_8ms}
            ABOX {[uint64]$b.Post.Metrics.abox_idle_wait -gt [uint64]$b.Pre.Metrics.abox_idle_wait}
            PSR {[uint64]$b.Post.Metrics.psr_mask -gt [uint64]$b.Pre.Metrics.psr_mask}
            BT {([uint64]$b.Post.BT.attach -gt [uint64]$b.Pre.BT.attach) -and ([uint64]$b.Post.BT.fallback -eq [uint64]$b.Pre.BT.fallback)}
            TICKER {[uint64]$b.Post.Ticker.wait -gt [uint64]$b.Pre.Ticker.wait}
            WIFI {([uint64]$b.Post.Wifi.attach -gt [uint64]$b.Pre.Wifi.attach) -and ([uint64]$b.Post.Wifi.fallback -eq [uint64]$b.Pre.Wifi.fallback)}
            default {([uint64]$b.Post.Status.floor_hits -gt [uint64]$b.Pre.Status.floor_hits) -and ([int]$b.Post.Status.little_cur -eq [int]$b.Post.Status.little_request)}
        }
        $line="P127 MODULE $name "+$(if($eligible){'APPLIED'}else{'NO_ELIGIBLE_PATH_OR_FALLBACK'})
        $script:P125Report.Add($line);Life-Say $line
        $a=P127-Window "P127-$name-A1" 1 $EnergySeconds $Stem $b.Timer
    }
}
function P125-Run {
    if(-not $Execute){
        Write-Host 'P12.5 preview: -Execute to measure OFF timer source. Trace=aggregate+detail; UsbCrossover=aggregate+A0/B/A1; OfflineOnly=B; AggregateOnly=aggregate; FrozenSummary=read-only counters; FrozenDump=read-only counters and pages on fixed firmware. No automatic cooling wait. DriverTrials=selective bounded A/B/A without page dumps; default COMBINED. ResumeSnapshot=FrozenDump checkpoint. EnergyTrials=guarded P12.7 A/B/A; EnergyOffline=connected/unplug/reconnect aggregate+WFI+FG.'
        return
    }
    if($env:OS -ne 'Windows_NT'){throw 'Windows serial runner only'}
    if([IO.Ports.SerialPort]::GetPortNames() -notcontains $Port){throw "COM port $Port unavailable"}
    if($ReconnectPort -and $ReconnectPort -notmatch '^COM[0-9]+$'){throw 'ReconnectPort must be COM number'}
    $stem=Join-Path (Get-Location).Path ('putty-S9-p125-timer-'+(Get-Date -Format 'yyyyMMdd-HHmmss'))
    $raw=$stem+'.log';$errorText=$null;$script:P125Stem=$stem
    try{
        $script:LifeRaw=[IO.FileStream]::new($raw,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read)
        P125-Open $Port
        Life-SyncReadOnly
        $s=P125-Status
        if($s.armed -ne '0'){throw 'P125 previous probe still armed; bounded expiry required'}
        P125-Symbols $SymbolMap $s.anchor
        if($ResumeSnapshot -and $Scenario -ne 'FrozenDump'){throw 'ResumeSnapshot requires FrozenDump'}
        if($Scenario -in @('FrozenSummary','FrozenDump')){
            # Read-only recovery: do not arm/stop probe, change policy, or
            # require another OFF window. FrozenSummary is safe for old ABI.
            $frozen=P125-ReadSnapshot 'RECOVERED' -CountsOnly:($Scenario -eq 'FrozenSummary')
            $script:P125Report.Add("RECOVERED cumulative_only=1 rows=$($frozen.Rows.Count) start=$($frozen.Status.start) end=$($frozen.Status.end) reason=$($frozen.Status.reason)")
            Life-Say 'P125 recovered frozen cumulative counters; no new trial'
            return
        }
        if(-not $s.ContainsKey('page_lookup') -or $s.page_lookup -ne 'scalar'){
            throw 'P125 legacy page resolver refused; use FrozenSummary or matched P12.5.1 kernel'
        }
        if($script:LifeState -eq 'ON'){
            Life-Say 'P125 press POWER ONCE to turn screen OFF now; no swipe'
            $until=(Life-Now).AddSeconds(25)
            while($script:LifeState -ne 'OFF' -and (Life-Now) -lt $until){Life-Pump;Life-CheckSafety (Life-Now)}
            if($script:LifeState -ne 'OFF'){throw 'P125 physical screen OFF not observed'}
        }
        P125-PumpOff 25
        $g=P12-Query 'g3d_lab_stats'
        if($g -notmatch ' domain=0 pll=0 ' -or $g -notmatch ' fault=0 '){throw 'P125 independent GPU OFF evidence missing'}
        if($Scenario -in @('EnergyTrials','EnergyOffline')){
            P127-RunEnergy $stem
            return
        }
        if($Scenario -eq 'DriverTrials'){
            P126-DriverTrials $stem
            return
        }
        $before=P125-ReadSnapshot 'BASE'
        if($Scenario -in @('Trace','UsbCrossover','AggregateOnly')){$before=P125-Phase 'AGG' 1 $AggregateSeconds $stem $before}
        if($Scenario -in @('Trace','UsbCrossover')){$before=P125-Phase 'A0' 2 $TraceSeconds $stem $before}
        if($Scenario -in @('UsbCrossover','OfflineOnly')){$before=P125-Phase 'B_OFFLINE' 3 $TraceSeconds $stem $before}
        if($Scenario -eq 'UsbCrossover'){$before=P125-Phase 'A1' 2 $TraceSeconds $stem $before}
        [void](P12-Query 'g3d_lab_stats')
    }catch{
        $errorText=$_.Exception.Message;Life-Say "P125 STOP $errorText"
    }finally{
        if($script:P127Dirty -and $null -ne $script:LifeSerial -and $script:LifeSerial.IsOpen){
            try{[void](P12-Query 'p127_mode' '0' -Cleanup);$script:P127Dirty=$false}catch{Life-Say 'P127 restore ACK missing; bounded kernel expiry'}
        }
        if($script:P126Dirty -and $null -ne $script:LifeSerial -and $script:LifeSerial.IsOpen){
            try{[void](P12-Query 'p126_mode' '0' -Cleanup);$script:P126Dirty=$false}catch{Life-Say 'P126 cleanup ACK missing; 120s bounded kernel lease'}
        }
        if($script:P125Dirty -and $null -ne $script:LifeSerial -and $script:LifeSerial.IsOpen){
            try{[void](P12-Query 'p125_mode' '0' -Cleanup);$script:P125Dirty=$false}catch{Life-Say 'P125 cleanup ACK missing; kernel lease <=180s'}
        }
        if($null -ne $script:LifeSerial){try{$script:LifeSerial.Dispose()}catch{};$script:LifeSerial=$null}
        if($null -ne $script:LifeRaw){$script:LifeRaw.Dispose();$script:LifeRaw=$null}
        if(Test-Path -LiteralPath $raw){
            $script:P125Report.Add('STOP='+$(if($errorText){$errorText}else{'COMPLETE'}))
            $script:P125Report.Add('Nested layers are not additive IRQ counts. Wall samples inclusive, not CPU time. USB link event duration not proof of physical cable or power. Process names intentionally unresolved by USB readback; retain scalar PID and task generation. Flags cumulative OR. USB poll deltas include snapshot traffic.')
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
