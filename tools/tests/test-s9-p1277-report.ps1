#requires -Version 5.1
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../s9-p125-timer-serial.ps1') -LibraryOnly -P12754
function Check($ok,[string]$why){if(-not $ok){throw $why}}
$fixtures=@{
 p1277_auto_stats='version=1 permission=1 enabled=1 state=4 stage=5 samples=2 retries=0 abox_driver_ok=1 fault=0 last_uv=637500 last_mhz=598 last_reason=0 own_lease=12345 live_mode=69 ufs_safe=1 interactive_qualification=0'
 p1277_layer2_stats='version=1 request=0 module=1 state=0 result=1 samples=20 restored=1 power_qualified=0'
 p1277_burst_stats='version=1 request=0 owned=1 trace_state=4 observed_trigger=100 trace_generation=1 threshold=150 dwell_ms=250 pretrigger=0'
 p1277_panel_stats='version=1 permission=1 request=1 persistent=1 failed=0 asleep=1 pending=0 sleep=1 latch29=0 fallback=0 fault=0 sleep_verified=1 wake_uncertain=0'
 p1277_gpu_dvs_stats='version=1 permission=1 active=0 failed=0 high=1 rebase=1 last_configured_vsel=104 fault=0'
 p1277_psci_stats='version=1 permission=1 done=1 raw_version=0x00010000 raw_features=0xffffffff signed_features=-1 osi=0 extended=0 conduit=SMC_EL2_possible suspend_called=0'
}
foreach($key in $fixtures.Keys){Check (P12-ReplyShape $key $fixtures[$key] $true) "valid $key";Check (-not (P12-ReplyShape $key ($fixtures[$key]+' extra=0') $true)) "bad suffix $key"}
Check (P12-ReplyShape 'p1277_revision' '7' $false) 'Rev7 parser'
Check (-not (P12-ReplyShape 'p1277_revision' '6' $false)) 'Rev6 rejected'
$dir=Join-Path ([IO.Path]::GetTempPath()) ('p1277-test-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $dir|Out-Null
try{
 function New-R($module,$start,$w){[pscustomobject]@{version=1;power_verified=$true;applied=$true;restored=$true;diagnostic=$false;module=$module;start=$start;end=$start+60000;boot='same';mean_discharge_w=$w;window_s=60;start_big_mc=62000;start_lit_mc=60500;mean_voltage_uv=4000000}}
 $a=New-R 'STOCK60' 100000 2.2;$b=New-R 'BASE15' 200000 1.9;$c=New-R 'STOCK60' 300000 2.1
 $a|ConvertTo-Json|Set-Content (Join-Path $dir a.json)
 $b|ConvertTo-Json|Set-Content (Join-Path $dir b.json)
 $c|ConvertTo-Json|Set-Content (Join-Path $dir c.json)
 $c.boot='other';$c|ConvertTo-Json|Set-Content (Join-Path $dir bad.json)
 @{version=1;trials=@(@{A0='a.json';B='b.json';A1='c.json'},@{A0='a.json';B='b.json';A1='bad.json'},@{A0='a.json';B='b.json';A1='a.json'})}|ConvertTo-Json -Depth 5|Set-Content (Join-Path $dir manifest.json)
 $r=& (Join-Path $PSScriptRoot '../s9-p1277-report.ps1') -Manifest (Join-Path $dir manifest.json)|ConvertFrom-Json
 Check ($r.accepted_count -eq 1 -and $r.rejected_count -eq 2) 'aggregate counts'
 Check ([Math]::Abs($r.trials[0].comparison.delta_mW-250) -lt 0.01) 'independent ABA delta'
}finally{Remove-Item -LiteralPath $dir -Recurse -Force}
Write-Output 'P1277 strict status shapes, offline aggregate, wrong boot and duplicate reports PASS (mock)'
