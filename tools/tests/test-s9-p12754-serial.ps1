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
