[CmdletBinding()]
param(
    [switch]$EnforceParity,
    [string]$OutputDirectory = '',
    [string]$CompareBinaryPath = '',
    [switch]$NoClobber
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$output = if ($OutputDirectory) { [IO.Path]::GetFullPath($OutputDirectory, $root) }
    else { Join-Path $root 'artifacts/hpwater-validation' }
$compare = if ($CompareBinaryPath) { [IO.Path]::GetFullPath($CompareBinaryPath, $root) }
    else { Join-Path $root 'build-windows-ci/RelWithDebInfo/PrismGoldenImageCompare.exe' }
if (-not (Test-Path -LiteralPath $compare -PathType Leaf)) { throw "Missing image comparator: $compare" }
if ($NoClobber) {
    foreach ($name in @('acceptance-summary.json') + @('near', 'horizon', 'refraction', 'foam', 'caustics', 'underwater', 'waterline' | ForEach-Object { "parity_$_.txt" })) {
        if (Test-Path -LiteralPath (Join-Path $output $name)) { throw "Summary evidence already exists: $name" }
    }
}
$benchmarks = @()
foreach ($api in @('d3d12', 'vulkan')) {
    foreach ($quality in @('normal', 'high', 'extreme')) {
        foreach ($extent in @('1280x800', '2560x1417')) {
            $name = "bench_${api}_${quality}_$extent"
            $report = Get-Content (Join-Path $output "$name.json") -Raw | ConvertFrom-Json
            $stages = @($report.passes | Where-Object { $_.name.StartsWith('WaterOptics.') })
            $benchmarks += [ordered]@{
                name = $name; api = $api; quality = $quality; extent = $extent
                waterGpuMilliseconds = ($stages | Measure-Object -Property gpuMilliseconds -Sum).Sum
                refractionMilliseconds = ($stages | Where-Object { $_.name -like '*Refraction*' } | Measure-Object gpuMilliseconds -Sum).Sum
                volumeMilliseconds = ($stages | Where-Object { $_.name -like '*Volume*' } | Measure-Object gpuMilliseconds -Sum).Sum
                poolMiB = $report.capture.waterMemoryMiB; coverage = $report.capture.waterCoverage.fraction
                activeViews = $report.capture.activeViews; samples = $report.capture.refraction.effectiveSamples
                volumeExtent = $report.capture.volumetrics.extent; causticsMode = $report.capture.caustics.mode
            }
        }
    }
}
$parity = @()
foreach ($view in @('near', 'horizon', 'refraction', 'foam', 'caustics', 'underwater', 'waterline')) {
    $reference = Join-Path $output "compare_hpwater-ocean_d3d12_$view.bmp"
    $candidate = Join-Path $output "compare_hpwater-ocean_vulkan_$view.bmp"
    $arguments = @($reference, $candidate, "--report=$(Join-Path $output "parity_$view.txt")",
        '--mean=0.03', '--rmse=0.10', '--changed=0.25', '--ssim=0.90', '--pixel-tolerance=8')
    if ($EnforceParity) { $arguments += '--enforce' }
    $metrics = & $compare @arguments
    $passed = $LASTEXITCODE -eq 0
    $entry = [ordered]@{ view = $view; passed = $passed }
    foreach ($line in $metrics) {
        $parts = $line -split '=', 2
        if ($parts.Count -eq 2) { $entry[$parts[0]] = $parts[1] }
    }
    $parity += $entry
}
$summary = [ordered]@{
    format = 'PrismWaterAcceptanceSummary'; version = 1
    timingPolicy = 'Latest completed GPU frame after deterministic warm-up; not an average or FPS'
    benchmarks = $benchmarks; backendParity = $parity
}
$summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $output 'acceptance-summary.json') -Encoding utf8
$benchmarks | ForEach-Object { [pscustomobject]$_ } | Format-Table api,quality,extent,waterGpuMilliseconds,poolMiB,coverage
$parity | ForEach-Object { [pscustomobject]$_ } | Format-Table view,passed,mean_absolute_error,structural_similarity
if ($EnforceParity -and ($parity | Where-Object { -not $_.passed })) { throw 'Backend image parity failed; inspect parity reports.' }
