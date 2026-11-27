#requires -Version 5.1
# P12.5 OFF timer observation. No changes to hardware or scheduling policy.
[CmdletBinding()]
param(
 [ValidatePattern('^COM[0-9]+$')][string]$Port='COM125',
 [string]$ReconnectPort='',
 [ValidateSet('Trace','UsbCrossover','OfflineOnly','AggregateOnly','FrozenSummary','FrozenDump','DriverTrials','EnergyTrials','EnergyOffline','Idle52','Idle52Offline','Idle52Recover','Audit53','Audit53Recover','Audit53Compare','Guard53','Hardware54','GpuCacheCheck','P1273Diagnostics','BusTrace','ThermalTrace','ThermalTraceDump','ThermalGpuDump','OffBurstCapABA','OfflineABA','FunctionalGate','CombinedConnected')][string]$Scenario='Trace',
 [ValidateRange(30,90)][int]$TraceSeconds=60,
 [ValidateRange(12,40)][int]$AggregateSeconds=20,
 [string]$SymbolMap='',
 [string]$ResumeSnapshot='',
 [ValidateRange(50,500)][int]$PagePaceMs=80,
 [ValidateRange(15,60)][int]$DriverSeconds=30,
 [string]$DriverModules='COMBINED',
 [ValidateRange(15,300)][int]$EnergySeconds=30,
 [string]$EnergyModules='USB,ABOX,BT,WIFI,TICKER,PSR,LITTLE949',
 [ValidateRange(30,250)][int]$ReadPaceMs=60,
 [switch]$AllowLowerFloor,
 [switch]$RequireGpuCache,
 [switch]$P1273Telemetry,
 [switch]$P12752Idle,
 [string]$P12752PriorReport='',
 [uint64]$ExpectedStart=0,
 [switch]$P12754,
 [ValidateSet('STOCK120','STOCK60','BASE15','PSR','LITTLE949','LITTLE832','LITTLE715','LITTLE598','ABOX','BT','WIFI','TICKER','BUS')][string]$Audit53Module='STOCK120',
 [string]$Audit53Checkpoint='',
 [string[]]$Audit53Reports=@(),
 [string]$Audit53PriorReport='',
 [switch]$AllowOffCap,
 [switch]$AllowStrongerCap,
 [ValidateSet('ABOX','BT','WIFI','TICKER','LITTLE949','COMBINED5','COMBINED6')][string]$OfflineModule='LITTLE949',
 [ValidateSet('USB','ABOX','BT','WIFI','TICKER','LITTLE949','LITTLE832','LITTLE715','LITTLE598','COMBINED5','COMBINED6')][string]$GateModule='USB',
 [ValidateSet('COMBINED5','COMBINED6')][string]$CombinedProfile='COMBINED5',
 [switch]$AllowFunctionalGate,
 [switch]$AllowCombined,
 [switch]$AllowOfflineTrial,
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
$script:P1272HostEnabled=$true
$script:P12752IdleAllowed=[bool]$P12752Idle
$script:P12752IdleDirty=$false
$script:P12754HostEnabled=[bool]$P12754
$script:P12753HostEnabled=$Scenario -in @('Audit53','Audit53Recover','Guard53','Hardware54')
$script:P12753ArmAllowed=($Scenario -eq 'Audit53' -and [bool]$AllowOfflineTrial) -or ($Scenario -eq 'Hardware54' -and $P12754)
$script:P1273HostEnabled=([bool]$P1273Telemetry -or [bool]$P12752Idle -or $script:P12753HostEnabled -or $Scenario -in @('P1273Diagnostics','BusTrace','ThermalTrace','ThermalTraceDump','ThermalGpuDump','OffBurstCapABA','OfflineABA','FunctionalGate','CombinedConnected'))
$script:P1273GateDirty=$false
$script:P1273OfflineDirty=$false
$script:P1273CapDirty=$false
$script:P1273TraceDirty=$false
$script:P1272Dirty=$false
$script:P1272Required=$RequireGpuCache
# P12.7.1 transport only, no new firmware ABI.
$script:P12ReadPaceMs=$ReadPaceMs
$script:P12ReadAttempts=2
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
    $snapshot=[pscustomobject]@{Status=$status;Rows=$rows;Counts=$counts;Coverage=$coverage;Usb=$usb;PageMasks=$pageMasks}
    if($script:P125Stem){
        # Save all eight CPU counters BEFORE the optional legacy USB reads.
        $snapshot|Export-Clixml -Depth 8 -LiteralPath "$script:P125Stem.$Label.scalar.clixml"
    }
    foreach($k in @('p6_usb_ticks','p6_usb_connected_ticks','p69_usb_disconnected_ticks')){
        try{$usb[$k]=[uint64](P12-Query $k)}
        catch{
            # P127 has independent USB cadence counters. Only exhausted
            # READ timeout is optional; refusal, transport and safety
            # failures still stop the suite. Never substitute zero.
            if($Scenario -notin @('EnergyTrials','EnergyOffline','OfflineABA','Audit53','Audit53Recover') -or $_.Exception.Message -ne "Missing echo and sysctl readback sysctl kern.hakios.$k"){throw}
            $msg="P127 USB_OPTIONAL_MISSING $Label $k (no delta for this counter)"
            $script:P125Report.Add($msg);Life-Say $msg
        }
    }
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
function P125-LinkIntervalValid($Status,[int]$Seconds,[switch]$Fixed){
    if([double]$Status.hz -le 0 -or [uint64]$Status.end -le [uint64]$Status.start -or
       $Status.down -eq '0' -or $Status.down -ne $Status.start){return $false}
    $duration=([double]$Status.end-[double]$Status.start)/[double]$Status.hz
    if($Fixed){return ([uint64]$Status.up -ge [uint64]$Status.end -and [Math]::Abs($duration-60) -le 0.001)}
    return ($Status.up -eq $Status.end -and $duration -ge ($Seconds-5))
}
function P125-Delta($Before,$After,[string]$Label,[int]$Mode,[string]$Stem,[switch]$OfflineAggregate){
    $st=$After.Status;$hz=[double]$st.hz
    if($hz -le 0 -or [uint64]$st.end -le [uint64]$st.start){throw 'P125 missing kernel interval; raw retained'}
    $seconds=([double]$st.end-[double]$st.start)/$hz
    $fixed=($Scenario -eq 'OfflineABA')
    $expectedReason=if($fixed){'10'}else{'0'}
    $valid=($st.reason -eq $expectedReason -and $st.display_on -eq '0' -and $st.active -eq '0' -and $st.armed -eq '0')
    if($fixed){
        if($Mode -ne 3 -or -not (P125-LinkIntervalValid $st 60 -Fixed)){$valid=$false}
    }elseif($Mode -eq 3 -and -not (P125-LinkIntervalValid $st $TraceSeconds)){$valid=$false}
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
    foreach($k in @('p6_usb_ticks','p6_usb_connected_ticks','p69_usb_disconnected_ticks')){
        if($Before.Usb.ContainsKey($k) -and $After.Usb.ContainsKey($k)){
            $script:P125Report.Add("USB $Label $k delta=$(P125-Sub $After.Usb[$k] $Before.Usb[$k])")
        }else{$script:P125Report.Add("USB $Label $k delta=UNAVAILABLE")}
    }
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
function P125-PortNames { return [IO.Ports.SerialPort]::GetPortNames() }
function P125-IsReadDisconnect($Exception){
    # PowerShell wraps SerialPort.Read() exceptions. A closed port is
    # expected ONLY while waiting for the explicitly requested unplug.
    for($e=$Exception;$null -ne $e;$e=$e.InnerException){
        if($e -is [System.IO.IOException]){return $true}
        if($e -is [System.InvalidOperationException] -and
           ($e.Message -match '(?i)port.*(?:closed|not open)' -or
            ($null -ne $script:LifeSerial -and -not $script:LifeSerial.IsOpen))){return $true}
    }
    return $false
}
function P125-Offline([int]$WaitSeconds=$TraceSeconds) {
    $script:P125OfflineTransportLost=$false
    $target=if($ReconnectPort){$ReconnectPort}else{$Port}
    $source=$Port
    # P12752 Idle52Offline is a kernel-timestamped measurement. A Windows
    # COM name is registry/enumeration metadata, not a USB link witness.
    # Other callers retain their old strict host enumeration behavior.
    $audit53=$Scenario -eq 'Audit53'
    $kernelLinkProof=($P12752Idle -and $Scenario -in @('Idle52Offline','EnergyOffline')) -or $audit53
    if($audit53){Life-Say 'P12753 OFFLINE ARMED: unplug USB now; if screen wakes, turn it OFF once, then leave device alone'}
    else{Life-Say 'P125 USB OFFLINE ARMED: disconnect USB now; do NOT wake screen'}
    $until=(Life-Now).AddSeconds(20)
    while((P125-PortNames) -contains $source){
        if((Life-Now) -ge $until){throw 'P125 disconnect not observed; stop probe'}
        # Do not catch thermal, display or parser errors as link loss.
        Life-CheckSafety (Life-Now);P12-EarlyThermalGuard
        if(-not $audit53 -and $script:LifeState -ne 'OFF'){throw 'P125 display woke before USB disconnect'}
        try{Life-Pump}
        catch{
            if(-not (P125-IsReadDisconnect $_.Exception)){throw}
            Life-Say 'P125 serial read lost during requested unplug; closing host handle'
            break
        }
        Life-CheckSafety (Life-Now);P12-EarlyThermalGuard
        if(-not $audit53 -and $script:LifeState -ne 'OFF'){throw 'P125 display woke before USB disconnect'}
        Start-Sleep -Milliseconds 100
    }
    # Release the old handle immediately, before consulting Windows.
    # Keeping it alive can itself delay removal of the virtual COM name.
    if($null -ne $script:LifeCurrentSegment){$script:LifeCurrentSegment.End=Life-Now}
    $script:LifeCurrentSegment=$null
    $script:P125OfflineTransportLost=$true
    try{$script:LifeSerial.Dispose()}catch{}
    $script:LifeSerial=$null
    if($kernelLinkProof){
        if((P125-PortNames) -contains $source){
            Life-Say 'P125 COM name still listed after handle loss; provisional offline, kernel down/up proof required'
        }else{
            Life-Say 'P125 COM name absent; provisional offline, kernel down/up proof required'
        }
    }else{
        $until=(Life-Now).AddSeconds(10)
        while((P125-PortNames) -contains $source -and (Life-Now) -lt $until){Start-Sleep -Milliseconds 200}
        if((P125-PortNames) -contains $source){throw 'P125 COM did not disappear; offline interval not verified'}
    }
    $hostWait=if($Scenario -eq 'OfflineABA'){$WaitSeconds+5}else{$WaitSeconds}
    if($kernelLinkProof){Life-Say "P125 host handle closed; keep unplugged $hostWait seconds (kernel verification pending)"}
    else{Life-Say "P125 host COM disconnected; keep unplugged $hostWait seconds"}
    $until=(Life-Now).AddSeconds($hostWait)
    while((Life-Now) -lt $until){
        # For P12752 no opens, reads, writes or enumeration-based early
        # reconnect guesses. An early physical link-up freezes P125 in
        # kernel; P125-Delta rejects an insufficient down/up interval.
        if(-not $kernelLinkProof){
            $ports=@(P125-PortNames)
            if($ports -contains $source -or $ports -contains $target){throw 'P125 reconnected early; offline phase invalid'}
        }
        Start-Sleep -Milliseconds 250
    }
    Life-Say "P125 RECONNECT USB NOW to $target; keep screen OFF"
    $until=(Life-Now).AddSeconds(25)
    if($kernelLinkProof){
        # A persistent virtual COM name may allow Open() before the
        # device can answer. A successful Open alone is not a reconnect.
        # Retry only the exact COM and the read-only fence, never arm.
        $until=(Life-Now).AddSeconds(45)
        $AckTimeoutSeconds=5
        while((Life-Now) -lt $until){
            Life-CheckSafety (Life-Now);P12-EarlyThermalGuard
            try{
                P125-Open $target
                $script:LifePartial='';$script:LifeSynced=$false;$script:LifeFenceSent=$false;$script:LifeFenceEcho=$false
                $script:LifeState='UNKNOWN';$script:LifeScene='UNKNOWN'
                Life-SyncReadOnly
                if(-not $audit53 -and $script:LifeState -ne 'OFF'){throw 'P125 reconnected while screen ON'}
                $script:Port=$target
                return
            }catch{
                $retry=(P125-IsReadDisconnect $_.Exception) -or
                    ($_.Exception.Message -like 'P10 display sysctl reply absent;*')
                for($e=$_.Exception;$null -ne $e;$e=$e.InnerException){
                    if($e -is [System.UnauthorizedAccessException] -or $e -is [System.TimeoutException]){$retry=$true;break}
                }
                if($null -ne $script:LifeSerial){try{$script:LifeSerial.Dispose()}catch{};$script:LifeSerial=$null}
                if(-not $retry){throw}
                Life-Say 'P125 reconnect handle/fence not ready; retrying exact COM read-only'
                Start-Sleep -Milliseconds 500
            }
        }
        throw 'P125 reconnect timeout; kernel probe lease self-expires'
    }
    # Enumeration can precede a usable device handle. Retry only
    # opening the exact requested COM, never re-arm or resend a policy.
    $opened=$false
    while((Life-Now) -lt $until){
        if((P125-PortNames) -contains $target){
            try{P125-Open $target;$opened=$true;break}
            catch{
                $retry=$false
                for($e=$_.Exception;$null -ne $e;$e=$e.InnerException){
                    if($e -is [System.IO.IOException] -or $e -is [System.UnauthorizedAccessException]){$retry=$true;break}
                }
                if($null -ne $script:LifeSerial){try{$script:LifeSerial.Dispose()}catch{};$script:LifeSerial=$null}
                if(-not $retry){throw}
            }
        }
        Start-Sleep -Milliseconds 250
    }
    if(-not $opened){throw 'P125 reconnect timeout; kernel probe lease self-expires'}
    $script:Port=$target
    $script:LifePartial='';$script:LifeSynced=$false;$script:LifeFenceSent=$false;$script:LifeFenceEcho=$false
    $script:LifeState='UNKNOWN';$script:LifeScene='UNKNOWN'
    Life-SyncReadOnly
    if($script:LifeState -ne 'OFF'){throw 'P125 reconnected while screen ON'}
}
function P12752-ReadIdle([string]$Stem,[string]$Label,$status,[switch]$FrozenOnly){
    $line=P12-Query 'p12752_idle_info';$info=P125-Fields $line
    Set-Content -Encoding UTF8 -LiteralPath "$Stem.$Label.idle-info.txt" -Value $line
    if($info.phase -ne $status.start -or [int]$info.count -gt 96 -or [int]$info.bus -gt 13){throw 'P12752 idle phase identity/coverage mismatch'}
    $rows=@()
    for($i=0;$i -lt [int]$info.count;$i++){
        P12-EarlyThermalGuard
        [void](P12-Query 'p12752_idle_select' "$i")
        $line=P12-Query 'p12752_idle_page';$v=P125-Fields $line
        if([int]$v.select -ne $i -or $v.phase -ne $info.phase -or [uint64]$v.at -lt [uint64]$status.start -or [uint64]$v.at -gt [uint64]$status.end){throw 'P12752 idle row identity mismatch'}
        Add-Content -Encoding UTF8 -LiteralPath "$Stem.$Label.idle-pages.txt" -Value $line
        $rows+=[pscustomobject]@{phase=$Label;at=$v.at;seconds=([double]$v.at-[double]$status.start)/[double]$status.hz;kind=[int]$v.kind;domain=[int]$v.index;rate_khz=$v.raw;valid=$v.valid;errno=$v.errno;latency_us=1000000*[double]$v.latency/[double]$status.hz;big_mc=$v.big;little_mc=$v.lit;gpu_mc=$v.gpu;battery_c=$v.bat;age_ms=$v.age;big_sum=$v.bs;little_sum=$v.ls;big_khz=$v.bkhz;little_khz=$v.lkhz;big_measured_mhz=$v.bmeas;little_measured_mhz=$v.lmeas;policy=$v.policy;driver=$v.driver;decon_frames=$v.refresh}
    }
    if($rows.Count){$rows|Export-Csv -NoTypeInformation -Encoding UTF8 -LiteralPath "$Stem.$Label.idle.csv"}
    $thermal=@($rows|Where-Object kind -eq 1)
    $bus=@($rows|Where-Object kind -eq 2)
    $duration=([double]$status.end-[double]$status.start)/[double]$status.hz
    $valid=($thermal.Count -ge 5 -and $info.reason -eq '0' -and $info.drops -eq '0' -and $status.reason -in @('0','10') -and $status.display_on -eq '0' -and $status.armed -eq '0' -and @($thermal|Where-Object valid -ne '1').Count -eq 0)
    if($valid -and ($thermal[-1].seconds-$thermal[0].seconds) -lt 0.8*$duration){$valid=$false}
    $summary=[pscustomobject]@{label=$Label;phase=$info.phase;count=$info.count;drops=$info.drops;reason=$info.reason;timer_reason=$status.reason;thermal_valid=$valid;bus_rows=$bus.Count;bus_valid=@($bus|Where-Object valid -eq '1').Count;peak_big_mc=if($thermal.Count){($thermal|Measure-Object big_mc -Maximum).Maximum}else{$null};peak_lit_mc=if($thermal.Count){($thermal|Measure-Object little_mc -Maximum).Maximum}else{$null}}
    $summary|ConvertTo-Json|Set-Content -Encoding UTF8 -LiteralPath "$Stem.$Label.idle-summary.json"
    if(-not $FrozenOnly){
        [void](P12-Query 'p12752_idle_mode' '0' -Cleanup)
        $script:P12752IdleDirty=$false
    }
    $script:P125Report.Add("P12752 IDLE $Label thermal_valid=$valid rows=$($rows.Count) bus=$($bus.Count) cutoff=$($info.reason)")
    # Don't qualify a partial window, but preserve all evidence before
    # caller decides whether to stop the A/B/A.
    return $summary
}
function P12752-RecoverFrozen([string]$Stem,[uint64]$Start){
    # No new arm, policy writes or screen requirements. Ordinary host
    # thermal checks remain in force. Only the bounded idle cursor writes.
    $script:P12Phase='P12752-RECOVER'
    $s=P125-Status
    if(!$Start -or [uint64]$s.start -ne $Start -or [uint64]$s.end -le $Start -or
       $s.armed -ne '0' -or $s.active -ne '0'){throw 'P12752 recovery requires the exact stopped phase; no new trial'}
    $s|ConvertTo-Json|Set-Content -Encoding UTF8 -LiteralPath "$Stem.RECOVERED.timer.json"
    if((P127-Status).mode -ne '0' -or (P126-Status).mode -ne '0' -or
       (P12-Query 'p12752_idle_mode') -ne '0'){throw 'P12752 recovery requires stopped policy and recorder'}
    $info=P125-Fields (P12-Query 'p12752_idle_info')
    if($info.mode -ne '0' -or [uint64]$info.phase -ne $Start){throw 'P12752 recovery recorder phase mismatch'}
    [void](P12752-ReadIdle $Stem 'RECOVERED' $s -FrozenOnly)
    # FG ring is time-keyed; retain only actual in-window samples.
    # No inference of complete offline power from a cutoff.
    $seq=P125-Fields (P12-Query 'p127_fg_seq')
    $samples=@()
    for($page=0;$page -lt 16;$page++){
        $line=P12-Query "p127_fg$page"
        Add-Content -Encoding UTF8 -LiteralPath "$Stem.RECOVERED.fg-pages.txt" -Value $line
        foreach($m in [regex]::Matches($line,'(?:^| )r=(\d+)/(\d+)/(-?\d+)/(-?\d+)/(\d+)/(\d+)/(\d+)/(\d+)')){
            $ticks=[uint64]$m.Groups[2].Value
            if($ticks -lt $Start -or $ticks -gt [uint64]$s.end){continue}
            $ua=[int]$m.Groups[3].Value;$uv=[uint32]$m.Groups[5].Value
            $samples+=[pscustomobject]@{seq=[uint64]$m.Groups[1].Value;ticks=$ticks;seconds=([double]$ticks-$Start)/[double]$s.hz;current_ua=$ua;avg_ua=[int]$m.Groups[4].Value;voltage_uv=$uv;external=[int]$m.Groups[6].Value;charger_state=[int]$m.Groups[7].Value;qh_raw=[uint32]$m.Groups[8].Value;net_battery_w=[double]$ua*$uv/1e12}
        }
        if($samples.Count){$samples|Sort-Object seq|Export-Csv -NoTypeInformation -Encoding UTF8 -LiteralPath "$Stem.RECOVERED.fg.csv"}
    }
    $last=P125-Fields (P12-Query 'p127_fg_seq')
    if($last.seq -ne $seq.seq){throw 'P12752 recovery FG ring changed'}
    [pscustomobject]@{verified=$false;reason=$s.reason;phase=$s.start;sample_count=$samples.Count;note='Recovery only; no complete power qualification'}|ConvertTo-Json|Set-Content -Encoding UTF8 -LiteralPath "$Stem.RECOVERED.power.json"
    # Raw cumulative counters require original before snapshot for deltas.
    for($cpu=0;$cpu -lt 8;$cpu++){
        foreach($key in @("p125_counts$cpu","p125_coverage$cpu","p1273_wfi$cpu")){
            $line=P12-Query $key
            Add-Content -Encoding UTF8 -LiteralPath "$Stem.RECOVERED.cumulative.txt" -Value "$key=$line"
        }
    }
    $after=P125-Status
    foreach($key in @('start','end','reason','anchor','hz','down','up')){
        if($s[$key] -ne $after[$key]){throw 'P12752 recovery phase changed; partial evidence only'}
    }
    if($after.armed -ne '0' -or $after.active -ne '0'){throw 'P12752 recovery no longer frozen'}
    $script:P125Report.Add("P12752 RECOVERY_ONLY phase=$Start reason=$($s.reason) fg_samples=$($samples.Count) cumulative_only=1 power_verified=False")
    Life-Say 'P12752 frozen evidence recovered; no new trial or power qualification'
}
function P125-RejoinMatches($Armed,$Rejoined){
    return ($Rejoined.anchor -eq $Armed.anchor -and $Rejoined.hz -eq $Armed.hz -and
        [uint64]$Rejoined.now -gt [uint64]$Armed.now -and
        [uint64]$Rejoined.start -ge [uint64]$Armed.now -and
        $Rejoined.down -eq $Rejoined.start -and $Rejoined.down -ne '0')
}
function P125-Phase([string]$Label,[int]$Mode,[int]$Seconds,[string]$Stem,$Before,[switch]$CountsOnly,[switch]$OfflineAggregate){
    $script:P12Phase=$Label;$script:P125Dirty=$true
    if($P12752Idle){
        $idleMode=if($Scenario -eq 'BusTrace'){2}else{1}
        $script:P12752IdleDirty=$true
        [void](P12-Query 'p12752_idle_mode' "$idleMode")
        if([int](P12-Query 'p12752_idle_mode') -ne $idleMode){throw 'P12752 idle arm not verified'}
    }
    [void](P12-Query 'p125_mode' "$Mode")
    $armed=P125-Status
    if([int]$armed.armed -ne $Mode){throw 'P125 mode arm not verified'}
    if($Mode -eq 3){
        $script:P125OfflineTransportLost=$false
        try{
            P125-Offline
            # The live fence is not enough to prove the original kernel phase.
            # Check the arm identity before any write on the re-opened port.
            $rejoined=P125-Status
            if(-not (P125-RejoinMatches $armed $rejoined)){
                throw 'P125 reconnected kernel/phase identity mismatch; old lease bounded, raw retained'
            }
            $msg="P125 REJOIN verified start=$($rejoined.start) down=$($rejoined.down) up=$($rejoined.up) reason=$($rejoined.reason)"
            $script:P125Report.Add($msg);Life-Say $msg
            # This verifies phase identity only. Duration, stop reason,
            # thermal coverage and battery external=0 are checked later.
        }catch{
            if($script:P125OfflineTransportLost){
                # Never send restore commands to an unverified new session.
                # Preserve dirty flags as unconfirmed; close transport so
                # the outer cleanup cannot write to this unknown boot.
                if($null -ne $script:LifeSerial){try{$script:LifeSerial.Dispose()}catch{};$script:LifeSerial=$null}
                Life-Say 'P125 offline rejoin unverified; no restore ACK, original kernel leases bounded'
            }
            throw
        }
    }else{P125-PumpOff $Seconds}
    [void](P12-Query 'p125_mode' '0' -Cleanup)
    $script:P125Dirty=$false
    if($P12752Idle -and $Mode -eq 3 -and $rejoined.reason -ne '0'){
        # Already frozen before reconnect; don't demand another OFF
        # window. Preserve cutoff data if cool, then stop the suite.
        [void](P12-Query 'p127_mode' '0' -Cleanup)
        $script:P127Dirty=$false
        P12752-RecoverFrozen $Stem ([uint64]$rejoined.start)
        $script:P12752IdleDirty=$false
        throw "P12752 offline cutoff reason=$($rejoined.reason); frozen evidence retained"
    }
    P125-PumpOff 2
    $after=P125-ReadSnapshot "$Label-END" -CountsOnly:$CountsOnly
    $idle=$null
    # Read pages after the P127 policy has been stopped, not inside its lease.
    # On an aborted phase, retain idle rows before the existing delta rejection.
    if($P12752Idle -and ($Scenario -eq 'DriverTrials' -or $after.Status.reason -notin @('0','10'))){$idle=P12752-ReadIdle $Stem $Label $after.Status}
    if($Scenario -eq 'DriverTrials'){
        $diag=P12-Query 'p126_diag'
        $script:P125Report.Add("P126 DIAG $Label cumulative $diag")
    }
    P125-Delta $Before $after $Label $Mode $Stem -OfflineAggregate:$OfflineAggregate
    if($P12752Idle -and $null -ne $idle -and -not $idle.thermal_valid){
        $script:P125Report.Add("P12752 $Label partial/cutoff; no energy qualification or next experiment")
        # P127-Window will still read FG/WFI in its post block where possible.
        if($Scenario -eq 'DriverTrials'){throw 'P12752 quiet window aborted; restore and review'}
    }
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
        $key=if($script:P1273HostEnabled){"p1273_wfi$cpu"}else{"p127_wfi$cpu"}
        $version=if($script:P1273HostEnabled){'2'}else{'1'}
        $v=P125-Fields (P12-Query $key)
        if($v.version -ne $version -or [int]$v.cpu -ne $cpu){throw 'P127 WFI ABI mismatch'}
        $bins=@($v.bins.Split(',')|ForEach-Object {
            $x=$_.Split('/');if($x.Count -ne 2){throw 'P127 WFI pair invalid'}
            [pscustomobject]@{N=(P125-Hex $x[0]);Ticks=(P125-Hex $x[1])}
        })
        $expected=if($script:P1273HostEnabled){9}else{7}
        if($bins.Count -ne $expected){throw 'P127 WFI bin count'}
        $w[$cpu]=[pscustomobject]@{Bins=$bins;SGI=(P125-Hex $v.sgi);PPI=(P125-Hex $v.ppi);SPI=(P125-Hex $v.spi)}
    }
    $seq=P125-Fields (P12-Query 'p127_fg_seq')
    if($seq.version -ne '1' -or $seq.slots -ne '64'){throw 'P127 FG ABI mismatch'}
    return [pscustomobject]@{Wfi=$w;Seq=[uint64]$seq.seq;Status=(P127-Status);Metrics=(P125-Fields (P12-Query 'p127_metrics'));Wifi=(P125-Fields (P12-Query 'p127_wifi'));BT=(P125-Fields (P12-Query 'p127_bt'));Ticker=(P125-Fields (P12-Query 'p127_ticker'));Bus=(P12-Query 'p127_bus');Shub=(P12-Query 'p127_shub');Diag=(P12-Query 'p126_diag')}
}
function P127-Delta($Before,$After,$Timer,[string]$Label,[string]$Stem){
    $hz=[double]$Timer.hz;$seconds=([double]$Timer.end-[double]$Timer.start)/$hz
    if($seconds -le 0){throw 'P127 invalid kernel interval'}
    $labels=@('lt100us','100_499us','500_999us','1_1.999ms','2_3.499ms','3.5_9.999ms')
    if($script:P1273HostEnabled){$labels+=@('10_19.999ms','20_49.999ms','ge50ms')}else{$labels+='ge10ms'}
    $rows=@()
    for($cpu=0;$cpu -lt 8;$cpu++){
        $a=$After.Wfi[$cpu];$b=$Before.Wfi[$cpu]
        $n=0.0;$t=0.0
        for($i=0;$i -lt $labels.Count;$i++){
            $n+=P125-Sub $a.Bins[$i].N $b.Bins[$i].N
            $t+=P125-Sub $a.Bins[$i].Ticks $b.Bins[$i].Ticks
        }
        for($i=0;$i -lt $labels.Count;$i++){
            $count=P125-Sub $a.Bins[$i].N $b.Bins[$i].N
            $ticks=P125-Sub $a.Bins[$i].Ticks $b.Bins[$i].Ticks
            $rows+=[pscustomobject]@{phase=$Label;cpu=$cpu;seconds=$seconds;bin=$labels[$i];count=$count;ticks=$ticks;wfi_seconds=$ticks/$hz;count_pct=$(if($n){100*$count/$n}else{0});wfi_time_pct=$(if($t){100*$ticks/$t}else{0});window_pct=100*$ticks/$hz/$seconds;sgi=(P125-Sub $a.SGI $b.SGI);ppi=(P125-Sub $a.PPI $b.PPI);spi=(P125-Sub $a.SPI $b.SPI)}
        }
    }
    $rows|Export-Csv -NoTypeInformation -Encoding UTF8 -LiteralPath "$Stem.$Label.wfi.csv"
    $big=@($rows|Where-Object {$_.cpu -in 3,4,5,6})
    $all=($big|Measure-Object count -Sum).Sum;$long=($big|Where-Object {$_.bin -in @('3.5_9.999ms','ge10ms','10_19.999ms','20_49.999ms','ge50ms')}|Measure-Object count -Sum).Sum
    $time=($big|Measure-Object ticks -Sum).Sum;$lt=($big|Where-Object {$_.bin -in @('3.5_9.999ms','ge10ms','10_19.999ms','20_49.999ms','ge50ms')}|Measure-Object ticks -Sum).Sum
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
        Add-Content -Encoding UTF8 -LiteralPath "$Stem.$Label.fg-pages.txt" -Value $line
        if($line -notmatch "^version=1 page=$page(?: |$)"){throw 'P127 gauge page mismatch'}
        foreach($m in [regex]::Matches($line,'(?:^| )r=(\d+)/(\d+)/(-?\d+)/(-?\d+)/(\d+)/(\d+)/(\d+)/(\d+)')){
            $seq=[uint64]$m.Groups[1].Value
            if($seq -le $Before.Seq -or $seq -gt $After.Seq){continue}
            if($samples.ContainsKey($seq)){throw 'P127 duplicate gauge sample'}
            $ticks=[uint64]$m.Groups[2].Value
            $ua=[int]$m.Groups[3].Value;$uv=[uint32]$m.Groups[5].Value
            $samples[$seq]=[pscustomobject]@{phase=$Label;seq=$seq;ticks=$ticks;seconds_from_start=([double]$ticks-[double]$Timer.start)/$hz;in_window=($ticks -ge [uint64]$Timer.start -and $ticks -le [uint64]$Timer.end);current_ua=$ua;avg_ua=[int]$m.Groups[4].Value;voltage_uv=$uv;external=[int]$m.Groups[6].Value;charger_state=[int]$m.Groups[7].Value;qh_raw=[uint32]$m.Groups[8].Value;net_battery_w=[double]$ua*[double]$uv/1e12}
        }
        if($samples.Count){$samples.Values|Sort-Object seq|Export-Csv -NoTypeInformation -Encoding UTF8 -LiteralPath "$Stem.$Label.fg-partial.csv"}
        Start-Sleep -Milliseconds $PagePaceMs
    }
    if($samples.Count -ne $num){throw 'P127 gauge sequence gap'}
    if($num){$samples.Values|Sort-Object seq|Export-Csv -NoTypeInformation -Encoding UTF8 -LiteralPath "$Stem.$Label.fg.csv"}
    if($P12752Idle){
        $w=@($samples.Values|Where-Object in_window|Sort-Object ticks)
        $offline=$Label -match 'OFFLINE'
        $valid=$offline -and $w.Count -ge 2 -and $Timer.reason -in @('0','10') -and [uint64]$Timer.down -ne 0 -and $Timer.down -eq $Timer.start -and $Timer.display_on -eq '0' -and $seconds -ge 55
        if($Timer.reason -eq '0' -and $Timer.up -ne $Timer.end){$valid=$false}
        $joules=0.0;$covered=0.0
        foreach($v in $w){if($v.external -ne 0 -or $v.current_ua -ge 0 -or $v.voltage_uv -lt 2500000 -or $v.voltage_uv -gt 5000000){$valid=$false}}
        for($i=1;$i -lt $w.Count;$i++){
            $dt=([double]$w[$i].ticks-[double]$w[$i-1].ticks)/$hz
            if($dt -le 0 -or $dt -gt 15 -or ([uint64]$w[$i].seq-[uint64]$w[$i-1].seq) -ne 1){$valid=$false;continue}
            $covered+=$dt;$joules-=($w[$i].net_battery_w+$w[$i-1].net_battery_w)*$dt/2
        }
        if($covered -lt 0.8*$seconds){$valid=$false}
        $qfile="$Stem.$Label.idle-summary.json"
        if(-not (Test-Path -LiteralPath $qfile) -or -not (Get-Content -Raw -LiteralPath $qfile|ConvertFrom-Json).thermal_valid){$valid=$false}
        [pscustomobject]@{phase=$Label;offline=$offline;verified=$valid;window_s=$seconds;covered_s=$covered;samples=$w.Count;discharge_wh=if($valid){$joules/3600}else{$null};mean_discharge_w=if($valid){$joules/$covered}else{$null};note='Covered interval only; no extrapolation, connected net battery is not system power'}|ConvertTo-Json|Set-Content -Encoding UTF8 -LiteralPath "$Stem.$Label.power.json"
    }
    $script:P125Report.Add("P127 FG $Label samples=$num; signed NET battery power, not whole-system watts when external=1")
}
function P1272-Caps {
    $line=P12-Query 'p1272_gpu_caps'
    $c=P125-Fields $line
    if($c.version -ne '1' -or $c.ready -ne '1' -or $c.mask -ne '1ffffffffff' -or [uint64]$c.denied -ne 0){
        throw "P1272 GPU capability cache or MMIO guard check failed: $line"
    }
    return $c
}
function P1272-Check {
    $script:P12Phase='GPU-CACHE-CHECK'
    $pre=P1272-Caps
    $state=P125-Fields (P12-Query 'p1272_probe_state')
    if($state.eligible -ne '1' -or $state.domain -ne '0' -or $state.pll -ne '0' -or $state.mux -ne '0' -or $state.fault -ne '0' -or $pre.live -ne '0'){throw 'P1272 not eligible: keep display OFF and allow cooling'}
    if((P125-Status).armed -ne '0' -or (P126-Status).mode -ne '0' -or (P127-Status).mode -ne '0'){throw 'P1272 requires all timer/driver trials stopped'}
    if((P12-Query 'p1272_probe_request') -ne '0'){throw 'P1272 prior request pending'}
    $script:P1272ProbeLines.Clear()
    $script:P1272Dirty=$true
    [void](P12-Query 'p1272_probe_request' '1')
    Life-Say 'P1272 waiting for fresh IOKit GET_PARAM and single real GPU job; keep screen OFF'
    $end=(Life-Now).AddSeconds(60)
    $done=$false
    while((Life-Now) -lt $end){
        P125-PumpOff 1
        if(@($script:P1272ProbeLines|Where-Object {$_ -match 'p1272-probe: (?:INCONCLUSIVE|GET_PARAM .* refused)'}).Count){throw 'P1272 qualification failed or inconclusive; see raw log'}
        if(@($script:P1272ProbeLines|Where-Object {$_ -match 'p1272-probe: COMPLETE PASS wake=\d+->\d+ domain=15 safe=1 tz=1 caps_equal=1 job_good=1\s*$'}).Count){$done=$true;break}
    }
    if(-not $done -or -not @($script:P1272ProbeLines|Where-Object {$_ -match 'p1272-probe: OFF_GET_PARAM PASS count=41 domain=0 pll=0 mux=0 wake=\d+ off=\d+\s*$'}).Count){throw 'P1272 missing fresh OFF/real-job proof (boot p1272_probe=1, host request once per boot)'}
    [void](P12-Query 'p1272_probe_request' '0' -Cleanup)
    $script:P1272Dirty=$false
    $post=P1272-Caps
    if((P125-Sub ([uint64]$post.off_hits) ([uint64]$pre.off_hits)) -lt 41 -or (P125-Sub ([uint64]$post.off_param2) ([uint64]$pre.off_param2)) -lt 1){throw 'P1272 OFF capability counter evidence missing'}
    P125-PumpOff 25
    $final=P125-Fields (P12-Query 'p1272_probe_state')
    if($final.domain -ne '0' -or $final.pll -ne '0' -or $final.mux -ne '0' -or $final.fault -ne '0' -or [uint64]$final.wake -le [uint64]$state.wake -or [uint64]$final.off -le [uint64]$state.off -or $final.wake_fail -ne $state.wake_fail){throw 'P1272 re-gate or wake proof missing'}
    [void](P1272-Caps)
    $script:P125Report.Add('P1272 GPU_CACHE_CHECK PASS fresh 41-property GET_PARAM OFF, real write job, identical properties ON, re-gated')
    Life-Say 'P1272 GPU_CACHE_CHECK PASS'
}
function P127-Window([string]$Label,[int]$Mask,[int]$Seconds,[string]$Stem,$BeforeTimer,[switch]$Offline){
    $script:P12Phase="$Label-PRE"
    if($script:P1272Required){[void](P1272-Caps)}
    $initial=P127-Status
    if($initial.mode -ne '0'){throw 'P127 old mode still active'}
    $before=P127-Read
    [pscustomobject]@{Version=1;Label=$Label;Mask=$Mask;Seconds=$Seconds;Offline=[bool]$Offline;BeforeTimer=$BeforeTimer;Before=$before}|Export-Clixml -Depth 12 -LiteralPath "$Stem.$Label.energy-pre.clixml"
    $psrPre=$null
    if($script:P1273HostEnabled -and ($Mask -eq 9 -or $Scenario -eq 'EnergyTrials')){
        $psrPre=P125-Fields (P12-Query 'p1273_psr')
        Add-Content -Encoding UTF8 -LiteralPath "$Stem.$Label.psr.txt" -Value ($psrPre|ConvertTo-Json -Compress)
        $expectedPsr=if($P12752Idle){'0'}else{'300'}
        if($Mask -eq 9 -and ($psrPre.ready -ne '1' -or $psrPre.boot_ms -ne $expectedPsr -or $psrPre.governor -ne '0' -or ($psrPre.owner -ne '0' -and $P12752Idle) -or $psrPre.owner -eq '2')){
            throw "PSR comparison requires boot_ms=$expectedPsr and restored ownership"
        }
    }
    $fixed=($Scenario -eq 'OfflineABA')
    if($fixed){
        if(-not $Offline -or $Seconds -ne 60){throw 'P1273 fixed offline phase requires 60s'}
        $script:P1273OfflineDirty=$true
        [void](P12-Query 'p1273_offline_s' '60')
        if((P12-Query 'p1273_offline_s') -ne '60'){throw 'P1273 fixed duration ACK missing'}
    }
    $script:P127Dirty=$true
    try{[void](P12-Query 'p127_mode' "$Mask")}catch{
        $original=$_
        if($script:P1273HostEnabled -and (($Mask -shr 4) -band 7)){
            try{
                $denial=P12-Query 'p1273_floor'
                Add-Content -Encoding UTF8 -LiteralPath "$Stem.$Label.floor-denial.txt" -Value $denial
            }catch{Life-Say 'P1273 floor denial read unavailable; retaining original error'}
        }
        throw $original
    }
    $armed=P127-Status
    if([int]$armed.mode -ne $Mask -or [int]$armed.lease_s -lt ($Seconds+12)){throw 'P127 lease arm refused or too short'}
    P125-PumpOff 2
    $timerMode=if($Offline){3}else{1}
    if($Offline){$script:TraceSeconds=$Seconds}
    $afterTimer=P125-Phase $Label $timerMode $Seconds $Stem $BeforeTimer -CountsOnly -OfflineAggregate:$Offline
    $script:P12Phase="$Label-POST"
    $after=P127-Read
    $psrPost=$null;$psrRestore=$null
    if($null -ne $psrPre){
        $psrPost=P125-Fields (P12-Query 'p1273_psr')
        Add-Content -Encoding UTF8 -LiteralPath "$Stem.$Label.psr.txt" -Value ($psrPost|ConvertTo-Json -Compress)
    }
    [pscustomobject]@{Version=1;Label=$Label;Mask=$Mask;Timer=$afterTimer;Before=$before;After=$after}|Export-Clixml -Depth 12 -LiteralPath "$Stem.$Label.energy-post.clixml"
    if($fixed){
        $f=P125-Fields (P12-Query 'p1273_offline_status')
        $f|Export-Clixml -LiteralPath "$Stem.$Label.offline-status.clixml"
        if($after.Status.mode -ne '0' -or $after.Status.reason -ne '10' -or $f.fixed -ne '0' -or
            $f.capture_reason -ne '10' -or $f.policy_reason -ne '10' -or $f.deadline -ne $afterTimer.Status.end){throw 'P1273 planned offline freeze/rollback not proven'}
    }elseif([int]$after.Status.mode -ne $Mask -or $after.Status.reason -ne '0'){
        if($P12752Idle){P127-Delta $before $after $afterTimer.Status $Label $Stem}
        throw "P127 $Label lease revoked; phase invalid"
    }
    # Stop policy before reading gauge pages. P125 already froze WFI/FG.
    [void](P12-Query 'p127_mode' '0' -Cleanup)
    if($fixed){
        [void](P12-Query 'p1273_offline_s' '0' -Cleanup)
        if((P12-Query 'p1273_offline_s') -ne '0'){throw 'P1273 offline configuration restore missing'}
        $script:P1273OfflineDirty=$false
    }
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
    if($null -ne $psrPre){
        $psrRestore=P125-Fields (P12-Query 'p1273_psr')
        Add-Content -Encoding UTF8 -LiteralPath "$Stem.$Label.psr.txt" -Value ($psrRestore|ConvertTo-Json -Compress)
        if($psrRestore.governor -ne '0' -or $psrRestore.drained -ne '0' -or $psrRestore.owner -eq '2' -or $psrRestore.faults -ne $psrPre.faults){throw 'P1273 PSR restoration/fault check failed'}
    }
    $script:P127Dirty=$false
    if($script:P1272Required){[void](P1272-Caps)}
    $script:P125Report.Add("P127 RESTORE $Label mode=0 $($restored.little_cur) kHz")
    $script:P12Phase="$Label-FG"
    if($P12752Idle){[void](P12752-ReadIdle $Stem $Label $afterTimer.Status)}
    P127-Delta $before $after $afterTimer.Status $Label $Stem
    if($P12752Idle -and -not (Get-Content -Raw -LiteralPath "$Stem.$Label.idle-summary.json"|ConvertFrom-Json).thermal_valid){throw 'P12752 quiet phase incomplete; no automatic next trial'}
    $result=[pscustomobject]@{Timer=$afterTimer;Pre=$before;Post=$after;Restored=$restored;PsrPre=$psrPre;PsrPost=$psrPost;PsrRestore=$psrRestore}
    $result|Export-Clixml -Depth 12 -LiteralPath "$Stem.$Label.energy-complete.clixml"
    return $result
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
        $line="P127 MODULE $name "+$(if($eligible -and $script:P1273HostEnabled -and $name -eq 'PSR'){'TRANSITION_ONLY_PENDING_A1'}elseif($eligible){'APPLIED'}else{'NO_ELIGIBLE_PATH_OR_FALLBACK'})
        $script:P125Report.Add($line);Life-Say $line
        $prior=$a
        $a=P127-Window "P127-$name-A1" 1 $EnergySeconds $Stem $b.Timer
        if($P12752Idle -and $name -eq 'PSR'){
            $matched=$prior.PsrPre.boot_ms -eq '0' -and $a.PsrPre.boot_ms -eq '0' -and $prior.PsrPre.owner -eq '0' -and $a.PsrPre.owner -eq '0' -and $b.PsrPost.governor -eq '1' -and $b.PsrPost.drained -eq '1' -and $b.PsrRestore.owner -eq '0'
            $eligible=$eligible -and $matched
            $script:P125Report.Add("P12752 PSR disabled-vs-governor100 matched=$matched applied=$eligible; screen wake validation still required")
        }elseif($script:P1273HostEnabled -and $name -eq 'PSR'){
            $matched=$null -ne $prior.PsrPre -and $null -ne $prior.PsrPost -and
                $prior.PsrPre.boot_ms -eq '300' -and $a.PsrPre.boot_ms -eq '300' -and
                [uint64]$prior.PsrPost.legacy_ticks -gt [uint64]$prior.PsrPre.legacy_ticks -and
                [uint64]$a.PsrPost.legacy_ticks -gt [uint64]$a.PsrPre.legacy_ticks -and
                $b.PsrPost.governor -eq '1' -and $b.PsrPost.drained -eq '1' -and
                (P125-Sub ([uint64]$b.PsrPost.legacy_ticks) ([uint64]$b.PsrPre.legacy_ticks)) -le 4
            $eligible=$eligible -and $matched
            $script:P125Report.Add("P1273 PSR matched legacy cadence=$matched applied=$eligible; screen wake validation still required")
        }
        $script:P125Report.Add("P127 MODULE $name ABA_COMPLETE applied=$eligible")
        $script:P125Report|Set-Content -Encoding UTF8 -LiteralPath "$Stem.p125-report.txt"
        Life-Say "P127 MODULE $name ABA_COMPLETE applied=$eligible"
    }
}
function P1273-ReadDiagnostics([string]$Stem,[string]$Label){
    $script:P12Phase="P1273-$Label"
    if((P125-Status).armed -ne '0'){throw 'P1273 diagnostics require stopped observation'}
    $floor=P12-Query 'p1273_floor'
    Add-Content -Encoding UTF8 -LiteralPath "$Stem.p1273-diag.txt" -Value "$Label floor $floor"
    for($i=0;$i -lt 8;$i++){
        $line=P12-Query "p1273_bus$i"
        Add-Content -Encoding UTF8 -LiteralPath "$Stem.p1273-diag.txt" -Value "$Label bus$i $line"
    }
    for($cpu=0;$cpu -lt 8;$cpu++){
        $line=P12-Query "p1273_wfi$cpu"
        Add-Content -Encoding UTF8 -LiteralPath "$Stem.p1273-diag.txt" -Value "$Label wfi$cpu $line"
    }
    $script:P125Report.Add("P1273 $Label diagnostic checkpoint; cached bus data may be unsampled/stale, WFI cumulative")
}
function P1273-TraceStatus {
    $s=P125-Fields (P12-Query 'p1273_trace_status')
    if($s.version -ne '1' -or $s.buckets -ne '128' -or $s.owners -ne '16'){throw 'P1273 trace ABI mismatch'}
    return $s
}
function P1273-TraceCapture([string]$Stem){
    $script:P12Phase='THERMAL-TRACE'
    $pre=P1273-TraceStatus
    if($pre.state -ne '0'){throw 'P1273 prior capture exists; preserve frozen data (reboot clears ring)'}
    if((P126-Status).mode -ne '0' -or (P127-Status).mode -ne '0'){throw 'P1273 requires stock driver policy'}
    $script:P1273TraceDirty=$true
    [void](P12-Query 'p1273_trace_mode' '1')
    $s=P1273-TraceStatus
    if($s.state -notin @('2','4') -or [uint64]$s.generation -le [uint64]$pre.generation){throw 'P1273 trace arm missing'}
    $gen=$s.generation
    $until=(Life-Now).AddSeconds(185)
    while($s.state -ne '4' -and (Life-Now) -lt $until){
        P125-PumpOff 5
        $s=P1273-TraceStatus
        if($s.generation -ne $gen){throw 'P1273 generation changed'}
    }
    if($s.state -ne '4'){throw 'P1273 capture did not freeze'}
    $script:P1273TraceDirty=$false
    $s|Export-Clixml -LiteralPath "$Stem.thermal-status.clixml"
    $script:P125Report.Add("P1273 TRACE FROZEN generation=$gen trigger=$($s.trigger) reason=$($s.reason); use ThermalTraceDump; no-trigger capture is inconclusive")
}
function P1273-TraceDump([string]$Stem){
    $script:P12Phase='THERMAL-DUMP'
    $s=P1273-TraceStatus
    if($s.state -ne '4' -or $s.span -eq '0'){throw 'P1273 trace not frozen'}
    $s|Export-Clixml -LiteralPath "$Stem.thermal-status.clixml"
    $gen=$s.generation
    $n=0
    for($cpu=0;$cpu -lt 8;$cpu++){
        $line=P12-Query "p1273_trace_cpu$cpu"
        $c=P125-Fields $line
        if($c.generation -ne $gen -or [int]$c.cpu -ne $cpu){throw 'P1273 coverage identity mismatch'}
        Add-Content -Encoding UTF8 -LiteralPath "$Stem.thermal-pages.txt" -Value $line
        $m=@($c.mask.Split('/')|ForEach-Object {P125-Hex $_})
        for($slot=0;$slot -lt 128;$slot++){
            if(-not ($m[[int][Math]::Floor($slot/64)] -band ([uint64]1 -shl ($slot%64)))){continue}
            $mask=0
            for($p=0;$p -lt 8;$p++){
                if($p -gt 0 -and -not ($mask -band (3 -shl ($p*2)))){continue}
                $sel=$cpu*1024+$slot*8+$p
                [void](P12-Query 'p1273_trace_select' "$sel")
                $line=P12-Query 'p1273_trace_page'
                $v=P125-Fields $line
                if($v.generation -ne $gen -or [int]$v.select -ne $sel -or $v.present -ne '1' -or ([uint64]$v.bin%128) -ne $slot){throw 'P1273 page identity mismatch'}
                if($p -eq 0){$mask=P125-Hex $v.mask;$bin=$v.bin}
                if((P125-Hex $v.mask) -ne $mask -or $v.bin -ne $bin -or $mask -gt 65535){throw 'P1273 bucket changed'}
                $expected=0
                foreach($i in @(0,1)){if($mask -band (1 -shl ($p*2+$i))){$expected++}}
                if(([regex]::Matches($line,' r=')).Count -ne $expected){throw 'P1273 owner count mismatch'}
                Add-Content -Encoding UTF8 -LiteralPath "$Stem.thermal-pages.txt" -Value $line
                $n++
            }
        }
    }
    $post=P1273-TraceStatus
    foreach($key in @('state','generation','start','end','trigger','span','reason')){if($s[$key] -ne $post[$key]){throw 'P1273 final trace identity mismatch'}}
    $script:P125Report.Add("P1273 TRACE DUMP COMPLETE generation=$gen pages=$n; dropped/clipped coverage must be reviewed")
}
function P1273-GpuDump([string]$Stem){
    $script:P12Phase='THERMAL-GPU-DUMP'
    $s=P1273-TraceStatus
    if($s.state -ne '4'){throw 'P1273 trace not frozen'}
    $s|Export-Clixml -LiteralPath "$Stem.thermal-status.clixml"
    $line=P12-Query 'p1273_gpu_status'
    $g=P125-Fields $line
    if($g.generation -ne $s.generation -or [uint64]$g.completed -gt [uint64]$g.submitted){throw 'P1273 GPU coverage mismatch'}
    Add-Content -Encoding UTF8 -LiteralPath "$Stem.thermal-gpu.txt" -Value $line
    $n=[uint64]$g.completed
    $first=[Math]::Max(1,[double]$n-1023)
    if([uint64]$g.first_wake){
        [void](P12-Query 'p1273_gpu_select' '1024')
        $line=P12-Query 'p1273_gpu_page'
        $v=P125-Fields $line
        if($v.slot -ne '1024' -or $v.seq -ne $g.first_wake -or $v.generation -ne $s.generation -or $v.wake_ok -ne '1'){throw 'P1273 first wake mismatch'}
        Add-Content -Encoding UTF8 -LiteralPath "$Stem.thermal-gpu.txt" -Value $line
    }
    for($i=[uint64]$first;$i -le $n;$i++){
        $slot=($i-1)%1024
        [void](P12-Query 'p1273_gpu_select' "$slot")
        $line=P12-Query 'p1273_gpu_page'
        $v=P125-Fields $line
        if([uint64]$v.slot -ne $slot -or [uint64]$v.seq -ne $i -or $v.generation -ne $s.generation -or
            [uint64]$v.queued -gt [uint64]$v.began -or [uint64]$v.began -gt [uint64]$v.ended){throw 'P1273 GPU row identity/timing mismatch'}
        Add-Content -Encoding UTF8 -LiteralPath "$Stem.thermal-gpu.txt" -Value $line
    }
    for($slot=0;$slot -lt 128;$slot++){
        [void](P12-Query 'p1273_gov_select' "$slot")
        $line=P12-Query 'p1273_gov_page'
        $v=P125-Fields $line
        if([int]$v.slot -ne $slot){throw 'P1273 governor slot mismatch'}
        if($v.present -eq '1' -and (([uint64]$v.bin%128) -ne $slot -or [uint64]$v.at -lt [uint64]$s.start -or [uint64]$v.at -ge [uint64]$s.end)){throw 'P1273 governor row outside capture'}
        Add-Content -Encoding UTF8 -LiteralPath "$Stem.thermal-gov.txt" -Value $line
    }
    $post=P1273-TraceStatus
    foreach($key in @('state','generation','start','end','trigger','span','reason')){if($s[$key] -ne $post[$key]){throw 'P1273 GPU dump trace changed'}}
    $script:P125Report.Add("P1273 GPU/GOV DUMP COMPLETE submitted=$($g.submitted) completed=$n overwritten=$($g.overwritten); incomplete jobs and overwritten rows are missing evidence")
}
function P1273-CapWindow([string]$Stem,[string]$Label,[int]$Mode){
    $script:P12Phase=$Label
    $before=P125-Fields (P12-Query 'p1273_cap_status')
    if($before.state -in @('2','3')){throw 'P1273 prior cap active'}
    P125-PumpOff 5
    $script:P1273CapDirty=$true
    [void](P12-Query 'p1273_cap_mode' "$Mode")
    $until=(Life-Now).AddSeconds(80)
    $s=P125-Fields (P12-Query 'p1273_cap_status')
    while($s.state -ne '4' -and (Life-Now) -lt $until){
        if([uint64]$s.generation -le [uint64]$before.generation -or [int]$s.mode -ne $Mode){throw 'P1273 cap generation/mode mismatch'}
        P125-PumpOff 2
        $s=P125-Fields (P12-Query 'p1273_cap_status')
    }
    $s|Export-Clixml -LiteralPath "$Stem.$Label.cap.clixml"
    if($s.state -ne '4' -or [uint64]$s.generation -le [uint64]$before.generation -or [int]$s.mode -ne $Mode){throw 'P1273 cap did not terminate'}
    [void](P12-Query 'p1273_cap_mode' '0' -Cleanup)
    if((P12-Query 'p1273_cap_mode') -ne '0'){throw 'P1273 cap restore not proven'}
    $script:P1273CapDirty=$false
    if($s.reason -eq '10'){throw 'P1273 no naturally sustained load: INCONCLUSIVE; do not induce overheating'}
    if($s.reason -ne '1' -or $s.start -eq '0' -or $s.end -ne $s.until -or $s.windows -eq '0'){throw "P1273 cap phase revoked reason=$($s.reason)"}
    $script:P125Report.Add("P1273 CAP $Label natural trigger, 15s bounded; rate asked flags=$($s.big_asked)/$($s.lit_asked); workload matching and energy not established")
    return $s
}
function P1273-CapABA([string]$Stem){
    if(-not $AllowOffCap){throw 'OffBurstCapABA requires -AllowOffCap after reviewing frozen ThermalTrace'}
    $trace=P1273-TraceStatus
    if($trace.state -ne '4' -or $trace.trigger -eq '0'){throw 'P1273 attributed burst capture required'}
    $mode=if($AllowStrongerCap){2}else{1}
    if($mode -eq 2 -and (P125-Fields (P12-Query 'p1273_cap_status')).mild_done -ne '1'){throw 'P1273 stronger cap requires completed mild trial and explicit review'}
    $a=P1273-CapWindow $Stem 'CAP-A0' 3
    $b=P1273-CapWindow $Stem 'CAP-B' $mode
    $c=P1273-CapWindow $Stem 'CAP-A1' 3
    $script:P125Report.Add("P1273 CAP ABA recorded; peaks BIG=$($a.big_peak)/$($b.big_peak)/$($c.big_peak), LITTLE=$($a.lit_peak)/$($b.lit_peak)/$($c.lit_peak); not a matched-workload energy result")
}
function P1273-OfflineSamples([string]$Stem,[string]$Label){
    $path="$Stem.$Label.fg.csv"
    if(-not (Test-Path -LiteralPath $path)){throw "P1273 $Label no fuel-gauge samples"}
    $all=@(Import-Csv -LiteralPath $path)
    if(@($all|Where-Object {$_.in_window -eq 'True' -and ([int]$_.external -ne 0 -or [int]$_.current_ua -ge 0 -or [int]$_.voltage_uv -lt 3000000 -or [int]$_.voltage_uv -gt 4500000)}).Count){throw "P1273 $Label charging/non-discharge/invalid voltage inside window"}
    # Trim link transition edges; integrate only observed support.
    $v=@($all|Where-Object {$_.in_window -eq 'True' -and [double]$_.seconds_from_start -ge 5 -and [double]$_.seconds_from_start -le 55}|Sort-Object {[double]$_.seconds_from_start})
    if($v.Count -lt 4){throw "P1273 $Label insufficient independent samples"}
    for($i=1;$i -lt $v.Count;$i++){
        $dt=[double]$v[$i].seconds_from_start-[double]$v[$i-1].seconds_from_start
        if($dt -le 0 -or $dt -gt 15 -or [uint64]$v[$i].seq -ne ([uint64]$v[$i-1].seq+1)){throw "P1273 $Label sample gap"}
    }
    return $v
}
function P1273-OfflineIntegrate($Rows,[string]$Label,[double]$Lo,[double]$Hi){
    if($Hi-$Lo -lt 30){throw 'P1273 insufficient common integration support'}
    $area=0.0;$duration=0.0
    for($i=1;$i -lt $Rows.Count;$i++){
        $x=[double]$Rows[$i-1].seconds_from_start;$y=[double]$Rows[$i].seconds_from_start
        $a=[Math]::Max($x,$Lo);$b=[Math]::Min($y,$Hi)
        if($b -le $a){continue}
        $p=-[double]$Rows[$i-1].net_battery_w;$q=-[double]$Rows[$i].net_battery_w
        $pa=$p+($q-$p)*($a-$x)/($y-$x);$pb=$p+($q-$p)*($b-$x)/($y-$x)
        $area+=($pa+$pb)*($b-$a)/2;$duration+=$b-$a
    }
    if([Math]::Abs($duration-($Hi-$Lo)) -gt 0.001){throw 'P1273 extrapolation/coverage gap'}
    return [pscustomobject]@{Label=$Label;Samples=$Rows.Count;Start=$Lo;End=$Hi;Seconds=$duration;MeanW=$area/$duration;ObservedWh=$area/3600}
}
function P1273-OfflineABA([string]$Stem){
    if(-not $AllowOfflineTrial){throw 'OfflineABA requires -AllowOfflineTrial after individual functional review'}
    if((P126-Status).mode -ne '0' -or (P127-Status).mode -ne '0' -or (P12-Query 'p1273_offline_s') -ne '0'){throw 'P1273 prior lease or offline configuration active'}
    $defs=@{ABOX=5;BT=257;WIFI=129;TICKER=513;LITTLE949=17;COMBINED5=903;COMBINED6=919}
    if($OfflineModule -in 'COMBINED5','COMBINED6'){P1273-CombinedPermission $defs[$OfflineModule]}
    $nonce=P12-Query 'p1273_boot_identity'
    $base=P125-ReadSnapshot 'P1273-OFFLINE-BASE' -CountsOnly
    $a=P127-Window 'OFFLINE-A0' 1 60 $Stem $base -Offline
    if((P12-Query 'p1273_boot_identity') -ne $nonce){throw 'P1273 boot identity changed after A0'}
    $b=P127-Window 'OFFLINE-B' $defs[$OfflineModule] 60 $Stem $a.Timer -Offline
    if((P12-Query 'p1273_boot_identity') -ne $nonce){throw 'P1273 boot identity changed after B'}
    $c=P127-Window 'OFFLINE-A1' 1 60 $Stem $b.Timer -Offline
    if((P12-Query 'p1273_boot_identity') -ne $nonce){throw 'P1273 boot identity changed after A1'}
    if($a.Timer.Status.anchor -ne $b.Timer.Status.anchor -or $a.Timer.Status.anchor -ne $c.Timer.Status.anchor -or
       $a.Timer.Status.hz -ne $b.Timer.Status.hz -or $a.Timer.Status.hz -ne $c.Timer.Status.hz -or
       [uint64]$b.Timer.Status.start -le [uint64]$a.Timer.Status.end -or [uint64]$c.Timer.Status.start -le [uint64]$b.Timer.Status.end){throw 'P1273 offline capture continuity failed'}
    $labels=@('OFFLINE-A0','OFFLINE-B','OFFLINE-A1')
    $samples=@{};$lo=5.0;$hi=55.0
    foreach($label in $labels){
        $v=@(P1273-OfflineSamples $Stem $label)
        $samples[$label]=$v
        $lo=[Math]::Max($lo,[double]$v[0].seconds_from_start)
        $hi=[Math]::Min($hi,[double]$v[-1].seconds_from_start)
    }
    $energy=@(foreach($label in $labels){P1273-OfflineIntegrate $samples[$label] $label $lo $hi})
    if($OfflineModule -in 'COMBINED5','COMBINED6'){
        P1273-CombinedPermission $defs[$OfflineModule]
        P1273-CombinedApplication $b $defs[$OfflineModule] -Offline
    }
    $energy|Export-Csv -NoTypeInformation -Encoding UTF8 -LiteralPath "$Stem.offline-energy.csv"
    $reference=($energy[0].MeanW+$energy[2].MeanW)/2
    $script:P125Report.Add("P1273 OFFLINE ABA fixed 60s each, module=$OfflineModule common support=$lo..$hi s A0=$($energy[0].MeanW)W B=$($energy[1].MeanW)W A1=$($energy[2].MeanW)W Amean=$reference W; observed NET battery discharge only; USB quiet N/A offline, no causal claim without workload/temperature match")
}
# Explicit gate: user performs external real traffic while OFF, then tests
# foreground functions after kernel has restored mode0. Never ACK on time alone.
function P1273-GateStatus {
    $v=P125-Fields (P12-Query 'p1273_gate_status')
    if($v.version -ne '1' -or [int]$v.qualified -gt $(if($P12754){7423}else{255})){throw 'P1273 gate ABI mismatch'}
    return $v
}
function P1273-GateBit([int]$Mode){
    switch($Mode){3{1} 5{2} 257{4} 129{8} 513{16} 17{32} 33{1024} 49{2048} 65{4096} 903{64} 919{128} default{throw 'P1273 invalid gate mask'}}
}
function P1273-CombinedPermission([int]$Mode,[switch]$Qualifying){
    $script:P1273CombinedAllowed=$false
    if(-not $AllowCombined){throw 'P1273 combined requires -AllowCombined and same-boot gate'}
    $g=P1273-GateStatus
    $need=if($Mode -eq 903){31}elseif($Mode -eq 919){127}else{throw 'P1273 exact combined masks only'}
    if(-not $Qualifying){$need=$need -bor (P1273-GateBit $Mode)}
    if($g.phase -ne '0' -or (([int]$g.qualified -band $need) -ne $need)){throw "P1273 gate prerequisite missing need=$need actual=$($g.qualified)"}
    $script:P1273CombinedAllowed=$true
}
function P1273-GateCheckpoint([string]$Stem,[string]$Phase){
    $script:P12Phase=$Phase
    $g=P1273-GateStatus
    $row=[pscustomobject]@{Phase=$Phase;Gate=$g;Policy=(P127-Status);Metrics=(P125-Fields (P12-Query 'p127_metrics'));BT=(P125-Fields (P12-Query 'p127_bt'));WiFi=(P125-Fields (P12-Query 'p127_wifi'));Ticker=(P125-Fields (P12-Query 'p127_ticker'));Active=(P12-Query 'p1273_gate_active');Post=(P12-Query 'p1273_gate_post')}
    $row|Export-Clixml -Depth 8 -LiteralPath "$Stem.$Phase.gate.clixml"
    return $row
}
function P1273-GateForeground([string]$Module){
    # No blocking Read-Host: maintain thermal watchdog while the user tests.
    Life-Say "P1273 ${Module}: policy OFF. Wake screen and verify normal function now. ABOX: play audible audio and record/play mic; BT: real paired data/reconnect; WIFI: bidirectional LAN transfer/reconnect; ticker: network timing/cancel; floor: input response and normal UI; USB: serial after link cycle. For combined test ALL applicable functions. Press Y on PC only after successful checks, N to reject."
    $until=(Life-Now).AddSeconds(100)
    while((Life-Now) -lt $until){
        Life-Pump;Life-CheckSafety (Life-Now);P12-EarlyThermalGuard
        try{
            if([Console]::KeyAvailable){
                $key=[Console]::ReadKey($true).Key
                if($key -eq [ConsoleKey]::N){throw 'P1273 operator rejected functional test'}
                if($key -eq [ConsoleKey]::Y){
                    if($script:LifeState -ne 'ON'){throw 'P1273 foreground wake not observed'}
                    return
                }
            }
        }catch{throw "P1273 interactive confirmation unavailable or rejected: $($_.Exception.Message)"}
    }
    throw 'P1273 foreground confirmation timed out; no qualification'
}
function P1273-FunctionalGate([string]$Stem){
    if(-not $AllowFunctionalGate){throw 'FunctionalGate requires -AllowFunctionalGate; real external workload and foreground confirmation'}
    $defs=@{USB=3;ABOX=5;BT=257;WIFI=129;TICKER=513;LITTLE949=17;LITTLE832=33;LITTLE715=49;LITTLE598=65;COMBINED5=903;COMBINED6=919}
    if($GateModule -in @('LITTLE832','LITTLE715','LITTLE598')){P12754-PriorFloor $GateModule}
    $mask=$defs[$GateModule];$bit=P1273-GateBit $mask
    if($mask -in 903,919){P1273-CombinedPermission $mask -Qualifying}
    $script:P1273GateAllowed=$true
    $pre=P1273-GateCheckpoint $Stem 'GATE-PRE'
    if($pre.Gate.phase -ne '0' -or $pre.Policy.mode -ne '0'){throw 'P1273 pending gate or prior policy; cancel/read status first'}
    $script:P1273GateDirty=$true
    [void](P12-Query 'p1273_gate_begin' "$mask")
    $g=P1273-GateStatus
    if($g.phase -ne '1' -or [int]$g.mode -ne $mask -or [int]$g.generation -le [int]$pre.Gate.generation){throw 'P1273 gate prepare not verified'}
    $generation=$g.generation
    $script:P127Dirty=$true
    [void](P12-Query 'p127_mode' "$mask")
    $g=P1273-GateStatus
    if($g.phase -ne '2' -or $g.generation -ne $generation -or $g.lease -eq '0' -or [int](P127-Status).mode -ne $mask){throw 'P1273 gate arm identity mismatch'}
    Life-Say "P1273 $GateModule active; keep screen OFF, no touch. Generate genuine background traffic from another device for BT/WIFI/ticker; do NOT intentionally heat device."
    if($mask -eq 3){
        P125-PumpOff 8
        P125-Offline 5
        P125-PumpOff 12
    }else{P125-PumpOff 45}
    $active=P1273-GateCheckpoint $Stem 'GATE-ACTIVE'
    if($active.Gate.generation -ne $generation -or $active.Gate.phase -ne '2' -or $active.Policy.mode -ne "$mask" -or $active.Policy.reason -ne '0'){throw 'P1273 functional policy revoked'}
    if($mask -in @(17,33,49,65)){
        Life-Say "P1273 ${GateModule}: wake screen NOW to prove kernel revokes the floor lease; verify normal storage and UI during foreground review"
        $until=(Life-Now).AddSeconds(20)
        while($script:LifeState -ne 'ON' -and (Life-Now) -lt $until){
            Life-Pump;Life-CheckSafety (Life-Now);P12-EarlyThermalGuard
        }
        if($script:LifeState -ne 'ON'){throw 'P1273 floor wake not observed'}
        $expectedReason='4'
    }else{
        [void](P12-Query 'p127_mode' '0' -Cleanup)
        P125-PumpOff 3
        $expectedReason='0'
    }
    $stopped=P1273-GateCheckpoint $Stem 'GATE-RESTORED'
    if($stopped.Policy.mode -ne '0' -or $stopped.Policy.reason -ne $expectedReason){throw 'P1273 normal rollback not proven'}
    $script:P127Dirty=$false
    [void](P12-Query 'p1273_gate_review' $generation)
    if((P1273-GateStatus).phase -ne '3'){throw 'P1273 review not armed'}
    P1273-GateForeground $GateModule
    $post=P1273-GateCheckpoint $Stem 'GATE-FOREGROUND'
    if($post.Gate.phase -ne '3' -or $post.Gate.generation -ne $generation -or $post.Gate.missing -ne '0'){
        throw "P1273 no qualification: missing evidence bitmap=$($post.Gate.missing), report retained"
    }
    [void](P12-Query 'p1273_gate_ack' $generation)
    $done=P1273-GateCheckpoint $Stem 'GATE-DONE'
    if($done.Gate.phase -ne '0' -or (([int]$done.Gate.qualified -band $bit) -ne $bit)){throw 'P1273 qualification ACK missing'}
    $script:P1273GateDirty=$false
    $script:P125Report.Add("P1273 GATE $GateModule qualified for this boot; generation=$generation qualified=$($done.Gate.qualified); operator-confirmed foreground and driver activity, not energy proof")
}
function P1273-CombinedApplication($B,[int]$Mode,[switch]$Offline){
    $flags=[ordered]@{
        USB=if($Offline){'N/A_OFFLINE'}elseif([uint64]$B.Post.Metrics.usb_8ms -gt [uint64]$B.Pre.Metrics.usb_8ms){'APPLIED'}else{'NO_EVIDENCE'}
        ABOX=if([uint64]$B.Post.Metrics.abox_stop -gt [uint64]$B.Pre.Metrics.abox_stop -and [uint64]$B.Post.Metrics.abox_idle_wait -gt [uint64]$B.Pre.Metrics.abox_idle_wait){'APPLIED'}else{'NO_EVIDENCE'}
        BT=if([uint64]$B.Post.BT.attach -gt [uint64]$B.Pre.BT.attach -and $B.Post.BT.fallback -eq $B.Pre.BT.fallback){'APPLIED'}else{'NO_EVIDENCE'}
        WIFI=if([uint64]$B.Post.Wifi.attach -gt [uint64]$B.Pre.Wifi.attach -and $B.Post.Wifi.fallback -eq $B.Pre.Wifi.fallback){'APPLIED'}else{'NO_EVIDENCE'}
        TICKER=if([uint64]$B.Post.Ticker.wait -gt [uint64]$B.Pre.Ticker.wait){'APPLIED'}else{'NO_EVIDENCE'}
    }
    if($Mode -eq 919){
        $flags['LITTLE949']=if([uint64]$B.Post.Status.floor_hits -gt [uint64]$B.Pre.Status.floor_hits){'REQUESTED_VERIFY_MEASURED_MHZ'}else{'NO_EVIDENCE'}
    }
    $line=($flags.GetEnumerator()|ForEach-Object {"$($_.Key)=$($_.Value)"}) -join ' '
    $script:P125Report.Add("P1273 COMBINED APPLICATION $line")
    Life-Say "P1273 COMBINED APPLICATION $line"
    if('NO_EVIDENCE' -in $flags.Values){$script:P125Report.Add('P1273 combined effect incomplete; do not attribute energy difference to full profile')}
}
function P1273-CombinedConnected([string]$Stem){
    $mask=if($CombinedProfile -eq 'COMBINED5'){903}else{919}
    P1273-CombinedPermission $mask
    if($EnergySeconds -gt 30){throw 'P1273 first combined connected A/B/A limited to <=30s'}
    $base=P125-ReadSnapshot 'P1273-COMBINED-BASE' -CountsOnly
    $a=P127-Window 'COMBINED-A0' 1 $EnergySeconds $Stem $base
    $b=P127-Window 'COMBINED-B' $mask $EnergySeconds $Stem $a.Timer
    $c=P127-Window 'COMBINED-A1' 1 $EnergySeconds $Stem $b.Timer
    P1273-CombinedPermission $mask
    P1273-CombinedApplication $b $mask
    $script:P125Report.Add("P1273 combined $mask A/B/A restored; actual module application must be verified from B counters, connected battery not energy evidence")
}
function P12752-CheckOptions{
    if(-not $P12752Idle){if($Scenario -in @('Idle52','Idle52Offline','Idle52Recover')){throw 'Idle52 needs -P12752Idle'};return}
    if($Scenario -eq 'Idle52Recover' -and -not $ExpectedStart){throw 'Idle52Recover requires -ExpectedStart from original P125 REJOIN'}
    if($Scenario -notin @('Idle52','Idle52Offline','Idle52Recover','BusTrace','EnergyTrials','EnergyOffline','DriverTrials')){throw 'P12752Idle supports only independent idle scenarios'}
    if($Scenario -eq 'EnergyTrials'){
        $m=$EnergyModules.Trim().ToUpperInvariant()
        if($m -notin @('PSR','USB','ABOX','BT','WIFI','TICKER','LITTLE949','LITTLE832','LITTLE715','LITTLE598') -or $EnergySeconds -gt 60){throw 'P12752 requires one idle module, <=60s'}
        $prior=@{LITTLE832='LITTLE949';LITTLE715='LITTLE832';LITTLE598='LITTLE715'}
        if($prior.ContainsKey($m)){
            if(-not $AllowLowerFloor -or -not $P12752PriorReport){throw 'P12752 deeper floor needs prior reviewed A/B/A report and -AllowLowerFloor'}
            $report=Get-Content -Raw -LiteralPath $P12752PriorReport
            if($report -notmatch ('(?m)^P127 MODULE '+$prior[$m]+' ABA_COMPLETE applied=True\s*$') -or $report -notmatch '(?m)^STOP=COMPLETE\s*$'){throw 'Prior floor application/restoration report missing'}
        }
    }
    if($Scenario -eq 'DriverTrials' -and $DriverModules.Trim().ToUpperInvariant() -notin @('HAPTIC','WIFI','GNSS','TOUCH','LEEWAY','SHUB')){throw 'P12752 requires one P126 driver module'}
    if($Scenario -in @('EnergyOffline','Idle52Offline') -and ($EnergySeconds -ne 60 -or -not $AllowOfflineTrial)){throw 'P12752 offline requires explicit -AllowOfflineTrial and 60s'}
    if($Scenario -eq 'Idle52' -and $EnergySeconds -gt 90){throw 'P12752 baseline <=90s'}
}
function P12753-Power($S,$Rows,$FG,[int]$Seconds){
    $hz=[double]$S.hz
    $span=if($hz -gt 0){([double]$S.end-[double]$S.start)/$hz}else{0}
    $ok=$hz -gt 0 -and $S.state -eq '3' -and $S.reason -eq '10' -and $S.mask -eq '0' -and $S.drops -eq '0' -and [uint64]$S.start -gt 0 -and $S.end -eq $S.until -and [uint64]$S.down -gt [uint64]$S.arm -and [uint64]$S.up -ge [uint64]$S.end -and ([double]$S.start-[double]$S.down) -ge 20*$hz -and [Math]::Abs($span-$Seconds) -lt 0.01
    $t=@($Rows|Where-Object {$_.kind -eq '1'}|Sort-Object {[uint64]$_.at})
    $driver=if([int]$S.policy){15}else{0}
    if($t.Count -lt 2){$ok=$false}
    else{
        if(([double]$t[-1].at-[double]$t[0].at) -lt 0.8*$span*$hz){$ok=$false}
        foreach($r in $t){
            if($r.valid -ne '1' -or $r.mask -ne '0' -or $r.policy -ne $S.policy -or [int]$r.driver -ne $driver -or [uint64]$r.g3d -eq 4294967295 -or ([uint64]$r.g3d -band 15) -ne 0 -or [uint64]$r.at -lt [uint64]$S.start -or [uint64]$r.at -gt [uint64]$S.end){$ok=$false}
        }
        for($i=1;$i -lt $t.Count;$i++){if(([double]$t[$i].at-[double]$t[$i-1].at) -gt 4*$hz){$ok=$false}}
    }
    $w=@($FG|Where-Object {[uint64]$_.ticks -ge [uint64]$S.start -and [uint64]$_.ticks -le [uint64]$S.end}|Sort-Object {[uint64]$_.ticks})
    $joules=0.0;$covered=0.0
    foreach($v in $w){if([int]$v.external -ne 0 -or [int]$v.current_ua -ge 0 -or [uint64]$v.voltage_uv -lt 2500000 -or [uint64]$v.voltage_uv -gt 5000000){$ok=$false}}
    for($i=1;$i -lt $w.Count;$i++){
        $dt=if($hz -gt 0){([double]$w[$i].ticks-[double]$w[$i-1].ticks)/$hz}else{0}
        if($dt -le 0 -or $dt -gt 15 -or ([uint64]$w[$i].seq-[uint64]$w[$i-1].seq) -ne 1){$ok=$false;continue}
        $covered+=$dt
        $joules-=([double]$w[$i].current_ua*[double]$w[$i].voltage_uv+[double]$w[$i-1].current_ua*[double]$w[$i-1].voltage_uv)*$dt/2e12
    }
    if($w.Count -lt 2 -or $covered -lt 0.8*$span -or $covered -le 0){$ok=$false}
    return [pscustomobject]@{version=1;power_verified=[bool]$ok;window_s=$span;covered_s=$covered;samples=$w.Count;mean_discharge_w=if($ok){$joules/$covered}else{$null};discharge_wh=if($ok){$joules/3600}else{$null};start_big_mc=if($t.Count){[int]$t[0].big}else{0};start_lit_mc=if($t.Count){[int]$t[0].lit}else{0};mean_voltage_uv=if($w.Count){($w|Measure-Object voltage_uv -Average).Average}else{0}}
}
function P12753-Diff($A,$B,[string]$Key){return P125-Sub ([uint64]$B[$Key]) ([uint64]$A[$Key])}
# Only cursor writes are idempotent. Never retry a policy or arm write.
function P12754-FrozenIdentity($S,$CP){
    if((P12-Query 'p1273_boot_identity') -ne $CP.Nonce){throw 'P12754 boot identity changed'}
    $v=P125-Fields (P12-Query 'p12753_status')
    foreach($k in @('state','request','arm','start','end','count','reason','mask')){
        if($v[$k] -ne $S[$k]){throw 'P12754 frozen phase changed; no retry'}
    }
}
function P12754-FrozenRow($S,$CP,[int]$Index){
    for($attempt=0;$attempt -lt 3;$attempt++){
        try{
            [void](P12-Query 'p12753_select' "$Index")
        }catch{
            if($_.Exception.Message -notmatch 'Missing echo and sysctl readback'){throw}
            P12754-FrozenIdentity $S $CP
            if((P12-Query 'p12753_select') -ne "$Index"){
                if($attempt -eq 2){throw 'P12754 frozen cursor not confirmed'}
                continue
            }
        }
        $r=P125-Fields (P12-Query 'p12753_page')
        if($r.select -ne "$Index" -or $r.phase -ne $S.start){throw 'P12754 row identity changed'}
        return $r
    }
    throw 'P12754 cursor retries exhausted'
}
function P12754-PriorFloor([string]$Module){
    $prior=@{LITTLE832='LITTLE949';LITTLE715='LITTLE832';LITTLE598='LITTLE715'}
    if(-not $prior.ContainsKey($Module)){return}
    if(-not $P12754 -or -not $AllowLowerFloor -or $Audit53Reports.Count -ne 3){throw 'P12754 lower floor requires -P12754 -AllowLowerFloor and prior same-boot A0/B/A1 JSON paths in -Audit53Reports'}
    $r=@($Audit53Reports|ForEach-Object {Get-Content -Raw -LiteralPath $_|ConvertFrom-Json})
    [void](P12753-Compare $r[0] $r[1] $r[2])
    if($r[1].module -ne $prior[$Module] -or $r[1].boot -ne (P12-Query 'p1273_boot_identity')){throw 'P12754 prior floor or boot mismatch'}
    if(-not $r[1].PSObject.Properties['rail_verified'] -or -not $r[1].rail_verified){throw 'P12754 prior floor lacks BUCK3 evidence'}
}
function P12754-Require {
    if((P12-Query 'p12754_revision') -ne '1'){throw 'P12754 ABI mismatch'}
    $w=P125-Fields (P12-Query 'p12754_wait')
    if($w.enabled -ne '1'){throw 'P12754 boot opt-in missing'}
}
function P12754-ExportHardware([string]$Stem,$Rows){
    $names=@('MIF','INT','CPUCL0','CPUCL1','G3D','INTCAM','FSYS0','CAM','DISP','AUD','IVA','SCORE','CP')
    $out=@(foreach($r in $Rows){
        if($r.kind -ne '2'){continue}
        $i=[int]$r.index
        if($i -lt 13){
            [pscustomobject][ordered]@{at=$r.at;source='ACPM_GET';name=$names[$i];valid=$r.valid;errno=$r.errno;word0=$r.bkhz;word1=$r.raw;word2=$r.lkhz;word3=$r.bmeas;rate_khz=if($r.valid -eq '1'){$r.raw}else{$null};pll_khz=$null;mux=$null;divider=$null;busd=$null;switch_mux=$null;gate=$null;ctrl=$null;vsel=$null;ctrl_errno=$null;vsel_errno=$null;configured_uv=$null;cache_age_ms=$null;aux0=$null;aux1=$null;aux2=$null;aux3=$null;aux4=$null}
        }elseif($i -ge 100 -and $i -le 105){
            $isRail=$i -ge 103;$isMif=$i -eq 100
            [pscustomobject][ordered]@{at=$r.at;source=if($isRail){'S2MPS18_CONFIG'}else{'CMU_REGISTER'};name=@('MIF_PLL','MFC_BUS','ISPPRE_BUS','BUCK2_BIG','BUCK3_LITTLE','BUCK6_G3D')[$i-100];valid=$r.valid;errno=$r.errno;word0=if(-not $isRail){$r.raw}else{$null};word1=$null;word2=$null;word3=$null;rate_khz=$null;pll_khz=if($isMif -and $r.valid -eq '1'){$r.refresh}else{$null};mux=if($isMif){$r.bkhz}elseif(-not $isRail){$r.raw}else{$null};divider=if($isMif){$r.lkhz}elseif(-not $isRail){$r.bkhz}else{$null};busd=if($isMif){$r.bmeas}else{$null};switch_mux=if($isMif){$r.lmeas}else{$null};gate=if(-not $isRail -and -not $isMif){$r.lkhz}else{$null};ctrl=if($isRail){$r.raw}else{$null};vsel=if($isRail){$r.bkhz}else{$null};ctrl_errno=if($isRail){$r.lkhz}else{$null};vsel_errno=if($isRail){$r.bmeas}else{$null};configured_uv=if($isRail -and $r.opp_valid -eq '1'){$r.opp_uv}else{$null};cache_age_ms=$null;aux0=$null;aux1=$null;aux2=$null;aux3=$null;aux4=$null}
        }elseif($i -ge 106 -and $i -le 109){
            [pscustomobject][ordered]@{at=$r.at;source='DRIVER_CACHE';name=@('AUD_CLOCK','ABOX_DMA','MFC_OWNER','ISPPRE_OWNER')[$i-106];valid=$r.valid;errno=$r.errno;word0=$null;word1=$null;word2=$null;word3=$null;rate_khz=$null;pll_khz=$null;mux=$null;divider=$null;busd=$null;switch_mux=$null;gate=$null;ctrl=$null;vsel=$null;ctrl_errno=$null;vsel_errno=$null;configured_uv=$null;cache_age_ms=if($i -lt 108){$r.age}else{$null};aux0=$r.raw;aux1=$r.bkhz;aux2=$r.lkhz;aux3=$r.bmeas;aux4=$r.lmeas}
        }
    })
    if($out.Count){$out|Export-Csv -NoTypeInformation -Encoding UTF8 -LiteralPath "$Stem.hardware54.csv"}
}
function P12753-ReadAudit([string]$Stem,$CP){
    if($CP.Module -eq 'HARDWARE54' -and -not $P12754){throw 'P12754 recovery requires -P12754'}
    if((P12-Query 'p1273_boot_identity') -ne $CP.Nonce){throw 'P12753 recovery boot identity changed'}
    $s=P125-Fields (P12-Query 'p12753_status')
    if($s.state -ne '3' -or $s.request -ne [string]$CP.Request -or $s.arm -ne $CP.Arm -or [int]$s.count -gt 96){throw 'P12753 frozen identity missing; no new arm'}
    $s|ConvertTo-Json|Set-Content -Encoding UTF8 -LiteralPath "$Stem.audit53-status.json"
    $out="$Stem.audit53-power.json"
    $note=if([uint64]$s.start -eq 0){'Aborted before measurement; no samples to recover'}else{'Frozen readback incomplete'}
    [pscustomobject]@{version=1;power_verified=$false;module=$CP.Module;reason=$s.reason;stop_mask=$s.mask;note=$note}|ConvertTo-Json|Set-Content -Encoding UTF8 -LiteralPath $out
    if($P12754){P12754-Require;P12-Query 'p12754_wait'|Set-Content -Encoding UTF8 -LiteralPath "$Stem.wait54.txt"}
    if([uint64]$s.start -eq 0){throw "P12753 never entered measurement: reason=$($s.reason) mask=$($s.mask); no samples to recover"}
    $rows=@()
    for($i=0;$i -lt [int]$s.count;$i++){
        if($P12754){$r=P12754-FrozenRow $s $CP $i}else{
            [void](P12-Query 'p12753_select' "$i")
            $r=P125-Fields (P12-Query 'p12753_page')
        }
        if($r.select -ne [string]$i -or $r.phase -ne $s.start){throw 'P12753 row identity changed'}
        $rows+=[pscustomobject]$r
        $rows|Export-Csv -NoTypeInformation -Encoding UTF8 -LiteralPath "$Stem.audit53-rows.csv"
    }
    $again=P125-Fields (P12-Query 'p12753_status')
    foreach($k in @('state','request','arm','start','end','count','reason','mask')){if($again[$k] -ne $s[$k]){throw 'P12753 frozen phase changed'}}
    if([uint64]$s.start -eq 0 -or [uint64]$s.end -le [uint64]$s.start){throw "P12753 never entered measurement: reason=$($s.reason) mask=$($s.mask)"}
    if($P12754){P12754-ExportHardware $Stem $rows}
    if($CP.Module -eq 'HARDWARE54'){
        P12754-FrozenIdentity $s $CP
        if($s.reason -ne '10' -or $s.mask -ne '0' -or $s.drops -ne '0'){throw 'P12754 hardware audit interrupted; partial evidence retained'}
        $samples=@($rows|Where-Object {$_.kind -eq '1'})
        $get=@($rows|Where-Object {$_.kind -eq '2' -and [int]$_.index -lt 13})
        if($samples.Count -lt 24 -or $get.Count -ne 13 -or @($get.index|Select-Object -Unique).Count -ne 13 -or @($samples|Where-Object {$_.policy -ne '0' -or $_.driver -ne '0' -or $_.mask -ne '0' -or $_.valid -ne '1'}).Count){throw 'P12754 hardware audit incomplete or policy changed'}
        [pscustomobject]@{version=1;power_verified=$false;diagnostic=$true;module=$CP.Module;boot=$CP.Nonce;start=$s.start;end=$s.end;reason=$s.reason;get_attempted=$get.Count;get_valid=@($get|Where-Object valid -eq '1').Count;thermal_rows=$samples.Count;note='Connected hardware audit complete, not battery power. INT/DISP downstream rate not decoded; unavailable is not zero.'}|ConvertTo-Json|Set-Content -Encoding UTF8 -LiteralPath $out
        $script:P125Report.Add('P12754 connected hardware audit complete; raw registers and VSEL, not battery power; unsupported clock paths remain unavailable')
        Life-Say 'P12754 hardware audit complete (diagnostic only)';return
    }
    $t=P125-ReadSnapshot 'AUDIT53-POST' -CountsOnly
    $e=P127-Read
    $d=P126-Status
    $ps=P125-Fields (P12-Query 'p1273_psr')
    [pscustomobject]@{Timer=$t;Energy=$e;Driver=$d;PSR=$ps}|Export-Clixml -Depth 12 -LiteralPath "$Stem.audit53-post.clixml"
    if($t.Status.start -ne $s.start -or $t.Status.end -ne $s.end){throw 'P12753 P125 interval differs (legacy stop or another probe); retained frozen evidence only'}
    $counts=@()
    for($cpu=0;$cpu -lt 8;$cpu++){
        $c=[ordered]@{cpu=$cpu}
        for($i=0;$i -lt $script:P125Counters.Count;$i++){$c[$script:P125Counters[$i]]=P125-Sub $t.Counts[$cpu][$i] $CP.BeforeTimer.Counts[$cpu][$i]}
        $counts+=[pscustomobject]$c
    }
    $counts|Export-Csv -NoTypeInformation -Encoding UTF8 -LiteralPath "$Stem.audit53-counters.csv"
    P127-Delta $CP.BeforeEnergy $e $s 'AUDIT53' $Stem
    $fg=@();if(Test-Path -LiteralPath "$Stem.AUDIT53.fg.csv"){$fg=@(Import-Csv -LiteralPath "$Stem.AUDIT53.fg.csv")}
    $summary=P12753-Power $s $rows $fg $CP.Seconds
    $a=$CP.BeforeEnergy;$m=$e.Metrics
    $restore=$e.Status.mode -eq '0' -and $d.mode -eq '0' -and $ps.owner -ne '2' -and $ps.governor -eq '0' -and $ps.drained -eq '0' -and $ps.faults -eq $CP.BeforePSR.faults
    foreach($pair in @(@('abox_stop','abox_resume'),@('psr_mask','psr_unmask'))){
        if((P12753-Diff $a.Metrics $m $pair[0]) -ne (P12753-Diff $a.Metrics $m $pair[1])){$restore=$false}
    }
    foreach($group in @('BT','Wifi')){
        if((P12753-Diff $a.$group $e.$group 'attach') -ne (P12753-Diff $a.$group $e.$group 'detach')){$restore=$false}
    }
    foreach($key in @('abox_fault','psr_fault')){if($m[$key] -ne $a.Metrics[$key]){$restore=$false}}
    $hits=[ordered]@{}
    foreach($key in @('haptic','wifi_worker','gnss','touch','touch_leeway','shub')){$hits[$key]=P12753-Diff $CP.Before126 $d $key}
    $applied=switch($CP.Module){
        'BASE15' {($hits.haptic+$hits.wifi_worker+$hits.gnss+$hits.touch) -gt 0}
        'PSR' {(P12753-Diff $a.Metrics $m 'psr_mask') -gt 0}
        'LITTLE949' {(P12753-Diff $a.Status $e.Status 'floor_hits') -gt 0 -and @($rows|Where-Object {$_.kind -eq '1' -and [uint64]$_.lmeas -ge 921 -and [uint64]$_.lmeas -le 977}).Count -ge 2 -and [uint64]$e.Status.little_cur -ge 1053000}
        'ABOX' {(P12753-Diff $a.Metrics $m 'abox_stop') -gt 0 -and (P12753-Diff $a.Metrics $m 'abox_idle_wait') -gt 0}
        'BT' {(P12753-Diff $a.BT $e.BT 'attach') -gt 0 -and $a.BT.fallback -eq $e.BT.fallback}
        'WIFI' {(P12753-Diff $a.Wifi $e.Wifi 'attach') -gt 0 -and $a.Wifi.fallback -eq $e.Wifi.fallback}
        'TICKER' {(P12753-Diff $a.Ticker $e.Ticker 'wait') -gt 0}
        default {$true}
    }
    $rail=@($rows|Where-Object {$_.kind -eq '2' -and [int]$_.index -eq 104})
    $railVerified=$rail.Count -eq 2 -and @($rail|Where-Object {$_.valid -eq '1' -and $_.opp_valid -eq '1'}).Count -eq 2
    if($P12754 -and $CP.Module -match '^LITTLE(949|832|715|598)$'){
        $target=[int]$Matches[1];$thermal=@($rows|Where-Object {$_.kind -eq '1'})
        $matched=@($thermal|Where-Object {[int]$_.lkhz -eq 1000*$target -and [Math]::Abs([double]$_.lmeas-$target) -le .03*$target})
        $rate=P125-Fields (P12-Query 'p12754_rate')
        $rate|ConvertTo-Json|Set-Content -Encoding UTF8 -LiteralPath "$Stem.restore54.json"
        $restore=$restore -and $rate.fresh -eq '1' -and [uint64]$rate.cur -ge 1053000 -and [Math]::Abs(1000.0*[double]$rate.meas-[double]$rate.cur) -le .03*[double]$rate.cur
        $applied=$thermal.Count -gt 0 -and $matched.Count -ge [Math]::Ceiling(.8*$thermal.Count) -and $railVerified -and (P12753-Diff $a.Status $e.Status 'floor_hits') -gt 0 -and [uint64]$e.Status.little_cur -ge 1053000 -and $e.Status.ufs_safe -eq '1'
    }
    $names=@('MIF','INT','CPUCL0','CPUCL1','G3D','INTCAM','FSYS0','CAM','DISP','AUD','IVA','SCORE','CP')
    $bus=@($rows|Where-Object {$_.kind -eq '2' -and [int]$_.index -lt 13}|ForEach-Object {
        [pscustomobject]@{domain=$names[[int]$_.index];index=$_.index;at=$_.at;errno=$_.errno;raw=$_.raw;rate_khz=if($_.valid -eq '1'){$_.raw}else{$null};opp_table_uv=if($_.opp_valid -eq '1'){$_.opp_uv}else{$null};latency=$_.latency}
    })
    if($bus.Count){$bus|Export-Csv -NoTypeInformation -Encoding UTF8 -LiteralPath "$Stem.audit53-bus.csv"}
    $summary.power_verified=$summary.power_verified -and $restore -and $t.Status.reason -eq '10' -and ((P12-Query 'p1273_boot_identity') -eq $CP.Nonce)
    if(-not $summary.power_verified){$summary.mean_discharge_w=$null;$summary.discharge_wh=$null}
    $extra=[ordered]@{boot=$CP.Nonce;module=$CP.Module;request=$s.request;arm=$s.arm;start=$s.start;end=$s.end;policy=$s.policy;reason=$s.reason;stop_mask=$s.mask;applied=[bool]$applied;restored=[bool]$restore;driver_hits=$hits;bus_attempted=$bus.Count;bus_valid=@($bus|Where-Object {$null -ne $_.rate_khz}).Count;diagnostic=$CP.Module -eq 'BUS';rail_verified=[bool]$railVerified;note='Battery only, no extrapolation; driver deltas include setup/restore; sampled residency, not rail power'}
    foreach($k in $extra.Keys){$summary|Add-Member -NotePropertyName $k -NotePropertyValue $extra[$k]}
    $summary|ConvertTo-Json -Depth 6|Set-Content -Encoding UTF8 -LiteralPath $out
    Life-Say "P12753 $($CP.Module) verified=$($summary.power_verified) applied=$applied restored=$restore mean_W=$($summary.mean_discharge_w)"
    if(-not $summary.power_verified){throw 'P12753 incomplete or unsafe interval; retained data, not a power result'}
}
function P12753-RunAudit([string]$Stem){
    if((P12-Query 'p12753_revision') -ne '1'){throw 'P12753 ABI mismatch'}
    $g=P125-Fields (P12-Query 'p12753_guard')
    if($g.enabled -ne '1' -or $g.at -eq '0'){throw 'P12753 boot guard not running'}
    $s=P125-Fields (P12-Query 'p12753_status')
    if($s.state -notin @('0','3') -or (P126-Status).mode -ne '0' -or (P127-Status).mode -ne '0'){throw 'P12753 previous lease or audit active'}
    if($P12754){
        P12754-Require;P12754-PriorFloor $Audit53Module
        $g|ConvertTo-Json|Set-Content -Encoding UTF8 -LiteralPath "$Stem.preflight54.json"
        $w=P125-Fields (P12-Query 'p12754_wait')
        if($w.input_seen -ne '1'){throw 'P12754 requires a real touch event this boot before screen OFF'}
        if([int]$g.big -gt 60000 -or [int]$g.lit -gt 59000 -or [int]$g.gpu -gt 60000){throw 'P12754 cool first: BIG<=60C LITTLE<=59C GPU<=60C'}
    }
    $codes=@{STOCK120=1;BASE15=2;PSR=3;LITTLE949=4;ABOX=5;BT=6;WIFI=7;TICKER=8;BUS=9;STOCK60=10;LITTLE832=11;LITTLE715=12;LITTLE598=13}
    $code=$codes[$Audit53Module];$seconds=if($code -in @(1,9)){120}else{60}
    $cp=[pscustomobject]@{Nonce=(P12-Query 'p1273_boot_identity');Module=$Audit53Module;Request=$code;Seconds=$seconds;Arm='0';BeforeTimer=(P125-ReadSnapshot 'AUDIT53-PRE' -CountsOnly);BeforeEnergy=(P127-Read);Before126=(P126-Status);BeforePSR=(P125-Fields (P12-Query 'p1273_psr'))}
    $cp|Export-Clixml -Depth 12 -LiteralPath "$Stem.audit53-checkpoint.clixml"
    try{[void](P12-Query 'p12753_arm' "$code")}catch{if($P12754){P12-Query 'p12754_wait'|Set-Content -Encoding UTF8 -LiteralPath "$Stem.wait54.txt"};throw}
    $s=P125-Fields (P12-Query 'p12753_status')
    if($s.state -ne '1' -or $s.request -ne [string]$code -or $s.arm -eq '0'){throw 'P12753 arm not confirmed; bounded expiry'}
    $cp.Arm=$s.arm
    $cp|Export-Clixml -Depth 12 -LiteralPath "$Stem.audit53-checkpoint.clixml"
    P125-Offline (95+$seconds)
    P12753-ReadAudit $Stem $cp
}
function P12754-RunHardware([string]$Stem){
    P12754-Require
    $cp=[pscustomobject]@{Nonce=(P12-Query 'p1273_boot_identity');Module='HARDWARE54';Request=14;Seconds=60;Arm='0'}
    try{[void](P12-Query 'p12753_arm' '14')}catch{P12-Query 'p12754_wait'|Set-Content -Encoding UTF8 -LiteralPath "$Stem.wait54.txt";throw}
    $s=P125-Fields (P12-Query 'p12753_status')
    if($s.state -ne '1' -or $s.request -ne '14' -or $s.arm -eq '0'){throw 'P12754 arm unconfirmed; bounded expiry'}
    $cp.Arm=$s.arm;$cp|Export-Clixml -Depth 5 -LiteralPath "$Stem.audit53-checkpoint.clixml"
    $until=(Life-Now).AddSeconds(155)
    while($s.state -ne '3' -and (Life-Now) -lt $until){
        P125-PumpOff 3
        $s=P125-Fields (P12-Query 'p12753_status')
        if($s.arm -ne $cp.Arm -or $s.request -ne '14'){throw 'P12754 phase identity changed'}
    }
    P12753-ReadAudit $Stem $cp
}
function P12753-Compare($A,$B,$C){
    foreach($r in @($A,$B,$C)){
        if($r.version -ne 1 -or -not $r.power_verified -or -not $r.applied -or -not $r.restored -or $r.diagnostic -or $r.mean_discharge_w -le 0){throw 'P12753 comparison requires three verified applied restored non-diagnostic reports'}
    }
    $base=if($B.module -eq 'BASE15'){'STOCK60'}elseif($B.module -in @('PSR','LITTLE949','LITTLE832','LITTLE715','LITTLE598','ABOX','BT','WIFI','TICKER')){'BASE15'}else{throw 'P12753 unsupported B module'}
    if($A.module -ne $base -or $C.module -ne $base -or $A.boot -ne $B.boot -or $B.boot -ne $C.boot -or [uint64]$A.end -ge [uint64]$B.start -or [uint64]$B.end -ge [uint64]$C.start){throw 'P12753 wrong A/B/A order, baseline or boot'}
    foreach($r in @($B,$C)){
        if([Math]::Abs($r.window_s-$A.window_s) -gt 0.01 -or [Math]::Abs($r.start_big_mc-$A.start_big_mc) -gt 2000 -or [Math]::Abs($r.start_lit_mc-$A.start_lit_mc) -gt 2000 -or [Math]::Abs($r.mean_voltage_uv-$A.mean_voltage_uv) -gt 150000){throw 'P12753 unmatched thermal/voltage/window conditions'}
    }
    $baseW=($A.mean_discharge_w+$C.mean_discharge_w)/2
    if([Math]::Abs($A.mean_discharge_w-$C.mean_discharge_w) -gt 0.1*$baseW){throw 'P12753 baseline drift >10%; repeat'}
    [pscustomobject]@{module=$B.module;baseline=$base;A0_W=$A.mean_discharge_w;B_W=$B.mean_discharge_w;A1_W=$C.mean_discharge_w;delta_mW=1000*($baseW-$B.mean_discharge_w);note='Marginal measured difference, not additive or isolated rail power'}
}
function P12753-Options {
    if($Scenario -eq 'Hardware54' -and (-not $P12754 -or $P12752Idle)){throw 'Hardware54 requires -P12754, no -P12752Idle'}
    if(($Scenario -eq 'Audit53' -and $Audit53Module -in @('LITTLE832','LITTLE715','LITTLE598')) -or ($Scenario -eq 'FunctionalGate' -and $GateModule -in @('LITTLE832','LITTLE715','LITTLE598'))){
        if(-not $P12754 -or -not $AllowLowerFloor -or $Audit53Reports.Count -ne 3){throw 'P12754 deeper floor requires prior A/B/A JSON paths and explicit opt-in'}
    }
    if($Scenario -eq 'Audit53'){
        if($P12754 -and $Audit53Module -eq 'BUS'){throw 'P12754 use Hardware54 connected instead of legacy BUS'}
        if(-not $AllowOfflineTrial -or $P12752Idle){throw 'Audit53 requires -AllowOfflineTrial, without -P12752Idle'}
        if($Audit53Module -eq 'PSR'){
            if(-not $Audit53PriorReport){throw 'PSR requires reviewed same-boot connected PSR A/B/A and wake/no-tear confirmation'}
            $r=Get-Content -Raw -LiteralPath $Audit53PriorReport
            if($r -notmatch '(?m)^P127 MODULE PSR ABA_COMPLETE applied=True\s*$' -or $r -notmatch '(?m)^STOP=COMPLETE\s*$'){throw 'PSR prior report lacks completed applied A/B/A'}
        }
    }
    if($Scenario -eq 'Audit53Recover' -and (-not $Audit53Checkpoint -or $P12752Idle)){throw 'Audit53Recover requires -Audit53Checkpoint, without -P12752Idle'}
}
function P125-Run {
    P12752-CheckOptions
    P12753-Options
    if($Scenario -eq 'Audit53Compare'){
        if($Audit53Reports.Count -ne 3){throw 'Provide A0,B,A1 JSON paths in order'}
        $r=@($Audit53Reports|ForEach-Object {Get-Content -Raw -LiteralPath $_|ConvertFrom-Json})
        P12753-Compare $r[0] $r[1] $r[2]|ConvertTo-Json
        return
    }
    if(-not $Execute){
        Write-Host 'P12.5 preview: -Execute to measure OFF timer source. Trace=aggregate+detail; UsbCrossover=aggregate+A0/B/A1; OfflineOnly=B; AggregateOnly=aggregate; FrozenSummary=read-only counters; FrozenDump=read-only counters and pages on fixed firmware. No automatic cooling wait. DriverTrials=selective bounded A/B/A without page dumps; default COMBINED. ResumeSnapshot=FrozenDump checkpoint. EnergyTrials=guarded P12.7 A/B/A; EnergyOffline=connected/unplug/reconnect aggregate+WFI+FG. P1273Diagnostics=read-only floor/bus/WFIv2; BusTrace=90s baseline-only ACPM observation. Guard53=read-only boot thermal guard; Audit53=delayed battery window; Audit53Recover=frozen checkpoint; Audit53Compare=local A/B/A.'
        return
    }
    if($env:OS -ne 'Windows_NT'){throw 'Windows serial runner only'}
    if([IO.Ports.SerialPort]::GetPortNames() -notcontains $Port){throw "COM port $Port unavailable"}
    if($ReconnectPort -and $ReconnectPort -notmatch '^COM[0-9]+$'){throw 'ReconnectPort must be COM number'}
    $stem=Join-Path (Get-Location).Path ('putty-S9-p125-timer-'+(Get-Date -Format 'yyyyMMdd-HHmmss'))
    $raw=$stem+'.log';$errorText=$null;$script:P125Stem=$stem
    $script:P12Journal="$stem.readback-journal.txt"
    Life-Say 'P12.7.1 host serial parser: typed mixed replies, paced read-only retry, incremental checkpoint'
    try{
        $script:LifeRaw=[IO.FileStream]::new($raw,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read)
        P125-Open $Port
        Life-SyncReadOnly
        if($Scenario -eq 'Guard53'){
            if((P12-Query 'p12753_revision') -ne '1'){throw 'P12753 ABI mismatch'}
            if($P12754){P12754-Require;P12-Query 'p12754_wait'|Set-Content -Encoding UTF8 -LiteralPath "$stem.wait54.txt"}
            foreach($key in @('p12753_guard','p12753_status','p1273_boot_identity')){$v=P12-Query $key;Add-Content -LiteralPath "$stem.guard53.txt" -Encoding UTF8 -Value "$key $v";Write-Host "$key $v"}
            return
        }
        if($Scenario -eq 'Audit53Recover'){
            if((P12-Query 'p12753_revision') -ne '1'){throw 'P12753 ABI mismatch'}
            P12753-ReadAudit $stem (Import-Clixml -LiteralPath $Audit53Checkpoint)
            return
        }
        $s=P125-Status
        if($s.armed -ne '0'){throw 'P125 previous probe still armed; bounded expiry required'}
        if($P12752Idle){
            if((P12-Query 'p12752_revision') -ne '1'){throw 'P12752 idle firmware mismatch'}
            $latch=[int](P12-Query 'p12751_boot_latch')
            if($latch -notin @(3,7)){throw 'P12752 idle requires OptEarly boot latch'}
            $elig=P125-Fields (P12-Query 'p12752_eligibility')
            if($elig.enabled -ne '1' -or $elig.conflict -ne '0'){throw 'P12752 idle conflict/ineligible'}
        }
        if($Scenario -eq 'Idle52Recover'){
            P12752-RecoverFrozen $stem $ExpectedStart
            return
        }
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
        if($Scenario -eq 'ThermalGpuDump'){
            P1273-GpuDump $stem
            return
        }
        if($Scenario -eq 'ThermalTraceDump'){
            P1273-TraceDump $stem
            return
        }
        if($Scenario -eq 'P1273Diagnostics'){
            P1273-ReadDiagnostics $stem 'READONLY'
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
        if($Scenario -eq 'Hardware54'){P12754-RunHardware $stem;return}
        if($Scenario -eq 'Audit53'){
            P12753-RunAudit $stem
            return
        }
        if($Scenario -eq 'OffBurstCapABA'){
            P1273-CapABA $stem
            return
        }
        if($Scenario -eq 'ThermalTrace'){
            P1273-TraceCapture $stem
            return
        }
        if($Scenario -in @('Idle52','Idle52Offline')){
            $base=P125-ReadSnapshot 'P12752-IDLE-BASE' -CountsOnly
            if($Scenario -eq 'Idle52Offline'){[void](P127-Window 'P12752-OFFLINE-A0' 1 60 $stem $base -Offline)}
            else{[void](P127-Window 'P12752-IDLE-A0' 1 $EnergySeconds $stem $base)}
            return
        }
        if($Scenario -eq 'BusTrace'){
            P1273-ReadDiagnostics $stem 'PRE'
            $base=P125-ReadSnapshot 'P1273-BUS-BASE' -CountsOnly
            [void](P127-Window 'P1273-BUS' 1 90 $stem $base)
            P1273-ReadDiagnostics $stem 'POST'
            return
        }
        if($Scenario -eq 'GpuCacheCheck'){
            P1272-Check
            return
        }
        if($Scenario -eq 'FunctionalGate'){
            P1273-FunctionalGate $stem
            return
        }
        if($Scenario -eq 'CombinedConnected'){
            P1273-CombinedConnected $stem
            return
        }
        if($Scenario -eq 'OfflineABA'){
            P1273-OfflineABA $stem
            return
        }
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
        if($script:P1273CapDirty -and $null -ne $script:LifeSerial -and $script:LifeSerial.IsOpen){
            try{[void](P12-Query 'p1273_cap_mode' '0' -Cleanup);$script:P1273CapDirty=$false}catch{Life-Say 'P1273 cap stop unconfirmed; kernel bounded expiry'}
        }
        if($script:P1273TraceDirty -and $null -ne $script:LifeSerial -and $script:LifeSerial.IsOpen){
            try{[void](P12-Query 'p1273_trace_mode' '0' -Cleanup);$script:P1273TraceDirty=$false}catch{Life-Say 'P1273 trace freeze unconfirmed; bounded 180s expiry'}
        }
        if($script:P1272Dirty -and $null -ne $script:LifeSerial -and $script:LifeSerial.IsOpen){
            try{[void](P12-Query 'p1272_probe_request' '0' -Cleanup);$script:P1272Dirty=$false}catch{Life-Say 'P1272 request cancel unconfirmed; 60s bounded expiry'}
        }
        if($script:P127Dirty -and $null -ne $script:LifeSerial -and $script:LifeSerial.IsOpen){
            try{[void](P12-Query 'p127_mode' '0' -Cleanup);$script:P127Dirty=$false;Life-Say 'P127 cleanup mode=0 ACK'}catch{Life-Say 'P127 restore ACK missing; bounded kernel expiry'}
        }
        if($script:P126Dirty -and $null -ne $script:LifeSerial -and $script:LifeSerial.IsOpen){
            try{[void](P12-Query 'p126_mode' '0' -Cleanup);$script:P126Dirty=$false}catch{Life-Say 'P126 cleanup ACK missing; 120s bounded kernel lease'}
        }
        if($script:P125Dirty -and $null -ne $script:LifeSerial -and $script:LifeSerial.IsOpen){
            try{[void](P12-Query 'p125_mode' '0' -Cleanup);$script:P125Dirty=$false}catch{Life-Say 'P125 cleanup ACK missing; kernel lease <=180s'}
        }
        if($script:P12752IdleDirty -and $null -ne $script:LifeSerial -and $script:LifeSerial.IsOpen){
            try{[void](P12-Query 'p12752_idle_mode' '0' -Cleanup);$script:P12752IdleDirty=$false}catch{Life-Say 'P12752 idle recorder config reset unconfirmed; bounded expiry'}
        }
        if($script:P1273GateDirty -and $null -ne $script:LifeSerial -and $script:LifeSerial.IsOpen){
            try{[void](P12-Query 'p1273_gate_begin' '0' -Cleanup);$script:P1273GateDirty=$false}catch{Life-Say 'P1273 pending gate discard unconfirmed; expires in kernel'}
        }
        if($script:P1273OfflineDirty -and $null -ne $script:LifeSerial -and $script:LifeSerial.IsOpen){
            try{[void](P12-Query 'p1273_offline_s' '0' -Cleanup);$script:P1273OfflineDirty=$false}catch{Life-Say 'P1273 offline configuration reset unconfirmed; hardware policy remains bounded'}
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
