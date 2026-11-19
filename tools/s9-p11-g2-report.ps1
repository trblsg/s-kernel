#requires -version 5.1
# Read-only S9 P1.1/G2 fast pass report.
# powershell -NoProfile -ExecutionPolicy Bypass -File tools/s9-p11-g2-report.ps1 -Log putty-P11G2-455.log
param([Parameter(Mandatory=$true)][string]$Log)
$ErrorActionPreference='Stop'
$r=[ordered]@{log=$Log;boot='';positiveP1=0;P11enter=0;P11return=0;P11commit=0;P11finish=0;P11dealloc=0;fault=0;missingLayout=0;prideSegv=0;prideAbort=0;g2=@();g1=@();g1top=@();prideEvents=@()}
$g2=[Collections.Generic.List[object]]::new()
$g1=[Collections.Generic.List[object]]::new()
$pride=[Collections.Generic.List[string]]::new()
foreach($s in [IO.File]::ReadAllLines((Resolve-Path -LiteralPath $Log).Path)){
    if(!$r.boot -and $s -match 'boot-args = (.*)'){$r.boot=$Matches[1]}
    if($s -match 'P1 userDictionary SPI: created='){$r.positiveP1++}
    if($s -match '^.*native-metal-p11: (userdict-enter|userdict-return|command-commit|command-finish|command-dealloc)'){
        switch($Matches[1]){
            'userdict-enter'{$r.P11enter++}
            'userdict-return'{$r.P11return++}
            'command-commit'{$r.P11commit++}
            'command-finish'{$r.P11finish++}
            'command-dealloc'{$r.P11dealloc++}
        }
    }
    if($s -match 'hakios-gl-g2: BINDING MISMATCH'){$r.fault++}
    if($s -match 'PosterBoard\[\d+:\d+\] native-metal: newTextureLayoutWithDescriptor:isHeapOrBufferBacked: is not implemented'){$r.missingLayout++}
    if($s -match 'PridePoster\.PridePosterExtension.*exited due to SIGSEGV'){$r.prideSegv++;$pride.Add($s)}
    if($s -match 'PridePoster\.PridePosterExtension.*exited due to SIGABRT'){$r.prideAbort++;$pride.Add($s)}
    if($s -match '^hakios-gl-g2: up=([\d.]+)s frames=(\d+)\.\.(\d+) attempted=(\d+) skipped=(\d+) driver=(\d+) invalidated=(\d+) fault=(\d+) verify=(\d+)'){
        $g2.Add([pscustomobject]@{up=[double]::Parse($Matches[1],[Globalization.CultureInfo]::InvariantCulture);attempt=[int]$Matches[4];skip=[int]$Matches[5];native=[int]$Matches[6];invalidate=[int]$Matches[7];fault=[int]$Matches[8]});continue
    }
    if($s -match '^hakios-gl-cost: up=([\d.]+)s frames=.* calls=(\d+) measured=(\d+)us .*texture=(\d+)/(\d+)'){
        $g1.Add([pscustomobject]@{up=[double]::Parse($Matches[1],[Globalization.CultureInfo]::InvariantCulture);calls=[int]$Matches[2];us=[long]$Matches[3];textureUs=[long]$Matches[4];textureCalls=[int]$Matches[5]});continue
    }
}
$r.g2=$g2.ToArray();$r.g1=$g1.ToArray();$r.prideEvents=$pride.ToArray()
Write-Output ('LOG '+$r.log)
Write-Output ('P1 marker='+$r.positiveP1+' P11 enter='+$r.P11enter+' return='+$r.P11return+' commit='+$r.P11commit+' finish='+$r.P11finish+' dealloc='+$r.P11dealloc)
Write-Output ('Pride SIGSEGV='+$r.prideSegv+' SIGABRT='+$r.prideAbort+' TextureLayout unimplemented='+$r.missingLayout)
if(!$r.P11enter){Write-Warning 'P1.1 tracer missing or never exercised; cannot infer crash route'}
if(!$r.g2.Count){Write-Warning 'G2 missing: check boot flag g2_bind=1 and correct GLES userland image'}
$windows=@(@(120,270),@(295,340),@(420,600),@(600,900),@(900,1200))
foreach($w in $windows){
    $g=@($r.g2|Where-Object{$_.up -ge $w[0] -and $_.up -lt $w[1]})
    $c=@($r.g1|Where-Object{$_.up -ge $w[0] -and $_.up -lt $w[1]})
    $attempt=($g|Measure-Object attempt -Sum).Sum
    $skipped=($g|Measure-Object skip -Sum).Sum
    $native=($g|Measure-Object native -Sum).Sum
    $invalid=($g|Measure-Object invalidate -Sum).Sum
    $texture=($c|Measure-Object textureUs -Sum).Sum
    $textureCalls=($c|Measure-Object textureCalls -Sum).Sum
    $ratio=if($attempt){[math]::Round(100*$skipped/$attempt,1)}else{0}
    $perCall=if($textureCalls){[math]::Round($texture/$textureCalls,1)}else{0}
    Write-Output ("MACH {0}-{1}s G2 samples={2} attempted={3} skipped={4} ({5}%) native={6} invalid={7} G1texture={8}us/{9} calls ({10}us/call)" -f $w[0],$w[1],$g.Count,$attempt,$skipped,$ratio,$native,$invalid,$texture,$textureCalls,$perCall)
}
if($r.fault){Write-Warning ("BINDING MISMATCH count="+$r.fault+" : rollback G2")}
if($r.prideSegv){Write-Warning 'PridePoster still SIGSEGV. Need on-device .ips crash report with PC/FAR/stack, not additional blind Metal changes.'}
Write-Output 'Note: G2 counts include wrapper-based context state; G1 times are CPU wall, not GPU power.'
