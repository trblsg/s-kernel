#requires -Version 5.1
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../s9-p125-timer-serial.ps1') -Scenario ThermalTraceDump -LibraryOnly
function Check($ok,[string]$why){if(-not $ok){throw $why}}
Check $script:P1273HostEnabled 'P1273 scenario not enabled'
$status='version=1 state=4 generation=1 start=100 end=1100 until=1100 trigger=500 span=250 reason=7 buckets=128 owners=16'
$page='version=1 select=0 generation=1 present=1 bin=0 mask=1 dropped=0 dropped_ticks=0 r=10/20/30/2/40/50/1/1'
Check (P12-ReplyShape 'p1273_trace_status' $status $true) 'status'
Check (P12-ReplyShape 'p1273_trace_page' $page $true) 'page'
Check (-not (P12-ReplyShape 'p1273_trace_page' ($page+' garbage') $true)) 'partial page accepted'
Check (-not (P12-ReplyShape 'p1273_trace_cpu1' 'version=1 cpu=0 generation=1 mask=1/0 overhead=0 clipped=0' $true)) 'wrong cpu'
Check (P12-ReplyShape 'p1273_bus6' 'version=1 index=2 seq=1 sampled=1 errno=-5 raw=0 valid=0 zero=0 at=10 age_ms=20 latency_us=30' $true) 'bus error'
Check (-not (P12-ReplyShape 'p1273_bus6' 'version=1 index=3 seq=1 sampled=1 errno=0 raw=0 valid=0 zero=1 at=10 age_ms=20 latency_us=30' $true)) 'wrong bus domain'
$bins=(('0/0,'*8)+'1/2')
Check (P12-ReplyShape 'p1273_wfi7' "version=2 cpu=7 sgi=0 ppi=0 spi=0 bins=$bins" $true) 'wfi9'
$gpu='version=1 slot=0 seq=1 generation=1 job=10 tid=20 task=30 pid=40 owner=1 fragment=0 dropped=0 status=0 queued=100 began=200 ended=300 wake_begin=210 wake_end=250 wake_attempt=1 wake_ok=1'
Check (P12-ReplyShape 'p1273_gpu_page' $gpu $true) 'gpu row'
Check (-not (P12-ReplyShape 'p1273_gpu_page' ($gpu+' extra') $true)) 'gpu suffix'
Check (P12-ReplyShape 'p1273_gov_page' 'version=1 slot=0 present=1 bin=0 at=100 big_load=60 lit_load=20 big_khz=1469000 lit_khz=1053000 big_thermal=0 lit_thermal=0 tmu_age=200 big_mc=65000 lit_mc=59000 gpu_mc=-273000' $true) 'gov row'
Check (P12-ReplyShape 'p1273_psr' 'version=1 ready=1 boot_ms=300 owner=2 governor=1 drained=1 legacy_ticks=20 legacy_mask=1 legacy_unmask=1 faults=0 last_frame=100' $true) 'psr row'
Check (P12-ReplyShape 'p1273_offline_status' 'version=1 fixed=0 duration=600 deadline=800 policy_reason=10 capture_reason=10' $true) 'offline status'
Check (P12-ReplyShape 'p1273_cap_status' 'version=1 state=4 mode=1 generation=1 reason=1 start=100 end=250 until=250 windows=20 big_cap=1469000 lit_cap=1248000 big_hits=10 lit_hits=10 big_peak=65000 lit_peak=59000 gpu_peak=60000 big_max=1794000 lit_max=1248000 big_asked=1 lit_asked=0 measured_ok=2 measured_bad=0 mild_done=1' $true) 'cap status'
$rows=@(0,10,20,30,40,50|ForEach-Object {[pscustomobject]@{seconds_from_start=$_;net_battery_w=-2.0}})
$e=P1273-OfflineIntegrate $rows 'MOCK' 5 45
Check ([Math]::Abs($e.MeanW-2.0) -lt 0.00001 -and $e.Seconds -eq 40) 'common interval integration'
$badSupport=$false
try{[void](P1273-OfflineIntegrate $rows 'MOCK' 10 20)}catch{$badSupport=$true}
Check $badSupport 'short support accepted'
foreach($bad in @(@('p1273_floor','1'),@('p1273_offline_s','120'),@('p1273_cap_mode','4'),@('p1273_gpu_select','1025'),@('p1273_gov_select','128'),@('p1273_trace_mode','2'),@('p1273_trace_select','8192'),@('p127_mode','903'),@('p127_mode','919'))){
    $refused=$false
    try{[void](P12-Query $bad[0] $bad[1])}catch{if($_.Exception.Message -match 'read-only|only permits'){$refused=$true}else{throw}}
    Check $refused "unsafe write $bad"
}
$script:mockSel=-1
function P12-Query([string]$Name,[string]$Value='',[switch]$Cleanup){
    switch -Regex ($Name){
        '^p1273_trace_status$' {return $status}
        '^p1273_trace_cpu([0-7])$' {
            $cpu=[int]$Matches[1]
            $mask=if($cpu -eq 0){'1/0'}else{'0/0'}
            return "version=1 cpu=$cpu generation=1 mask=$mask overhead=0 clipped=0"
        }
        '^p1273_trace_select$' {$script:mockSel=[int]$Value;return $Value}
        '^p1273_trace_page$' {Check ($script:mockSel -eq 0) 'unexpected select';return $page}
        default {throw "unexpected query $Name"}
    }
}
$stem=Join-Path $env:TEMP ('p1273-test-'+[guid]::NewGuid())
try{
    P1273-TraceDump $stem
    Check (@(Get-Content "$stem.thermal-pages.txt").Count -eq 9) 'frozen dump coverage'
    Check ($script:P125Report[-1] -match 'COMPLETE.*pages=1') 'completion missing'
}finally{Remove-Item "$stem.thermal-pages.txt","$stem.thermal-status.clixml" -ErrorAction SilentlyContinue}
Write-Host 'P12.7.3 typed ABI, write gate, frozen dump PASS (mock, no device)'
