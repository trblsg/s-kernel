#requires -Version 5.1
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../s9-p125-timer-serial.ps1') -LibraryOnly -P12754
function Check($ok,[string]$why){if(-not $ok){throw $why}}
function Refuse([scriptblock]$action,[string]$pattern){$refused=$false;try{& $action}catch{if($_.Exception.Message -match $pattern){$refused=$true}else{throw}};Check $refused "missing refusal $pattern"}
Check (P12-ReplyShape 'p12754_wait' 'version=1 enabled=1 preflight=0 wait=512 seen=520 quiet_ticks=123 input_seen=1 fingers=0' $true) 'wait shape'
Check (P12-ReplyShape 'p12754_rate' 'version=1 fresh=1 cur=1053000 meas=1049' $true) 'rate shape'
Check (-not (P12-ReplyShape 'p12754_wait' 'version=1 enabled=1 preflight=0 wait=0' $true)) 'truncated wait'
Check ((P1273-GateBit 33) -eq 1024 -and (P1273-GateBit 49) -eq 2048 -and (P1273-GateBit 65) -eq 4096) 'floor masks'
Refuse {P12754-PriorFloor 'LITTLE598'} 'prior same-boot'
$status='version=1 state=3 request=14 arm=1000 down=0 start=25000 end=85000 until=85000 up=0 now=100000 hz=1000 reason=10 mask=0 count=60 drops=0 bus=0 stop_big=62000 stop_lit=60500 stop_gpu=55000 stop_bat=37 stop_age=10 policy=0'
$s=P125-Fields $status
$cp=[pscustomobject]@{Nonce='bootA';Request=14;Arm='1000';Module='HARDWARE54'}
$script:mockStatus=$status;$script:mockNonce='bootA';$script:cursor='0';$script:writes=0
function P12-Query([string]$Name,[string]$Value=''){
    switch($Name){
        'p1273_boot_identity'{return $script:mockNonce}
        'p12753_status'{return $script:mockStatus}
        'p12753_select'{if($Value -ne ''){$script:writes++;$script:cursor=$Value;throw 'Missing echo and sysctl readback'};return $script:cursor}
        'p12753_page'{return "version=1 select=$script:cursor phase=25000"}
        default{throw "Unexpected query $Name"}
    }
}
$r=P12754-FrozenRow $s $cp 7
Check ($r.select -eq '7' -and $script:writes -eq 1) 'readback after lost echo'
$script:mockNonce='bootB';Refuse {P12754-FrozenRow $s $cp 8} 'boot identity changed'
$script:mockNonce='bootA';$script:mockStatus=$status.Replace('arm=1000','arm=2000')
Refuse {P12754-FrozenRow $s $cp 9} 'frozen phase changed'
Write-Host 'P12754 typed diagnostics, floor prerequisites and frozen selector recovery PASS (mock only)'
# Revision 2 typed stats and coverage: no power result, no missing register
# can be silently represented as zero or a successful complete audit.
$stat='version=1 skips=7'
for($i=0;$i -lt 12;$i++){$bit=1 -shl $i;$stat+=" n$bit=1 t$bit=260000"}
Check (P12-ReplyShape 'p127541_stats' $stat $true) 'revision 2 stats shape'
Check (-not (P12-ReplyShape 'p127541_stats' ($stat -replace ' t2048=260000$','') $true)) 'truncated stats refused'
Check (P12-ReplyShape 'p12754_revision' '2' $true) 'revision 2 shape'
Check (P12-ReplyShape 'p12754_revision' '3' $true) 'revision 3 shape'
Check (P12-ReplyShape 'p12754_revision' '4' $true) 'revision 4 shape'
Check (P12-ReplyShape 'p12754_revision' '5' $true) 'revision 5 shape'
Check ((P127543-ThermalExcess @{big=61000;lit=59576;gpu=59000}) -gt 0) '063643 cold blocker'
Check ((P127543-ThermalExcess @{big=61000;lit=59576;gpu=59000} -Warm) -le 0) '063643 warm admission'
Check ((P127543-ThermalExcess @{big=63000;lit=62000;gpu=62000} -Warm) -eq 0) 'warm boundary inclusive'
Check ((P127543-ThermalExcess @{big=63001;lit=62000;gpu=62000} -Warm) -eq 1) 'warm above boundary'
Check (P12-ReplyShape 'p127544_window' 'version=1 valid=1 arm=1000 start=25000 end=85000 irq27=300 wfi_n=100 wfi_ticks=200 ge_n=5 ge_ticks=50' $true) 'window shape'
Check (-not (P12-ReplyShape 'p127544_window' 'version=1 valid=1 arm=1000' $true)) 'truncated window'
Check (P12-ReplyShape 'p127543_wait' 'version=1 deadline=123456 plateau=1' $true) 'plateau disposition'
Check (P12-ReplyShape 'p127543_rail' 'version=1 generation=2 count=2 attempts=2 uv0=731250 uv1=731250' $true) 'rail gate evidence'
Check (-not (P12-ReplyShape 'p127543_rail' 'version=1 generation=2 count=3 attempts=2 uv0=731250 uv1=731250' $true)) 'reject extra samples'
Check (P12-ReplyShape 'p127543_assist' '17' $true) 'assist mode'
Check (-not (P12-ReplyShape 'p127543_assist' '81' $true)) 'no unqualified 455 assist'
Check ((P127543-ThermalExcess @{big=62000;lit=60500;gpu=59000}) -eq 2000) 'thermal plateau excess'
Check ((P127543-ThermalExcess @{big=59000;lit=58000;gpu=59000}) -eq -1000) 'within headroom'
Check (P12-ReplyShape 'p127542_audio' '17' $true) 'headless request generation'
Check (P12-ReplyShape 'p127542_audio_result' '-17' $true) 'headless failure generation'
Check (-not (P12-ReplyShape 'p127542_audio_result' '17 extra' $true)) 'reject mixed result'
Check (P12-ReplyShape 'p127542_g2' '1' $true) 'bounded g2 readback'
Check (P12-ReplyShape 'mali_irq' '0' $true) 'IRQ readback'
Check (-not (P12-ReplyShape 'p127542_g2' '2' $true)) 'reject invalid g2 control'
Check (P12-ReplyShape 'p12754_revision' '1' $true) 'legacy revision shape'
$rows=@();$at=100
foreach($i in (@(100..109)+@(0..12))){
    $at+=100
    $rows+= [pscustomobject]@{kind='1';index='0';at="$at";policy='0';driver='0';mask='0';valid='1'}
    $rows+= [pscustomobject]@{kind='2';index="$i";at="$at";valid='0'}
}
$c=P127541-HardwareCoverage $rows
Check ($c.complete -and $c.thermal_rows -eq 23 -and $c.get_valid -eq 0 -and $c.direct_attempted -eq 10) 'complete attempted coverage, zero GET unavailable'
$c=P127541-HardwareCoverage @($rows|Where-Object {-not ($_.kind -eq '2' -and $_.index -eq '104')})
Check (-not $c.complete -and 104 -in $c.missing) 'missing rail attempt refused'
$c=P127541-HardwareCoverage @()
Check (-not $c.complete -and $c.missing.Count -eq 23) 'empty audit not complete'
$rows[0].at='0'
Check (-not (P127541-HardwareCoverage $rows).complete) 'mismatched pair refused'
$ufs='version=1 nufs=1 switched=1 step=5 selftest=1 fences=1 dead=0 loader_core_taken=1 loader_gone=1 floor_safe=1 native_reads=12 loader_reads=0 read_errors=0 writes=4 write_errors=0 refused=0 write_busy=0 native_inflight=0 native_read_calls=12 native_read_wall_us=100 native_write_calls=4 native_write_wall_us=200 write_refused=0'
Check (P12-ReplyShape 'p1231_ufs_stats' $ufs $true) 'typed UFS counters'
Check (-not (P12-ReplyShape 'p1231_ufs_stats' ($ufs -replace ' write_refused=0$','') $true)) 'truncated UFS counters refused'
$a=[pscustomobject]@{rail_verified=$true;buck3_configured_uv=@(762500,762500)}
$b=[pscustomobject]@{rail_verified=$true;buck3_configured_uv=@(731250,731250)}
$c=[pscustomobject]@{rail_verified=$true;buck3_configured_uv=@(762500,762500)}
$comp=P127541-CompareRail $a $b $c
Check ($comp.all_B_below_baseline -and $comp.delta_configured_uv -eq 31250) 'VSEL measured comparison'
$b.buck3_configured_uv=@(731250,762500)
Check (-not (P127541-CompareRail $a $b $c).all_B_below_baseline) 'mixed VSEL cannot claim lower throughout'
$b.rail_verified=$false
Check ($null -eq (P127541-CompareRail $a $b $c)) 'unverified rail no comparison'
Write-Host 'P12754 revision 2 typed stats, coverage, UFS and VSEL comparison PASS (mock only)'
# No serial, no file creation. Exercise headless fail-closed state transitions.
$script:fakeNow=[datetime]'2026-11-27T00:00:00Z'
$script:LifeState='ON'
$script:mockAudioWrites=0;$script:mockResult='-17';$script:mockKey=$null
function Life-Now {return $script:fakeNow}
function Life-Say([string]$Message){}
function Life-Pump {$script:fakeNow=$script:fakeNow.AddSeconds(1)}
function Life-CheckSafety([datetime]$At){}
function P12-EarlyThermalGuard {}
function P127542-ReadKey {return $script:mockKey}
function P12-Query([string]$Name,[string]$Value='',[int]$ReadTimeoutSeconds=12,[switch]$NoReadRetry){
    if($Name -eq 'p1273_gate_status'){
        Check ($ReadTimeoutSeconds -eq 2 -and $NoReadRetry) 'floor read bounded and no retry'
        $script:mockGateReads++
        return "phase=3 generation=$script:mockGateGeneration missing=$script:mockGateMissing"
    }
    if($Name -eq 'p127542_audio' -and $Value -eq '17'){$script:mockAudioWrites++;return $Value}
    if($Name -eq 'p127542_audio_result'){return $script:mockResult}
    throw "unexpected headless query $Name"
}
Refuse {P127542-HeadlessReview '17' 'unused'} 'requires screen OFF'
Check ($script:mockAudioWrites -eq 0) 'no request on screen ON'
$script:LifeState='OFF'
Refuse {P127542-HeadlessReview '17' 'unused'} 'worker failed'
Check ($script:mockAudioWrites -eq 1) 'one request, no rearm'
$script:mockResult='18'
Refuse {P127542-HeadlessReview '17' 'unused'} 'generation mismatch'
$script:mockResult='0';$script:mockKey=[ConsoleKey]::Y
Refuse {P127542-HeadlessReview '17' 'unused'} 'timed out'
$script:mockResult='17';$script:mockKey=[ConsoleKey]::N
Refuse {P127542-HeadlessReview '17' 'unused'} 'operator rejected'
Write-Host 'P127542 headless generation, failed worker, early Y, operator rejection and no-screen gate PASS (mock only)'
# Rev4 floor review does not treat Y alone as kernel evidence.
$script:mockGateMissing='32';$script:mockGateGeneration='17';$script:mockGateReads=0
function P1273-GateStatus {
    $script:mockGateReads++
    return @{phase='3';generation=$script:mockGateGeneration;missing=$script:mockGateMissing}
}
$script:LifeState='OFF';$script:mockKey=[ConsoleKey]::Y
Refuse {P127543-FloorReview '17'} 'screen changed'
$script:LifeState='ON';$script:mockGateMissing='32'
Refuse {P127543-FloorReview '17'} 'fresh kernel evidence'
$script:mockGateMissing='0';$script:mockKey=$null
Refuse {P127543-FloorReview '17'} 'operator confirmation missing'
$script:mockKey=[ConsoleKey]::Y;$script:mockGateGeneration='18'
Refuse {P127543-FloorReview '17'} 'identity changed'
$script:mockGateGeneration='17'
P127543-FloorReview '17'
$script:mockKey=[ConsoleKey]::N
Refuse {P127543-FloorReview '17'} 'operator rejected'
Refuse {P127543-NetworkReview 'WIFI'} 'requires OFF'
$script:LifeState='OFF';Refuse {P127543-NetworkReview 'BT'} 'operator rejected'
$script:mockKey=$null;Refuse {P127543-NetworkReview 'TICKER'} 'timeout'
$script:mockKey=[ConsoleKey]::Y;P127543-NetworkReview 'WIFI'
Check (P12-ReplyShape 'p127543_assist' '1' $true) 'existing BASE15 explicit assist'
Write-Host 'P127543 floor review actual evidence+operator, generation mismatch, network refusal and bounded assist shape PASS (mock only)'
