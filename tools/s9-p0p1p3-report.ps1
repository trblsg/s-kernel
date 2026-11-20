#requires -version 5.1
# Read-only PuTTY comparison helper for one S9 P0/P1/P3 session.
param([Parameter(Mandatory=$true)][string]$Log,[string]$Baseline)
$ErrorActionPreference='Stop'
function Parse([string]$p) {
 $o=[ordered]@{File=$p;Kernel='';P0=@();Hash=@();Power=@();Pride=@();Crash=0;P1Type=@();G1=@();GLStage=@();Gov=@();Top=@()}
 $p0=[Collections.Generic.List[object]]::new()
 $hs=[Collections.Generic.List[object]]::new()
 $power=[Collections.Generic.List[object]]::new()
 $p1=[Collections.Generic.List[string]]::new()
 $g1=[Collections.Generic.List[object]]::new()
 $st=[Collections.Generic.List[object]]::new()
 $gov=[Collections.Generic.List[object]]::new()
 $cpu=[Collections.Generic.List[object]]::new()
 foreach($line in [IO.File]::ReadAllLines((Resolve-Path -LiteralPath $p).Path)) {
  if(!$o.Kernel -and $line -match 'boot-args = .*?hakios_kver=([^ ]+)'){$o.Kernel=$Matches[1]}
  if($line -match '^hakios-gl-p0: up=([\d.]+)s frames=\d+\.\.\d+ fast=(\d+) scalar=(\d+) verified=(\d+) mismatch=(\d+) fallback=(\d+)'){
   $p0.Add([pscustomobject]@{t=[double]::Parse($Matches[1],[Globalization.CultureInfo]::InvariantCulture);fast=[int]$Matches[2];scalar=[int]$Matches[3];verified=[int]$Matches[4];bad=[int]$Matches[5];fallback=[int]$Matches[6]});continue
  }
  if($line -match '^hakios-gl-path: up=([\d.]+)s .*?hash_samples=(\d+) hash_bytes=(\d+) hash_us=(\d+)us hash_same=(\d+) hash_changed=(\d+) hash_skipped=(\d+)'){
   $hs.Add([pscustomobject]@{t=[double]::Parse($Matches[1],[Globalization.CultureInfo]::InvariantCulture);n=[int]$Matches[2];bytes=[long]$Matches[3];us=[long]$Matches[4];same=[int]$Matches[5];changed=[int]$Matches[6];skip=[int]$Matches[7]});continue
  }
  if($line -match '^mali-power: up=(\d+)ms domain=(\d+) status=(\S+) config=(\S+) awake=(\d+) sleep=(\d+) pll_en=(\d+) pll_stable=(\d+) mux=(\d+) clock=(\d+)MHz frame_age_ms=(\d+) job_age_ms=(\d+) frame_valid=(\d+) job_valid=(\d+) framefloor=(\d+)MHz dvs=(\d+) rail_read=(\d+) rail_valid=(\d+) rail_ctrl=(\S+) rail_mv_last=(\d+)'){
   $power.Add([pscustomobject]@{t=[double]$Matches[1]/1000;domain=[int]$Matches[2];awake=[int]$Matches[5];pll=[int]$Matches[7];mux=[int]$Matches[9];clock=[int]$Matches[10];frameAge=[int]$Matches[11];jobAge=[int]$Matches[12];floor=[int]$Matches[15];railRead=[int]$Matches[17];railValid=[int]$Matches[18];mv=[int]$Matches[20]});continue
  }
  if($line -match 'native-metal-p1mps: command='){$p1.Add($line)}
  if($line -match 'PridePoster\.PridePosterExtension.*exited due to SIGSEGV'){$o.Crash++}
  if($line -match '^cpufreq_gov: (\d+) s: .*?temp big ([\d.]+) lit ([\d.]+) C'){$gov.Add([pscustomobject]@{t=[int]$Matches[1]+15;big=[double]$Matches[2]})}
  if($line -match '^cputop: (\d+) s: (\d+)% in all:'){$cpu.Add([pscustomobject]@{t=[int]$Matches[1]+15;percent=[int]$Matches[2]})}
  if($line -match '^hakios-gl-stage: up=([\d.]+)s frames=.*? n=(\d+) draw=(\d+)us .*? total=(\d+)us'){
   $st.Add([pscustomobject]@{t=[double]::Parse($Matches[1],[Globalization.CultureInfo]::InvariantCulture);n=[int]$Matches[2];draw=[long]$Matches[3];total=[long]$Matches[4]});continue
  }
 }
 $o.P0=$p0.ToArray();$o.Hash=$hs.ToArray();$o.Power=$power.ToArray()
 $o.P1Type=$p1.ToArray();$o.GLStage=$st.ToArray();$o.Gov=$gov.ToArray();$o.Top=$cpu.ToArray()
 return [pscustomobject]$o
}
function Sm($seq,[string]$k){$v=@($seq);if(!$v.Count){return 0};return ($v|Measure-Object -Property $k -Sum).Sum}
function Av($seq,[string]$k){$v=@($seq);if(!$v.Count){return 'N/A'};return [math]::Round(($v|Measure-Object -Property $k -Average).Average,2)}
$names=@($Log);if($Baseline){$names+=@($Baseline)}
foreach($path in $names) {
 $d=Parse $path
 Write-Output ('=== '+$d.File+' === kernel='+$d.Kernel)
 Write-Output ('Pride SIGSEGV='+$d.Crash+' MPS object-class lines='+$d.P1Type.Count+' P0 samples='+$d.P0.Count+' hash samples='+$d.Hash.Count+' P3 power samples='+$d.Power.Count)
 Write-Output ('P0 fast='+ (Sm $d.P0 fast)+' scalar='+ (Sm $d.P0 scalar)+' verified='+ (Sm $d.P0 verified)+' mismatches='+(Sm $d.P0 bad)+' fallback windows='+@($d.P0|Where-Object{$_.fallback}).Count)
 foreach($z in @(@(120,270),@(295,340),@(420,600),@(600,900),@(900,1200))) {
  $lo=$z[0];$hi=$z[1]
  $p0=@($d.P0|Where-Object{$_.t -ge $lo -and $_.t -lt $hi})
  $hs=@($d.Hash|Where-Object{$_.t -ge $lo -and $_.t -lt $hi})
  $pw=@($d.Power|Where-Object{$_.t -ge $lo -and $_.t -lt $hi})
  $gv=@($d.Gov|Where-Object{$_.t -ge $lo -and $_.t -lt $hi})
  $cpu=@($d.Top|Where-Object{$_.t -ge $lo -and $_.t -lt $hi})
  $st=@($d.GLStage|Where-Object{$_.t -ge $lo -and $_.t -lt $hi})
  if(!$gv.Count -and !$hs.Count){continue}
  $us=Sm $hs us;$bytes=Sm $hs bytes;$hashes=Sm $hs n
  $nsPerByte=if($bytes){[math]::Round($us*1000/$bytes,3)}else{'N/A'}
  $avgUs=if($hashes){[math]::Round($us/$hashes,1)}else{'N/A'}
  $frames=Sm $st n;$total=0
  foreach($v in $st){$total+=$v.total*$v.n}
  $glms=if($frames){[math]::Round($total/$frames/1000,3)}else{'N/A'}
  $domainOn=@($pw|Where-Object{$_.domain -eq 1}).Count
  $domainOff=@($pw|Where-Object{$_.domain -eq 0}).Count
  Write-Output ("UPTIME {0}-{1}s CPUavg={2}% BIG={3}C GLstage={4}ms/frame hashN={5} hashBytes={6} hashUs={7} avgHashUs={8} nsPerByte={9} unchanged={10} changed={11} P0verified={12} P0bad={13} GPUdomainOn={14} off={15} P3n={16} idleFrameAgeAvg={17}ms GPUclockAvg={18}MHz" -f $lo,$hi,(Av $cpu percent),(Av $gv big),$glms,$hashes,$bytes,$us,$avgUs,$nsPerByte,(Sm $hs same),(Sm $hs changed),(Sm $p0 verified),(Sm $p0 bad),$domainOn,$domainOff,$pw.Count,(Av $pw frameAge),(Av $pw clock))
 }
 if($d.P1Type.Count){Write-Output 'P1 MPS first class shapes:';$d.P1Type|Select-Object -First 8|ForEach-Object{Write-Output ('  '+$_)}}
}
Write-Output 'Hash times are 1/16 sampled work; no measured current/watt. Similar start temperatures and zero touch needed for fair A/B.'
