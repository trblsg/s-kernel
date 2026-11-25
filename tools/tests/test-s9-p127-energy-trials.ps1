#requires -Version 5.1
Set-StrictMode -Version 2
$ErrorActionPreference='Stop'
. (Join-Path (Split-Path $PSScriptRoot -Parent) 's9-p125-timer-serial.ps1') -LibraryOnly
$script:m=0;$script:seq=0;$script:hit=0;$script:revoked=$false
$script:writes=New-Object 'System.Collections.Generic.List[string]'
$script:phases=New-Object 'System.Collections.Generic.List[string]'
function P126-Status{return @{mode='0'}}
function P127-Status{return @{version='1';mode="$script:m";reason=$(if($script:revoked){'4'}else{'0'});lease_s='420';little_request='0';little_cur='1053000';ufs_safe='1';ufs_busy='0';floor_hits='0';floor_bypass='0'}}
function P12-Query([string]$Name,[string]$Value='',[switch]$Cleanup){
    switch -Regex ($Name){
        '^p127_mode$' {$script:m=[int]$Value;$script:writes.Add($Value);return $Value}
        '^p127_wfi([0-7])$' {return "version=1 cpu=$($Matches[1]) sgi=0 ppi=0 spi=0 bins=0/0,0/0,0/0,0/0,0/0,0/0,0/0"}
        '^p127_fg_seq$' {return "version=1 seq=$script:seq slots=64"}
        '^p127_fg0$' {return 'version=1 page=0 r=1/100/-450000/-460000/4000000/0/0/12 r=2/200/-400000/-420000/4000000/1/1/13'}
        '^p127_metrics$' {return "version=1 usb_8ms=$script:hit abox_fault=0 psr_fault=0 abox_stop=0 abox_resume=0 abox_idle_wait=0 psr_mask=0 psr_unmask=0"}
        '^p127_wifi$' {return 'version=1 attach=0 detach=0 fallback=0'}
        '^p127_bt$' {return 'version=1 attach=0 detach=0 fallback=0'}
        '^p127_ticker$' {return 'version=1 wait=0'}
        '^(p127_bus|p127_shub|p126_diag)$' {return 'version=1'}
        default {throw "unexpected command $Name"}
    }
}
function P125-PumpOff([int]$Seconds){}
function P125-ReadSnapshot([string]$Label,[switch]$CountsOnly){
    if(-not $CountsOnly){throw 'detail dump forbidden'}
    return @{label=$Label}
}
function P125-Phase([string]$Label,[int]$Mode,[int]$Seconds,[string]$Stem,$Before,[switch]$CountsOnly,[switch]$OfflineAggregate){
    if(-not $CountsOnly -or ($Mode -eq 3 -and -not $OfflineAggregate) -or ($Mode -ne 3 -and $OfflineAggregate)){throw 'incorrect observation mode'}
    $script:phases.Add("$Label/$Mode")
    if($script:m -eq 3){$script:hit+=100}
    if($script:revoked){$script:m=0}
    return @{Status=@{hz='100';start='0';end='3000'}}
}
$dir=Join-Path ([IO.Path]::GetTempPath()) ('p127-test-'+[guid]::NewGuid())
[void](New-Item -ItemType Directory -Path $dir)
try{
    $stem=Join-Path $dir 'test'
    $Scenario='EnergyTrials';$EnergyModules='USB,ABOX';$EnergySeconds=30
    P127-RunEnergy $stem
    if(($script:writes -join ',') -ne '1,0,3,0,1,0,5,0,1,0'){throw 'A/B/A lease sequence'}
    if($script:P127Dirty -or $script:m){throw 'restore not verified'}
    if(-not ($script:P125Report -match 'MODULE USB APPLIED') -or -not ($script:P125Report -match 'MODULE ABOX NO_ELIGIBLE')){throw 'application evidence'}
    $rows=@(Import-Csv "$stem.P127-A0.wfi.csv")
    if($rows.Count -ne 56){throw 'WFI shape'}
    $pre=P127-Read;$script:seq=2;$post=P127-Read
    $post.Wfi[3].Bins[5].N=2;$post.Wfi[3].Bins[5].Ticks=1
    P127-Delta $pre $post @{hz='100';start='0';end='3000'} 'FG' $stem
    $fg=@(Import-Csv "$stem.FG.fg.csv")
    if($fg.Count -ne 2 -or [double]$fg[0].net_battery_w -ne -1.8 -or $fg[1].external -ne '1'){throw 'signed battery data'}
    $Scenario='EnergyOffline';$EnergySeconds=60
    P127-RunEnergy $stem
    if(-not ($script:phases -match 'OFFLINE-B/3')){throw 'offline aggregate phase missing'}
    $Scenario='EnergyTrials';$EnergyModules='LITTLE455';$EnergySeconds=30
    $failed=$false;try{P127-RunEnergy $stem}catch{if($_.Exception.Message -match 'prior step review'){$failed=$true}else{throw}}
    if(-not $failed){throw 'unsafe lower floor accepted'}
    $EnergyModules='USB';$script:revoked=$true
    $failed=$false;try{P127-RunEnergy $stem}catch{if($_.Exception.Message -match 'lease revoked'){$failed=$true}else{throw}}
    if(-not $failed -or -not $script:P127Dirty){throw 'revocation cleanup'}
    Write-Output 'P12.7 A/B/A, offline aggregate, 56 WFI bins, signed gauge, lower-floor and revocation PASS'
}finally{Remove-Item -LiteralPath $dir -Recurse -Force}
