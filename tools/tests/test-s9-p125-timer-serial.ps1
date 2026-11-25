#requires -Version 5.1
Set-StrictMode -Version 2
$ErrorActionPreference='Stop'
. (Join-Path (Split-Path $PSScriptRoot -Parent) 's9-p125-timer-serial.ps1') -LibraryOnly
foreach($bad in @(@('p125_mode','4'),@('p125_select','2048'),@('p125_counts3','1'),@('p125_page','1'),@('p126_mode','64'),@('p126_status','1'),@('p126_diag','1'))){
    $denied=$false
    try{[void](P12-Query $bad[0] $bad[1])}catch{$denied=$true}
    if(-not $denied){throw 'unsafe P125 write accepted'}
}
$script:fixtureEnd=$false
$script:fixturePage=0
$script:fixtureLegacy=$false
$script:fixturePageReads=0
$script:fixtureMissingUsb=$false
function P12-Query([string]$Name,[string]$Value='',[switch]$Cleanup){
    if($Name -eq 'p125_status'){
        $s='version=1 active=0 armed=0 start=100 end=60100 now=60110 hz=1000 reason=0 down=0 up=0 display_on=0 anchor=fffffff007000000 slots=512 rows=2 counters=28 kinds=15'
        if(-not $script:fixtureLegacy){$s+=' page_lookup=scalar'}
        return $s
    }
    if($Name -match '^p125_counts(\d)$'){
        $cpu=[int]$Matches[1];$a=@('0')*28
        if($script:fixtureEnd -and $cpu -eq 3){$a[0]='a';$a[1]='a';$a[2]='2';$a[3]='8';$a[4]='c'}
        return "version=1 cpu=$cpu counters=$($a -join ',')"
    }
    if($Name -match '^p125_coverage(\d)$'){
        $cpu=[int]$Matches[1];$tot=@('0/0')*14;$pages='0,0,0,0'
        if($script:fixtureEnd -and $cpu -eq 3){$tot[0]='c/0';$pages='1,0,0,0'}
        return "version=1 cpu=$cpu pages=$pages bins=0,0,0,0,0 totals=$($tot -join ',') mapdrop=0"
    }
    if($Name -eq 'p125_select'){$script:fixturePage=[int]$Value;return $Value}
    if($Name -eq 'p125_page'){$script:fixturePageReads++;return "version=1 page=$script:fixturePage r=0/1/fffffff007000000/0/0/c/0/0/0/0/1/20/-"}
    if($Name -match '^p(6|69)_usb_'){
        if($script:fixtureMissingUsb -and $Name -eq 'p6_usb_connected_ticks'){throw "Missing echo and sysctl readback sysctl kern.hakios.$Name"}
        return '0'
    }
    throw "unexpected fixture request $Name"
}
$before=P125-ReadSnapshot 'BASE'
$script:fixtureEnd=$true
$after=P125-ReadSnapshot 'END'
if($after.Rows.Count -ne 1 -or $after.Rows['3/0'].Count -ne 12){throw 'row decode mismatch'}
$stem=Join-Path ([IO.Path]::GetTempPath()) ('p125-test-'+[guid]::NewGuid().ToString('N'))
try{
    $script:P125Stem=$stem
    $script:fixtureLegacy=$true
    $reads=$script:fixturePageReads
    $summary=P125-ReadSnapshot 'RECOVERED' -CountsOnly
    if($summary.Rows.Count -ne 0 -or $summary.Counts[3][4] -ne 12 -or $script:fixturePageReads -ne $reads){throw 'legacy recovery touched pages'}
    if(-not (Test-Path -LiteralPath "$stem.RECOVERED.frozen.clixml")){throw 'missing frozen checkpoint'}
    $failed=$false
    try{[void](P125-ReadSnapshot 'LEGACY')}catch{if($_.Exception.Message -match 'unsafe legacy page resolver'){$failed=$true}else{throw}}
    if(-not $failed -or $script:fixturePageReads -ne $reads){throw 'legacy page lookup was not blocked'}
    $script:fixtureLegacy=$false
    $complete=P125-ReadSnapshot 'FIXED'
    if($complete.Rows.Count -ne 1 -or -not (Test-Path -LiteralPath "$stem.FIXED.complete.clixml")){throw 'fixed page checkpoint missing'}
    $resumeReads=$script:fixturePageReads
    $ResumeSnapshot="$stem.FIXED.frozen.clixml"
    $resumed=P125-ReadSnapshot 'RECOVERED'
    if($resumed.Rows.Count -ne 1 -or $script:fixturePageReads -ne $resumeReads){throw 'resume re-read an already saved page'}
    $bad=Import-Clixml -LiteralPath $ResumeSnapshot
    $bad.Counts[3][4]=13
    $failed=$false
    try{P125-CheckFrozenIdentity $bad $complete}catch{if($_.Exception.Message -match 'counters changed'){$failed=$true}else{throw}}
    if(-not $failed){throw 'changed cumulative counter accepted during resume'}
    $ResumeSnapshot=''
    $Scenario='EnergyTrials';$script:fixtureMissingUsb=$true
    $partial=P125-ReadSnapshot 'USB_MISSING' -CountsOnly
    if($partial.Usb.ContainsKey('p6_usb_connected_ticks') -or $partial.Counts[3][4] -ne 12 -or -not (Test-Path -LiteralPath "$stem.USB_MISSING.scalar.clixml")){throw 'missing USB count invented or scalar checkpoint lost'}
    $Scenario='Trace'
    $failed=$false;try{[void](P125-ReadSnapshot 'STRICT' -CountsOnly)}catch{if($_.Exception.Message -match 'Missing echo'){$failed=$true}else{throw}}
    if(-not $failed){throw 'legacy scenario accepted missing USB count'}
    $script:fixtureMissingUsb=$false
    $script:P125Stem=''
    P125-Delta $before $after 'A0' 2 $stem
    $r=@(Import-Csv "$stem.A0.rows.csv")
    if($r.Count -ne 1 -or $r[0].calls -ne '12'){throw 'wrong callback delta'}
    $after.Counts[3][2]=3
    $failed=$false
    try{P125-Delta $before $after 'BAD' 2 $stem}catch{if($_.Exception.Message -match 'incomplete'){$failed=$true}else{throw}}
    if(-not $failed){throw 'bad queue accounting accepted'}
    $after.Counts[3][2]=2
    $after.Status.down='100';$after.Status.up='60100'
    P125-Delta $before $after 'B_OFFLINE' 3 $stem
    $after.Status.display_on='1'
    $failed=$false
    try{P125-Delta $before $after 'WAKE' 3 $stem}catch{if($_.Exception.Message -match 'incomplete'){$failed=$true}else{throw}}
    if(-not $failed){throw 'wake contamination accepted'}
}finally{
    foreach($f in [IO.Directory]::GetFiles([IO.Path]::GetDirectoryName($stem),([IO.Path]::GetFileName($stem)+'*'))){[IO.File]::Delete($f)}
}
$root=Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$kernel=Get-Content -Raw (Join-Path $root 'drivers/power/exynos/hakios.p125_sysctl.h')
$handler=$kernel.Substring($kernel.IndexOf('p125_page_sysctl('))
if($handler -match '\b(proc_find|proc_task|proc_best_name|proc_rele|proc_ref|thread_block|msleep)\s*\('){throw 'blocking process lookup returned to USB page path'}
if($kernel -notmatch 'page_lookup=scalar'){throw 'scalar ABI marker missing'}
Write-Output 'P12.5 host snapshot, callback accounting, offline bounds, scalar page and fail-closed PASS'
