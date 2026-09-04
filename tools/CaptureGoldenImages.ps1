param(
    [string]$BuildDirectory = "build-windows-ci\Debug",
    [string]$OutputDirectory = "build-windows-ci\golden",
    [string]$BaselineDirectory = "",
    [switch]$UpdateBaselines,
    [switch]$EnforceThresholds,
    [switch]$CapturePasses
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot
$buildPath = Join-Path $projectRoot $BuildDirectory
$outputPath = Join-Path $projectRoot $OutputDirectory
$renderer = Join-Path $buildPath "PrismRender.exe"
$comparator = Join-Path $buildPath "PrismGoldenImageCompare.exe"

New-Item -ItemType Directory -Force -Path $outputPath | Out-Null
if (!(Test-Path $renderer) -or !(Test-Path $comparator)) {
    throw "Build PrismRender and PrismGoldenImageCompare before running the capture pipeline."
}

$env:PRISM_RENDER_EXIT_AFTER_CAPTURE = "1"
$env:PRISM_RENDER_DETERMINISTIC = "1"
$stages = if ($CapturePasses) {
    @("shadow", "gbuffer0", "gbuffer1", "gbuffer2", "gbuffer3", "hdr", "bloom", "tonemap")
} else {
    @("tonemap")
}

foreach ($stage in $stages) {
    $env:PRISM_RENDER_CAPTURE_STAGE = $stage
    $d3d12Image = if ($stage -eq "tonemap") {
        Join-Path $outputPath "d3d12.bmp"
    } else {
        Join-Path $outputPath "d3d12-$stage.bmp"
    }
    $vulkanImage = if ($stage -eq "tonemap") {
        Join-Path $outputPath "vulkan.bmp"
    } else {
        Join-Path $outputPath "vulkan-$stage.bmp"
    }
    $report = if ($stage -eq "tonemap") {
        Join-Path $outputPath "comparison.txt"
    } else {
        Join-Path $outputPath "$stage-comparison.txt"
    }

    $env:PRISM_RENDER_CAPTURE_PATH = $d3d12Image
    & $renderer --api=d3d12
    if ($LASTEXITCODE -ne 0) { throw "D3D12 $stage capture failed." }

    $env:PRISM_RENDER_CAPTURE_PATH = $vulkanImage
    & $renderer --api=vulkan
    if ($LASTEXITCODE -ne 0) { throw "Vulkan $stage capture failed." }

    $arguments = @($d3d12Image, $vulkanImage, "--report=$report")
    if ($EnforceThresholds) {
        $arguments += @(
            "--enforce",
            "--mean=0.001",
            "--rmse=0.005",
            "--changed=0.01"
        )
    }
    & $comparator @arguments
    if ($LASTEXITCODE -ne 0) { throw "$stage golden image thresholds were exceeded. See $report" }
    Write-Host "Golden image report [$stage]: $report"
}

if ($BaselineDirectory -ne "") {
    $baselinePath = Join-Path $projectRoot $BaselineDirectory
    $baselineD3D12 = Join-Path $baselinePath "d3d12.bmp"
    $baselineVulkan = Join-Path $baselinePath "vulkan.bmp"
    New-Item -ItemType Directory -Force -Path $baselinePath | Out-Null
    if ($UpdateBaselines) {
        Copy-Item -Force $d3d12Image $baselineD3D12
        Copy-Item -Force $vulkanImage $baselineVulkan
    } else {
        if (!(Test-Path $baselineD3D12) -or !(Test-Path $baselineVulkan)) {
            throw "Per-backend baselines are missing. Run once with -UpdateBaselines."
        }
        & $comparator $baselineD3D12 $d3d12Image "--report=$(Join-Path $outputPath 'd3d12-regression.txt')" --enforce --mean=0.01 --rmse=0.02 --changed=0.05
        if ($LASTEXITCODE -ne 0) { throw "D3D12 regressed from its approved baseline." }
        & $comparator $baselineVulkan $vulkanImage "--report=$(Join-Path $outputPath 'vulkan-regression.txt')" --enforce --mean=0.01 --rmse=0.02 --changed=0.05
        if ($LASTEXITCODE -ne 0) { throw "Vulkan regressed from its approved baseline." }
    }
}
