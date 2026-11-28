# Offline aggregation only. No serial I/O, policy writes, or inference from
# an automatic driver witness. Manifest paths are relative to the manifest.
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$Manifest)
Set-StrictMode -Version 2
$ErrorActionPreference='Stop'
$manifestPath=(Resolve-Path -LiteralPath $Manifest).Path
$root=Split-Path -Parent $manifestPath
$inputData=Get-Content -Raw -LiteralPath $manifestPath|ConvertFrom-Json
if($inputData.version -ne 1 -or -not $inputData.trials){throw 'Expected manifest version=1 with trials array'}
function Read-Report([string]$Path){
    if(-not $Path){throw 'Missing report path'}
    if(-not [IO.Path]::IsPathRooted($Path)){$Path=Join-Path $root $Path}
    $resolved=(Resolve-Path -LiteralPath $Path).Path
    [pscustomobject]@{path=$resolved;value=(Get-Content -Raw -LiteralPath $resolved|ConvertFrom-Json)}
}
# Import just the existing comparator and helpers; no COM connection.
. (Join-Path $PSScriptRoot 's9-p125-timer-serial.ps1') -LibraryOnly -P12754
$results=@()
foreach($trial in $inputData.trials){
    $paths=@()
    try{
        $reports=@(Read-Report $trial.A0;Read-Report $trial.B;Read-Report $trial.A1)
        $paths=@($reports|ForEach-Object {$_.path})
        if(@($paths|Select-Object -Unique).Count -ne 3){throw 'A0/B/A1 must be three distinct reports'}
        foreach($r in $reports){
            foreach($flag in @('power_verified','applied','restored')){
                if($r.value.$flag -isnot [bool] -or -not $r.value.$flag){throw "Missing/false typed $flag"}
            }
            if([string]::IsNullOrWhiteSpace([string]$r.value.boot)){throw 'Missing boot identity'}
        }
        $review=if($trial.PSObject.Properties['qch_review'] -and $trial.qch_review){(Read-Report $trial.qch_review).value}else{$null}
        $disp=if($trial.PSObject.Properties['disp_review'] -and $trial.disp_review){(Read-Report $trial.disp_review).value}else{$null}
        $comparison=P12753-Compare $reports[0].value $reports[1].value $reports[2].value $review $disp
        $results+= [pscustomobject]@{accepted=$true;boot=$reports[1].value.boot;reports=$paths;comparison=$comparison}
    }catch{
        $results+= [pscustomobject]@{accepted=$false;reports=$paths;reason=$_.Exception.Message;comparison=$null}
    }
}
[pscustomobject]@{version=1;trials=$results;accepted_count=@($results|Where-Object accepted).Count;rejected_count=@($results|Where-Object {-not $_.accepted}).Count;note='Each A/B/A remains independent; no additive savings or automatic-policy power certification.'}|ConvertTo-Json -Depth 12
