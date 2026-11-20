#requires -version 5.1
# Read-only S9 unified diagnostic serial-log report.
# powershell -NoProfile -ExecutionPolicy Bypass -File tools/s9-diagnostic-bundle-report.ps1 -Log putty-P12G21.log
param([Parameter(Mandatory=$true)][string]$Log)
$ErrorActionPreference='Stop'
$c=[ordered]@{p11Get=0;p11Return=0;p12Reads=0;prideSegv=0;prideAbort=0;layout=0;g21=0;gPath=0;gCost=0;glStage=0;temps=0;touches=0}
$g21=[Collections.Generic.List[object]]::new()
$path=[Collections.Generic.List[object]]::new()
$gl=[Collections.Generic.List[object]]::new()
$cpu=[Collections.Generic.List[object]]::new()
$procs=[Collections.Generic.List[object]]::new()
$governor=[Collections.Generic.List[object]]::new()
$layouts=[Collections.Generic.List[string]]::new()
$traces=[Collections.Generic.List[string]]::new()
$boot=''
foreach($s in [IO.File]::ReadAllLines((Resolve-Path -LiteralPath $Log).Path)) {
 if(!$boot -and $s -match 'boot-args = (.*)'){$boot=$Matches[1]}
 if($s -match 'native-metal-p11: userdict-enter'){$c.p11Get++}
 if($s -match 'native-metal-p11: userdict-return'){$c.p11Return++}
 if($s -match 'native-metal-p12: get=(\d+).*entries=(\d+)'){$c.p12Reads++;$traces.Add($s)}
 if($s -match 'PridePoster\.PridePosterExtension.*exited due to SIGSEGV'){$c.prideSegv++}
 if($s -match 'PridePoster\.PridePosterExtension.*exited due to SIGABRT'){$c.prideAbort++}
 if($s -match 'native-metal-playout: calls=(\d+)'){$c.layout++;$layouts.Add($s)}
 if($s -match '^hakios-gl-g21: up=([\d.]+)s frames=.*?attempted=(\d+) client_same=(\d+) client_diff=(\d+) cache_same=(\d+) cache_diff=(\d+) cache_invalid=(\d+) raw=(\d+) forwarded=(\d+) deletes=(\d+) other=(\d+)'){
   $g21.Add([pscustomobject]@{t=[double]::Parse($Matches[1],[Globalization.CultureInfo]::InvariantCulture)
     attempted=[long]$Matches[2];clientSame=[long]$Matches[3];clientDifferent=[long]$Matches[4]
     cacheSame=[long]$Matches[5];cacheDifferent=[long]$Matches[6];cacheInvalid=[long]$Matches[7]
     internalRaw=[long]$Matches[8];forwarded=[long]$Matches[9];deletes=[long]$Matches[10];other=[long]$Matches[11]});continue
 }
 if($s -match '^hakios-gl-path: up=([\d.]+)s frames=.*?bind_samples=(\d+) bind_images=(\d+) bind_checks=(\d+) bind_dirty=(\d+) native=(\d+)us lookup=(\d+)us check=(\d+)us upload=(\d+)us draw_samples=(\d+) draw_native_calls=(\d+) draw_native=(\d+)us draw_other=(\d+)us flush_raw=(\d+)us hash_samples=(\d+) hash_bytes=(\d+) hash_us=(\d+)us hash_same=(\d+) hash_changed=(\d+) hash_skipped=(\d+)'){
   $path.Add([pscustomobject]@{t=[double]::Parse($Matches[1],[Globalization.CultureInfo]::InvariantCulture);
     bindSample=[long]$Matches[2];bindImage=[long]$Matches[3];bindCheck=[long]$Matches[4];bindDirty=[long]$Matches[5]
     native=[long]$Matches[6];lookup=[long]$Matches[7];check=[long]$Matches[8];upload=[long]$Matches[9]
     drawSample=[long]$Matches[10];drawNativeCall=[long]$Matches[11];drawNative=[long]$Matches[12]
     drawOther=[long]$Matches[13];flushRaw=[long]$Matches[14]
     hashSample=[long]$Matches[15];hashBytes=[long]$Matches[16];hashUs=[long]$Matches[17];hashSame=[long]$Matches[18]
     hashChanged=[long]$Matches[19];hashSkip=[long]$Matches[20]});continue
 }
 if($s -match '^hakios-gl-stage: up=([\d.]+)s frames=.*? total=(\d+)us .*?slow16=(\d+) slow33=(\d+)'){
   $gl.Add([pscustomobject]@{t=[double]::Parse($Matches[1],[Globalization.CultureInfo]::InvariantCulture);total=[long]$Matches[2];slow16=[int]$Matches[3];slow33=[int]$Matches[4]});$c.glStage++;continue
 }
 if($s -match '^hakios-gl-cost: up='){$c.gCost++}
 if($s -match '^cputop: (\d+) s: (\d+)%'){$cpu.Add([pscustomobject]@{t=[int]$Matches[1]+15;total=[int]$Matches[2]})}
 if($s -match '^poster_cost: \d+ s: (PosterBoard|backboardd|CollectionsPoster)\[\d+\] cpu=([\d.]+)% .*?up=(\d+)s sample'){
   $procs.Add([pscustomobject]@{name=$Matches[1];cpu=[double]::Parse($Matches[2],[Globalization.CultureInfo]::InvariantCulture);t=[int]$Matches[3]})}
 if($s -match '^cpufreq_gov: (\d+) s: .*?temp big ([\d.]+) lit ([\d.]+) C'){
   $tt=[int]$Matches[1]+15;$big=[double]$Matches[2];$little=[double]$Matches[3]
   $touch=0;if($s -match 'boost: (\d+) touch'){$touch=[int]$Matches[1]}
   $governor.Add([pscustomobject]@{t=$tt;big=$big;little=$little;touch=$touch})
   $c.touches+=$touch
 }
}
function Sum($set,[string]$key){$x=@($set);if(!$x.Count){return 0};return ($x|Measure-Object -Property $key -Sum).Sum}
function Avg($set,[string]$key){$x=@($set);if(!$x.Count){return 'NA'};return [math]::Round(($x|Measure-Object -Property $key -Average).Average,2)}
Write-Output "LOG: $Log"
Write-Output "KERNEL: $([regex]::Match($boot,'hakios_kver=([^ ]+)').Groups[1].Value)"
Write-Output ("MARKERS P11 get={0}/return={1}, P12={2}, PrideSIGSEGV={3}, SIGABRT={4}, layoutProbe={5}, G21={6}, GLpath={7}, G1={8}, GLstage={9}, touches={10}" -f $c.p11Get,$c.p11Return,$c.p12Reads,$c.prideSegv,$c.prideAbort,$layouts.Count,$g21.Count,$path.Count,$c.gCost,$c.glStage,$c.touches)
if(!$g21.Count){Write-Warning 'Missing G2.1 marker: g21_diag=1 or userland mismatch'}
if(!$path.Count){Write-Warning 'Missing sampled GL path/hash markers: glpath=1 glhash=1 or userland mismatch'}
if(!$c.p12Reads){Write-Warning 'P1.2 diagnostic not exercised: check p12_diag=1 and Pride extension launch'}
foreach($w in @(@(120,270),@(295,340),@(340,420),@(420,600),@(600,900),@(900,1200),@(1200,1500))){
 $lo=$w[0];$hi=$w[1]
 $gp=@($g21|Where-Object{$_.t -ge $lo -and $_.t -lt $hi})
 $pa=@($path|Where-Object{$_.t -ge $lo -and $_.t -lt $hi})
 $cpuNow=@($cpu|Where-Object{$_.t -ge $lo -and $_.t -lt $hi})
 $govNow=@($governor|Where-Object{$_.t -ge $lo -and $_.t -lt $hi})
 $posterNow=@($procs|Where-Object{$_.t -ge $lo -and $_.t -lt $hi -and $_.name -eq 'PosterBoard'})
 $bbNow=@($procs|Where-Object{$_.t -ge $lo -and $_.t -lt $hi -and $_.name -eq 'backboardd'})
 if(!$gp.Count -and !$pa.Count){continue}
 Write-Output ("UPTIME {0}-{1}: cpu={2}% BIG={3}C touches={4} PosterBoard={5}% backboard={6}%" -f $lo,$hi,(Avg $cpuNow total),(Avg $govNow big),(Sum $govNow touch),(Avg $posterNow cpu),(Avg $bbNow cpu))
 Write-Output ("  G21 attempted={0} clientSame={1} cacheSame={2} cacheInvalid={3} cacheDifferent={4} rawInternal={5} forwarded={6} deleted={7}" -f (Sum $gp attempted),(Sum $gp clientSame),(Sum $gp cacheSame),(Sum $gp cacheInvalid),(Sum $gp cacheDifferent),(Sum $gp internalRaw),(Sum $gp forwarded),(Sum $gp deletes))
 Write-Output ("  GLpath sampleBind={0} native={1}us lookup={2}us seedCheck={3}us upload={4}us bindDirty={5} drawSample={6} driverDraw={7}us drawOther={8}us flushRaw={9}us hashSample={10} hashBytes={11} hashUs={12}us hashSame={13} hashChanged={14}" -f
  (Sum $pa bindSample),(Sum $pa native),(Sum $pa lookup),(Sum $pa check),(Sum $pa upload),(Sum $pa bindDirty),
  (Sum $pa drawSample),(Sum $pa drawNative),(Sum $pa drawOther),(Sum $pa flushRaw),
  (Sum $pa hashSample),(Sum $pa hashBytes),(Sum $pa hashUs),(Sum $pa hashSame),(Sum $pa hashChanged))
}
Write-Output 'P12 reads (first 12):'
$traces|Select-Object -First 12|ForEach-Object{Write-Output ('  '+$_)}
Write-Output 'Metal layout samples:'
$layouts|Select-Object -First 10|ForEach-Object{Write-Output ('  '+$_)}
Write-Output 'CPU governor uptime is approximately mach uptime minus 15s, as in prior S9 logs; telemetry and thermal are observations, not proof of power savings.'
