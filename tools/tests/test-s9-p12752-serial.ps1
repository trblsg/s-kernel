#requires -Version 5.1
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../s9-p1274-serial.ps1') -LibraryOnly
function Check($ok,[string]$why){if(-not $ok){throw $why}}
function Refuse([scriptblock]$b,[string]$pattern){$refused=$false;try{& $b}catch{if($_.Exception.Message -match $pattern){$refused=$true}else{throw}};Check $refused "missing refusal $pattern"}
Check (P12-ReplyShape 'p12752_revision' '1' $true) 'revision'
Check (P12-ReplyShape 'p12752_eligibility' 'version=1 enabled=1 conflict=0 ocp=0 ufs_safe=1 ufs_busy=0 quiet=1 thermal=0 cores=4 online=68 exit=0' $true) 'eligibility shape'
Check (P12-ReplyShape 'p12752_idle_info' 'version=1 mode=0 phase=1000 count=6 drops=0 reason=0 bus=0' $true) 'idle info'
$row='version=1 select=0 phase=1000 at=1000 kind=1 index=0 raw=0 valid=1 errno=0 latency=0 big=62000 lit=60500 gpu=55000 bat=37 age=10 bs=4 ls=10 bkhz=650000 lkhz=1053000 bmeas=650 lmeas=1053 policy=1 driver=0 refresh=300'
Check (P12-ReplyShape 'p12752_idle_page' $row $true) 'idle page'
Check (-not (P12-ReplyShape 'p12752_idle_page' ($row+' junk') $true)) 'malformed page'
foreach($bad in @(@('p12752_profile','2'),@('p12752_idle_mode','1'),@('p12752_idle_select','0'),@('p12752_eligibility','1'))){
 Refuse { [void](P12-Query $bad[0] $bad[1]) } 'only permits|disallowed'
}
$opts.P12752=$true;$opts.Profile='R2';$opts.Power='Connected';$opts.BootVariant='OptEarly';$opts.Experiment='EarlyLoad';$opts.AllowIntervention=$true
Check-Prior
$opts.Profile='R1';Refuse {Check-Prior} 'prior qualified'
$opts.Profile='R2';$opts.BootVariant='StockEarly';Refuse {Check-Prior} 'OptEarly'
$P12752Idle=$true;$Scenario='Idle52Offline';$EnergySeconds=60;$AllowOfflineTrial=$false
Refuse {P12752-CheckOptions} 'explicit'
$AllowOfflineTrial=$true;P12752-CheckOptions
$Scenario='EnergyTrials';$EnergyModules='PSR,USB';Refuse {P12752-CheckOptions} 'one idle module'
$EnergyModules='LITTLE598';$AllowLowerFloor=$false;Refuse {P12752-CheckOptions} 'prior reviewed'
$dir=Join-Path $env:TEMP ('p12752-test-'+[guid]::NewGuid())
[void](New-Item -ItemType Directory -Path $dir)
try{
 $script:sel52=0;$script:info52='version=1 mode=0 phase=1000 count=6 drops=0 reason=0 bus=0'
 function P12-EarlyThermalGuard{}
 function P12-Query([string]$Name,[string]$Value='',[switch]$Cleanup){
  switch($Name){
   'p12752_idle_info'{return $script:info52}
   'p12752_idle_select'{$script:sel52=[int]$Value;return $Value}
   'p12752_idle_page'{return ($row -replace 'select=0',("select="+$script:sel52) -replace 'at=1000',("at="+(1000+2000*$script:sel52)))}
   'p12752_idle_mode'{return '0'}
   default{throw "unexpected $Name"}
  }
 }
 $status=@{start='1000';end='11000';hz='1000';reason='0';display_on='0';armed='0'}
 $out=P12752-ReadIdle (Join-Path $dir 'run') 'A0' $status
 Check ($out.thermal_valid -and $out.count -eq 6) 'valid mock not accepted'
 $status.reason='2';$out=P12752-ReadIdle (Join-Path $dir 'run2') 'A0' $status
 Check (-not $out.thermal_valid) 'thermal cutoff accepted'
 $status.start='2000';Refuse {P12752-ReadIdle (Join-Path $dir 'run3') 'A0' $status} 'identity'
 Set-Content -LiteralPath (Join-Path $dir 'Capture-test.log') -Value @('P12752 sample at=300000 pid=42 name=PosterBoard tid=12 usage=300 pc=ImageIO+0x123 lr=CoreGraphics+0x55','P12752 sample_done pid=42 looks=1 kept=1 failures=0','P12752 layout at=300001 n=32 format=80','P12752 mem at=300000 pid=42 code=1 rss=100','launchd spawn PosterBoard')
 $raw=P12752-RawEvidence $dir 1000
 Check ($raw.samples -eq 1 -and $raw.processes[0] -eq 'PosterBoard' -and $raw.layout_lines -eq 1 -and $raw.memory_lines -eq 1 -and $raw.lifecycle_lines -eq 1) 'raw evidence parser'
}finally{Remove-Item -LiteralPath $dir -Recurse -Force}
Write-Host 'P12752 host shapes/refusals/idle identity/cutoff/raw evidence mocks PASS; no serial I/O'

