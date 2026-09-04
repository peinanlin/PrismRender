[CmdletBinding()]
param(
    [ValidateSet('lifecycle', 'views', 'benchmark', 'details', 'all')][string]$Mode = 'all',
    [ValidateSet('d3d12', 'vulkan')][string[]]$Apis = @('d3d12', 'vulkan'),
    [string]$BinaryPath = '',
    [string]$OutputDirectory = '',
    [string]$WorkingDirectory = '',
    [switch]$NoClobber,
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$artifacts = if ($OutputDirectory) { [IO.Path]::GetFullPath($OutputDirectory, $root) }
    else { Join-Path $root 'artifacts/hpwater-validation' }
$capture = Join-Path $PSScriptRoot 'Capture-HpWater.ps1'
$captureOptions = @{ BinaryPath = $BinaryPath; OutputDirectory = $artifacts; WorkingDirectory = $WorkingDirectory; NoClobber = $NoClobber; DryRun = $DryRun }

# GPU captures are deliberately serial; parallel runs invalidate timing comparisons.
foreach ($api in $Apis) {
    if ($Mode -in @('all', 'lifecycle')) {
        & $capture @captureOptions -Api $api -Camera underwater -Sequence -Validation -QueueMode native -OutputName "accepted_sequence_$api"
    }
    if ($Mode -in @('all', 'views')) {
        foreach ($camera in @('near', 'horizon', 'refraction', 'foam', 'caustics', 'underwater', 'waterline')) {
            $surface = if ($camera -eq 'foam') { 'wake' } else { 'reference' }
            foreach ($scene in @('waveworks-ocean', 'hpwater-ocean')) {
                & $capture @captureOptions -Api $api -Scene $scene -Camera $camera -SurfacePreset $surface `
                    -RayMarch -Caustics rgb -OutputName "compare_${scene}_${api}_$camera"
            }
        }
    }
    if ($Mode -in @('all', 'benchmark')) {
        foreach ($quality in @('normal', 'high', 'extreme')) {
            foreach ($extent in @(@(1280, 800), @(2560, 1417))) {
                $name = "bench_${api}_${quality}_$($extent[0])x$($extent[1])"
                & $capture @captureOptions -Api $api -Camera underwater -Quality $quality -RayMarch -Caustics rgb `
                    -Width $extent[0] -Height $extent[1] -OutputName $name
                if ($DryRun) { continue }
                $report = Get-Content (Join-Path $artifacts "$name.json") -Raw | ConvertFrom-Json
                foreach ($field in @('outputExtent', 'activeViews', 'opticalQuality', 'spectralQuality',
                    'refraction', 'caustics', 'volumetrics', 'medium', 'history', 'distanceMeters',
                    'waterMemoryMiB', 'waterCoverage', 'waterDraws', 'waterDispatches', 'cpuStages')) {
                    if ($null -eq $report.capture.$field) { throw "Missing benchmark field: $field ($name)" }
                }
                if ($report.capture.outputExtent[0] -ne $extent[0] -or
                    $report.capture.outputExtent[1] -ne $extent[1] -or
                    -not $report.capture.waterCoverage.available -or $report.passes.Count -eq 0) {
                    throw "Incomplete benchmark report: $name"
                }
            }
        }
    }
    if ($Mode -eq 'details') {
        & $capture @captureOptions -Api $api -Camera caustics -RayMarch -Caustics rgb -DebugView caustic-energy `
            -OutputName "detail_${api}_caustic_energy"
        & $capture @captureOptions -Api $api -Camera horizon -RayMarch -Caustics rgb -DebugView caustic-cascades `
            -OutputName "detail_${api}_caustic_cascades"
        & $capture @captureOptions -Api $api -Camera underwater -RayMarch -Caustics rgb -DisableVolumetrics `
            -OutputName "detail_${api}_volume_off"
        & $capture @captureOptions -Api $api -Camera underwater -RayMarch -Caustics rgb -DebugView volumetric-history `
            -OutputName "detail_${api}_volume_history"
        foreach ($frame in @(100, 112, 121)) {
            & $capture @captureOptions -Api $api -Camera underwater -Sequence -Frames $frame -Validation -QueueMode native `
                -OutputName "detail_${api}_motion_$frame"
        }
        foreach ($surface in @('reference', 'calm', 'wake', 'strong-wind')) {
            & $capture @captureOptions -Api $api -Camera near -SurfacePreset $surface -Frames 180 -DebugView foam `
                -OutputName "detail_${api}_foam_$surface"
        }
    }
}
