#requires -Version 5.1
# Host-only analysis of retained live samples, UFS summaries, frozen PID/TID pages.
# No COM port, sysctl, policy change, or writes to the device.
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$SamplesCsv,
    [string]$RawLog='',
    [string]$ThermalPages='',
    [string]$ThermalStatus=''
)
Set-StrictMode -Version 2
$ErrorActionPreference='Stop'
if([bool]$ThermalPages -ne [bool]$ThermalStatus){throw 'Supply matching frozen pages and status together'}
$samples=@(Import-Csv -LiteralPath $SamplesCsv)
$off=@($samples|Where-Object {$_.display -eq 'OFF'})
$proc=@($off|Where-Object type -eq 'PROCESS_TOP')
$process=@(foreach($name in @('backboard','springboard','locationd','wifid','gnssd','poster_cpu')){
    $field=$name+'_pct'
    $v=@($proc|Where-Object {$_.PSObject.Properties[$field] -and $_.$field -ne ''}|ForEach-Object {[double]$_.$field})
    if($v.Count){
        $stat=$v|Measure-Object -Average -Maximum
        [pscustomobject]@{name=$name;reported=$v.Count;of=$proc.Count;mean_reported_pct=$stat.Average;max_reported_pct=$stat.Maximum}
    }
})
$ufs=@()
if($RawLog){
    $up=$null;$previous=$null
    foreach($line in Get-Content -LiteralPath $RawLog){
        if($line -match 'ufs: nufs: summary \(every 30 s, up (\d+) s\)'){$up=[uint64]$Matches[1];continue}
        if($null -eq $up -or $line -notmatch 'ufs: nufs: summary: reads (\d+) native, (\d+) through the loader \((\d+) MB\), (\d+) failed; writes (\d+) \((\d+) MB\), (\d+) failed; refused (\d+)'){continue}
        $cur=[pscustomobject]@{up_s=$up;reads=[uint64]$Matches[1];loader_reads=[uint64]$Matches[2];read_mb=[uint64]$Matches[3];read_errors=[uint64]$Matches[4];writes=[uint64]$Matches[5];write_mb=[uint64]$Matches[6];write_errors=[uint64]$Matches[7];refused=[uint64]$Matches[8]}
        if($null -ne $previous -and $cur.up_s -gt $previous.up_s -and $cur.reads -ge $previous.reads -and $cur.writes -ge $previous.writes -and $cur.write_mb -ge $previous.write_mb){
            $dt=$cur.up_s-$previous.up_s
            $ufs+= [pscustomobject]@{start_up_s=$previous.up_s;end_up_s=$cur.up_s;read_delta=$cur.reads-$previous.reads;write_delta=$cur.writes-$previous.writes;writes_per_s=($cur.writes-$previous.writes)/[double]$dt;write_mb_rounded_delta=$cur.write_mb-$previous.write_mb;read_errors=$cur.read_errors;write_errors=$cur.write_errors;refused=$cur.refused}
        }
        $previous=$cur;$up=$null
    }
}
$trace=$null
if($ThermalPages){
    $status=Import-Clixml -LiteralPath $ThermalStatus
    if($status.state -ne '4' -or [uint64]$status.span -eq 0){throw 'Trace is not frozen'}
    $pages=@{};$buckets=@{};$owners=@{}
    foreach($line in Get-Content -LiteralPath $ThermalPages){
        if($line -notmatch '^version=1 select=(\d+) generation=(\d+) present=1 bin=(\d+) mask=([0-9a-f]+) dropped=(\d+) dropped_ticks=(\d+)(.*)$'){continue}
        $sel=[int]$Matches[1];$gen=$Matches[2];$bin=[uint64]$Matches[3];$mask=$Matches[4];$drops=[uint64]$Matches[5];$dticks=[uint64]$Matches[6];$tail=$Matches[7]
        if($gen -ne [string]$status.generation -or $sel -lt 0 -or $sel -ge 8192 -or [int]($bin%128) -ne [int]([Math]::Floor($sel/8)%128) -or $pages.ContainsKey($sel)){throw 'Trace generation, selector or duplicate page mismatch'}
        $pages[$sel]=$true
        $cpu=[int][Math]::Floor($sel/1024);$slot=$sel%8;$bucketKey="$cpu/$bin"
        if($buckets.ContainsKey($bucketKey)){
            if($buckets[$bucketKey].mask -ne $mask -or $buckets[$bucketKey].dropped -ne $drops -or $buckets[$bucketKey].dropped_ticks -ne $dticks){throw 'Trace bucket changed'}
        }else{$buckets[$bucketKey]=[pscustomobject]@{mask=$mask;dropped=$drops;dropped_ticks=$dticks}}
        $m=[Convert]::ToUInt32($mask,16);if($m -gt 65535){throw 'Trace mask outside 16 owners'}
        $expected=0;foreach($j in @(0,1)){if($m -band (1 -shl ($slot*2+$j))){$expected++}}
        $records=[regex]::Matches($tail,' r=(\d+)/(\d+)/(\d+)/(\d+)/(\d+)/(\d+)/(\d+)/(\d+)')
        if($records.Count -ne $expected){throw 'Trace owner count mismatch'}
        foreach($rec in $records){
            $tid=$rec.Groups[1].Value;$task=$rec.Groups[2].Value;$ownerPid=$rec.Groups[3].Value;$kind=$rec.Groups[4].Value
            $key="$ownerPid/$task/$tid/$kind"
            if(-not $owners.ContainsKey($key)){$owners[$key]=[pscustomobject]@{pid=$ownerPid;task_generation=$task;tid=$tid;kind=$kind;user_ticks=[uint64]0;system_ticks=[uint64]0;fragments=[uint64]0;switches=[uint64]0}}
            $o=$owners[$key];$o.user_ticks+=[uint64]$rec.Groups[5].Value;$o.system_ticks+=[uint64]$rec.Groups[6].Value;$o.fragments+=[uint64]$rec.Groups[7].Value;$o.switches+=[uint64]$rec.Groups[8].Value
        }
    }
    if(-not $pages.Count){throw 'No frozen owner pages found'}
    $trace=[pscustomobject]@{generation=$status.generation;trigger=$status.trigger;reason=$status.reason;pages=$pages.Count;buckets=$buckets.Count;dropped_ticks=($buckets.Values|Measure-Object dropped_ticks -Sum).Sum;owners=@($owners.Values|Sort-Object { $_.user_ticks+$_.system_ticks } -Descending|Select-Object -First 30);note='Observed frozen pages only, not proof of full coverage. No-trigger or dropped/clipped trace is inconclusive. PID/task/TID are scalar identity, not a name or originating UFS writer.'}
}
[pscustomobject]@{version=1;off_samples=$off.Count;process_samples=$proc.Count;process_reported=$process;ufs_intervals=$ufs;trace=$trace;note='Process means exclude unreported samples. UFS raw summaries may include historical boot before the live fence; use uptime and the matching ufs541 bracket. Temporal correlation is not writer attribution. No battery watts.'}|ConvertTo-Json -Depth 8
