[CmdletBinding()]
param(
    [ValidateSet('d3d12','vulkan')][string]$Backend = 'd3d12',
    [ValidateRange(2,3)][int]$Repeats = 2,
    [string[]]$Scenes = @(),
    [string]$BinaryPath = 'build-windows-ci/RelWithDebInfo/PrismRender.exe',
    [string]$CatalogPath = 'scripts/ArchitectureDemoSequences.json',
    [Parameter(Mandatory)][string]$OutputDirectory,
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'ArchitectureValidation.Common.ps1')
. (Join-Path $PSScriptRoot 'ArchitectureSequences.Common.ps1')
. (Join-Path $PSScriptRoot 'ArchitectureDemoSequences.Common.ps1')
$catalogFile = [IO.Path]::GetFullPath($CatalogPath, $root)
$catalog = Read-ArchitectureDemoSequenceCatalog $root $catalogFile
if ($Scenes.Count) {
    foreach ($scene in $Scenes) { if ($scene -notin $catalog.cases.scene) { throw "Unknown Demo sequence scene: $scene" } }
    $cases = @($catalog.cases | Where-Object { $_.scene -in $Scenes })
} else { $cases = @($catalog.cases) }
$output = Resolve-ArchitectureOutputPath $root $OutputDirectory
if (Test-Path -LiteralPath $output) { throw "Refusing to overwrite Demo sequence matrix: $output" }
$binary = [IO.Path]::GetFullPath($BinaryPath, $root)
if (-not (Test-Path -LiteralPath $binary -PathType Leaf)) { throw "Missing renderer: $binary" }
$record = [ordered]@{ format='PrismArchitectureDemoSequenceMatrix';version=1;status='planned';backend=$Backend
    repeats=$Repeats;catalog=$catalogFile;catalogSha256=(Get-FileHash $catalogFile).Hash
    binary=$binary;binarySha256=(Get-FileHash $binary).Hash;scenes=@($cases.scene);runs=@();comparisons=@()
    limitations=@('Capture readback waits are not performance evidence.','Each repeat is an independent process with a continuous 1..endFrame trace.',
        'Same-backend V2 image/structure comparison is required between repeats; cross-backend parity is separate.') }
if ($DryRun) {
    $record['plans'] = @($cases | ForEach-Object { @{scene=$_.scene;frames=$_.frames;endFrame=$_.endFrame
        view=$_.view;actions=$_.actions;assetStreaming=$_.assetStreaming;waterLifecycle=$_.waterLifecycle;tags=$_.tags} })
    $record | ConvertTo-Json -Depth 10
    return
}
New-Item -ItemType Directory -Path $output | Out-Null
$indexPath = Join-Path $output 'index.json'
try {
    $record.status = 'running'
    $record | ConvertTo-Json -Depth 10 | Set-Content $indexPath -Encoding utf8
    foreach ($case in $cases) {
        $runDirectories = @()
        for ($repeat = 1; $repeat -le $Repeats; ++$repeat) {
            $child = Join-Path $output "$($case.scene)-r$repeat"
            $options = @{Backend=$Backend;Scene=$case.scene;Frames=[long[]]$case.frames;EndFrame=[long]$case.endFrame
                View=$case.view;BinaryPath=$binary;OutputDirectory=$child
                ActionsJson=(@($case.actions) | ConvertTo-Json -Depth 6 -Compress)}
            if ($case.assetStreaming) { $options.AssetStreaming = $true }
            if ($case.waterLifecycle) { $options.WaterLifecycle = $true }
            & (Join-Path $PSScriptRoot 'Capture-ArchitectureSequence.ps1') @options | Out-Null
            $run = Read-ArchitectureSequenceRun $root $child
            if ($run.run.binarySha256 -ne $record.binarySha256) { throw 'Demo sequence binary changed between runs.' }
            $runDirectories += $child
            $record.runs += @{scene=$case.scene;repeat=$repeat;directory=$child
                indexSha256=(Get-FileHash (Join-Path $child 'index.json')).Hash}
            $record | ConvertTo-Json -Depth 10 | Set-Content $indexPath -Encoding utf8
        }
        for ($repeat = 2; $repeat -le $Repeats; ++$repeat) {
            $comparison = Join-Path $output "comparisons/$($case.scene)-r1-r$repeat"
            & (Join-Path $PSScriptRoot 'Compare-ArchitectureSequences.ps1') -ReferenceDirectory $runDirectories[0] `
                -CandidateDirectory $runDirectories[$repeat - 1] -OutputDirectory $comparison | Out-Null
            $record.comparisons += @{scene=$case.scene;reference=1;candidate=$repeat;directory=$comparison
                indexSha256=(Get-FileHash (Join-Path $comparison 'index.json')).Hash}
        }
    }
    $record.status = 'passed'
} catch {
    $record.status = 'failed'; $record['error'] = $_.Exception.Message; throw
} finally {
    $record['artifacts'] = @(Get-ChildItem $output -Recurse -File | Where-Object FullName -ne $indexPath |
        ForEach-Object { @{path=[IO.Path]::GetRelativePath($output,$_.FullName);sha256=(Get-FileHash $_.FullName).Hash} })
    $record | ConvertTo-Json -Depth 10 | Set-Content $indexPath -Encoding utf8
}
Write-Output $indexPath
