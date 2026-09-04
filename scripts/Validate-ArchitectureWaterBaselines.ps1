[CmdletBinding()]
param(
    [ValidateSet('d3d12','vulkan')][string[]]$Apis = @('d3d12','vulkan'),
    [ValidateSet('views','quality')][string[]]$Modes = @('views','quality'),
    [ValidateRange(3,5)][int]$Repeats = 3,
    [string]$BinaryPath = 'build-windows-ci/RelWithDebInfo/PrismRender.exe',
    [switch]$Headless,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [switch]$DryRun
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$root=Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'ArchitectureValidation.Common.ps1')
$output=Resolve-ArchitectureOutputPath $root $OutputDirectory
$binary=[IO.Path]::GetFullPath($BinaryPath,$root)
$binaryHash=(Get-FileHash $binary).Hash
if (Test-Path $output) { throw 'Refusing to overwrite water baseline evidence.' }
if ($Apis.Count -eq 0 -or $Modes.Count -eq 0 -or @($Apis | Select-Object -Unique).Count -ne $Apis.Count -or
    @($Modes | Select-Object -Unique).Count -ne $Modes.Count) { throw 'Duplicate API or matrix mode.' }
$cases=[Collections.Generic.List[object]]::new()
foreach ($api in $Apis) {
    if ('views' -in $Modes) {
        foreach ($camera in @('near','horizon','refraction','foam','caustics','underwater','waterline')) {
            $cases.Add(@{ name="$api-view-$camera"; backend=$api; camera=$camera; quality='high'; width=1280; height=800
                surface=$(if($camera -eq 'foam') { 'wake' } else { 'reference' }) })
        }
    }
    if ('quality' -in $Modes) {
        foreach ($quality in @('normal','high','extreme')) {
            foreach ($extent in @(@(1280,800),@(2560,1417))) {
                $cases.Add(@{ name="$api-quality-$quality-$($extent[0])x$($extent[1])"; backend=$api; camera='underwater'
                    quality=$quality; width=$extent[0]; height=$extent[1]; surface='reference' })
            }
        }
    }
}
$record=[ordered]@{ format='PrismArchitectureWaterBaselines'; version=1; status='planned'; cases=$cases.ToArray()
    repeats=$Repeats; sampleFrame=30; queueMode='native'; completed=@(); comparisons=@()
    binarySha256=$binaryHash; headless=[bool]$Headless
    limitation='Fixed-frame visual matrix only, not V3 performance distributions or the separate lifecycle sequence.' }
if ($DryRun) { $record | ConvertTo-Json -Depth 6; return }
New-Item -ItemType Directory -Path $output | Out-Null
try {
    $firstSources=$null
    $record.status='running'
    $record | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $output 'index.json') -Encoding utf8
    foreach ($case in $cases) {
        for ($repeat=1; $repeat -le $Repeats; ++$repeat) {
            $name="$($case.name)-r$repeat"
            Write-Host "Capturing $name"
            $child=Join-Path $output $name
            & (Join-Path $PSScriptRoot 'Capture-ArchitectureSequence.ps1') -Backend $case.backend -Scene hpwater-ocean -Frames 30 -QueueMode native -WaterCamera $case.camera -WaterQuality $case.quality -WaterCaustics rgb -WaterSurface $case.surface -WaterRayMarch -Width $case.width -Height $case.height -BinaryPath $binary -Headless:$Headless -OutputDirectory $child | Out-Null
            $childRun=Get-Content (Join-Path $child 'index.json') -Raw | ConvertFrom-Json
            $actual=Get-Content (Join-Path $child 'captures/frame-30.capture.json') -Raw | ConvertFrom-Json
            if ($childRun.binarySha256 -ne $binaryHash -or
                $actual.source.history.waterOptics.key.quality -ne @{normal=0;high=1;extreme=2}[$case.quality]) {
                throw "Binary or effective water quality changed: $name"
            }
            $sources=Get-Content (Join-Path $child 'source-inputs.json') -Raw | ConvertFrom-Json
            if ($null -eq $firstSources) { $firstSources=$sources }
            elseif (@(Compare-Object $firstSources $sources -Property path,sha256).Count) {
                throw 'Source inputs changed between independent baseline runs.'
            }
            $record.completed += $name
        }
        # All pairs, not just two comparisons against a central image.
        for ($first=1; $first -lt $Repeats; ++$first) {
            for ($second=$first+1; $second -le $Repeats; ++$second) {
                $name="$($case.name)-r$first-r$second"
                & (Join-Path $PSScriptRoot 'Compare-ArchitectureSequences.ps1') -ReferenceDirectory (Join-Path $output "$($case.name)-r$first") -CandidateDirectory (Join-Path $output "$($case.name)-r$second") -OutputDirectory (Join-Path $output "comparisons/$name") | Out-Null
                $record.comparisons += $name
            }
        }
        $record | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $output 'index.json') -Encoding utf8
    }
    $record.status='stable'
} catch { $record.status='failed'; $record['error']=$_.Exception.Message; throw }
finally {
    $record['childIndexes']=@(Get-ChildItem $output -Recurse -Filter index.json -File |
        Where-Object FullName -ne (Join-Path $output 'index.json') |
        ForEach-Object { @{ path=[IO.Path]::GetRelativePath($output,$_.FullName); sha256=(Get-FileHash $_.FullName).Hash } })
    $record | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $output 'index.json') -Encoding utf8
}
Write-Output (Join-Path $output 'index.json')
