#requires -Version 5.1
Set-StrictMode -Version 2
$ErrorActionPreference='Stop'
. (Join-Path (Split-Path $PSScriptRoot -Parent) 's9-p125-timer-serial.ps1') -LibraryOnly
foreach($bad in @(@('p125_mode','4'),@('p125_select','2048'),@('p125_counts3','1'),@('p125_page','1'))){
    $denied=$false
    try{[void](P12-Query $bad[0] $bad[1])}catch{$denied=$true}
    if(-not $denied){throw 'unsafe P125 write accepted'}
}
$script:fixtureEnd=$false
$script:fixturePage=0
function P12-Query([string]$Name,[string]$Value='',[switch]$Cleanup){
    if($Name -eq 'p125_status'){return 'version=1 active=0 armed=0 start=100 end=60100 now=60110 hz=1000 reason=0 down=0 up=0 display_on=0 anchor=fffffff007000000 slots=512 rows=2 counters=28 kinds=15'}
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
    if($Name -eq 'p125_page'){return "version=1 page=$script:fixturePage r=0/1/fffffff007000000/0/0/c/0/0/0/0/1/20/-"}
    if($Name -match '^p(6|69)_usb_'){return '0'}
    throw "unexpected fixture request $Name"
}
$before=P125-ReadSnapshot 'BASE'
$script:fixtureEnd=$true
$after=P125-ReadSnapshot 'END'
if($after.Rows.Count -ne 1 -or $after.Rows['3/0'].Count -ne 12){throw 'row decode mismatch'}
$stem=Join-Path ([IO.Path]::GetTempPath()) ('p125-test-'+[guid]::NewGuid().ToString('N'))
try{
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
Write-Output 'P12.5 host snapshot, callback accounting, offline bounds and fail-closed PASS'