# Run the real P125-Offline control flow using an unopened .NET SerialPort.
# Windows can raise a wrapped InvalidOperationException before enumeration
# removes COM125. No physical serial handle is opened by these tests.
function Test-OfflineTransport([string]$Case){
    $script:fakeCase=$Case;$script:fakeAt=[datetime]::UtcNow
    $script:fakeList=0;$script:fakeRead=0;$script:fakeOpen=0;$script:fakeFence=0
    $script:fakeReconnect=$false;$script:fakeLost=$false
    $script:LifeSerial=[IO.Ports.SerialPort]::new()
    $script:LifeState='OFF';$script:LifeCurrentSegment=$null
    $Port='COM125';$ReconnectPort='COM126';$Scenario='Idle52Offline'
    $P12752Idle=$Case -in @('ok','stale','hot','parser','timeout','kernel_early','fencehot')
    if($Case -eq 'stale'){$ReconnectPort=''}
    $script:expectedPort=if($ReconnectPort){$ReconnectPort}else{$Port}
    $script:holdList=0;$script:hold=$false
    function Life-Now {$script:fakeAt=$script:fakeAt.AddSeconds(1);return $script:fakeAt}
    function Start-Sleep {param([int]$Milliseconds)}
    function Life-Say([string]$Message){
        if($Message -match 'keep unplugged'){$script:hold=$true}
        if($Message -match 'RECONNECT USB NOW'){$script:hold=$false;$script:fakeReconnect=$true}
    }
    function Life-CheckSafety([datetime]$At){if($script:fakeCase -eq 'hot'){throw 'synthetic thermal stop'}}
    function P12-EarlyThermalGuard{}
    function P125-PortNames{
        $script:fakeList++
        if($script:hold){$script:holdList++}
        if($script:fakeReconnect){
            if($script:fakeCase -ne 'timeout'){return $script:expectedPort}
            return
        }
        if($script:fakeCase -in @('stuck','stale','kernel_early') -or $script:fakeList -le 3){return 'COM125'}
        if($script:fakeCase -eq 'early' -and $script:fakeList -ge 7){return 'COM125'}
    }
    function Life-Pump{
        $script:fakeRead++
        if($script:fakeCase -eq 'parser'){throw 'synthetic malformed telemetry'}
        $buf=New-Object byte[] 16
        # Read() on a closed port raises the exact .NET exception class.
        [void]$script:LifeSerial.Read($buf,0,$buf.Length)
    }
    function P125-Open([string]$Name){
        if($Name -ne $script:expectedPort){throw 'wrong COM reopened'}
        if($script:hold -or -not $script:fakeReconnect){throw 'opened before reconnect prompt'}
        $script:fakeOpen++
        if($script:fakeOpen -eq 1 -or $script:fakeCase -eq 'timeout'){throw [System.IO.IOException]::new('synthetic enumeration race')}
        $script:LifeSerial=[IO.Ports.SerialPort]::new()
    }
    function Life-SyncReadOnly{
        $script:fakeFence++
        if($script:fakeCase -eq 'stale' -and $script:fakeFence -eq 1){throw 'P10 display sysctl reply absent; synthetic stale handle'}
        if($script:fakeCase -eq 'fencehot'){throw 'synthetic thermal stop in live fence'}
        $script:LifeState='OFF'
    }
    try{
        switch($Case){
            {$_ -in @('ok','stale','kernel_early')}{
                P125-Offline 6
                $openCount=if($Case -eq 'stale'){3}else{2}
                $fenceCount=if($Case -eq 'stale'){2}else{1}
                Check ($script:fakeRead -eq 1 -and $script:fakeOpen -eq $openCount -and $script:fakeFence -eq $fenceCount -and $script:holdList -eq 0) 'offline reopen/fence or silent hold failed'
            }
            'hot'{Refuse {P125-Offline 6} 'thermal stop';Check ($script:fakeRead -eq 0 -and $script:fakeOpen -eq 0) 'thermal error swallowed'}
            'parser'{Refuse {P125-Offline 6} 'malformed telemetry';Check ($script:fakeOpen -eq 0) 'parser error swallowed'}
            'stuck'{Refuse {P125-Offline 6} 'COM did not disappear';Check ($script:fakeOpen -eq 0) 'false unplug accepted'}
            'early'{Refuse {P125-Offline 6} 'reconnected early';Check ($script:fakeOpen -eq 0) 'early reconnect accepted'}
            'timeout'{Refuse {P125-Offline 6} 'reconnect timeout';Check ($script:fakeOpen -gt 0 -and $script:fakeFence -eq 0) 'timeout did not remain bounded to target'}
            'fencehot'{Refuse {P125-Offline 6} 'thermal stop in live fence';Check ($script:fakeOpen -eq 2 -and $script:fakeFence -eq 1) 'thermal stop retried'}
        }
    }finally{if($null -ne $script:LifeSerial){$script:LifeSerial.Dispose();$script:LifeSerial=$null}}
}
foreach($case in @('ok','stale','kernel_early','hot','parser','stuck','early','timeout','fencehot')){Test-OfflineTransport $case}
$arm=@{anchor='abcd';hz='26000000';now='1000'}
$end=@{anchor='abcd';hz='26000000';now='5000';start='1200';down='1200'}
Check (P125-RejoinMatches $arm $end) 'valid offline rejoin rejected'
$end.anchor='dcba';Check (-not (P125-RejoinMatches $arm $end)) 'wrong kernel accepted'
$end.anchor='abcd';$end.start='900';$end.down='900';Check (-not (P125-RejoinMatches $arm $end)) 'old phase accepted'
$end.start='1200';$end.down='0';Check (-not (P125-RejoinMatches $arm $end)) 'missing disconnect accepted'
$link=@{hz='26000000';start='2600000000';down='2600000000';end='4160000000';up='4160000000'}
Check (P125-LinkIntervalValid $link 60) 'complete kernel interval rejected'
$link.end='2860000000';$link.up=$link.end
Check (-not (P125-LinkIntervalValid $link 60)) 'early physical reconnect accepted'
$link.end='4160000000';$link.up='0'
Check (-not (P125-LinkIntervalValid $link 60)) 'missing link-up accepted'
$link.up=$link.end;$link.down='0'
Check (-not (P125-LinkIntervalValid $link 60)) 'missing link-down accepted'
$link.down=$link.start;Check (P125-LinkIntervalValid $link 60 -Fixed) 'fixed kernel interval rejected'
Write-Host 'P12752 real offline flow: closed Read, persistent COM125 name, silent hold, stale Open/fence retry, thermal/parser refusal, legacy early/stuck, timeout, phase identity and early/missing kernel link evidence PASS (no physical serial)'
