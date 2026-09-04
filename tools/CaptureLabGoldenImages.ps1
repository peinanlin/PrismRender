param(
    [string]$BuildDirectory = "build-windows-ci\RelWithDebInfo",
    [string]$OutputDirectory = "artifacts\lab-golden",
    [string]$BaselineDirectory = "",
    [ValidateSet('d3d12', 'vulkan')]
    [string]$Api = 'd3d12',
    [string]$SceneKeys = '',
    [switch]$UpdateBaselines,
    [switch]$EnforceBaselines
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot
$buildPath = Join-Path $projectRoot $BuildDirectory
$outputPath = Join-Path $projectRoot $OutputDirectory
$renderer = Join-Path $buildPath "PrismRender.exe"
$visibilityChecker = Join-Path $buildPath "PrismImageVisibilityCheck.exe"
$comparator = Join-Path $buildPath "PrismGoldenImageCompare.exe"

if ((-not (Test-Path -LiteralPath $renderer)) -or
    (-not (Test-Path -LiteralPath $visibilityChecker)) -or
    (-not (Test-Path -LiteralPath $comparator)))
{
    throw "Build PrismRender and the image validation tools before capturing Labs."
}
New-Item -ItemType Directory -Force -Path $outputPath | Out-Null

$scenes = @(
    'preview', 'showcase', 'reflections', 'shadows', 'lights',
    'gpu-driven', 'post-process', 'render-graph', 'materials',
    'streaming', 'atmosphere', 'large-world', 'terrain-vt', 'ocean'
)
if ($SceneKeys -ne '')
{
    $requestedScenes = @($SceneKeys.Split(',') | ForEach-Object { $_.Trim() })
    $unknownScenes = @($requestedScenes | Where-Object { $_ -notin $scenes })
    if ($unknownScenes.Count -ne 0)
    {
        throw "Unknown Lab key(s): $($unknownScenes -join ', ')"
    }
    $scenes = $requestedScenes
}
$views = @('scene', 'game')
$env:PRISM_RENDER_EXIT_AFTER_CAPTURE = '1'
$env:PRISM_RENDER_DETERMINISTIC = '1'
$env:PRISM_RENDER_CAPTURE_STAGE = 'tonemap'

foreach ($scene in $scenes)
{
    $env:PRISM_RENDER_DEMO_SCENE = $scene
    foreach ($view in $views)
    {
        $imageName = "$scene-$view-$Api.bmp"
        $imagePath = Join-Path $outputPath $imageName
        $reportPath = Join-Path $outputPath "$scene-$view-$Api-visibility.txt"
        $env:PRISM_RENDER_CAPTURE_VIEW = $view
        $env:PRISM_RENDER_CAPTURE_PATH = $imagePath

        & $renderer "--api=$Api"
        if ($LASTEXITCODE -ne 0)
        {
            throw "$scene $view capture failed."
        }
        & $visibilityChecker $imagePath "--report=$reportPath" `
            --enforce --mean=0.01 --contrast=0.005 --visible=0.05
        if ($LASTEXITCODE -ne 0)
        {
            throw "$scene $view failed image visibility checks. See $reportPath"
        }

        if ($BaselineDirectory -ne '')
        {
            $baselinePath = Join-Path $projectRoot $BaselineDirectory
            New-Item -ItemType Directory -Force -Path $baselinePath | Out-Null
            $baselineImage = Join-Path $baselinePath $imageName
            if ($UpdateBaselines)
            {
                Copy-Item -Force $imagePath $baselineImage
            }
            elseif ($EnforceBaselines)
            {
                if (!(Test-Path -LiteralPath $baselineImage))
                {
                    throw "Missing approved baseline: $baselineImage"
                }
                $comparisonReport = Join-Path $outputPath `
                    "$scene-$view-$Api-regression.txt"
                & $comparator $baselineImage $imagePath `
                    "--report=$comparisonReport" --enforce `
                    --mean=0.01 --rmse=0.02 --changed=0.05 --ssim=0.95
                if ($LASTEXITCODE -ne 0)
                {
                    throw "$scene $view regressed from its approved baseline."
                }
            }
        }
    }
}

Write-Host "Lab Scene/Game captures and visibility reports: $outputPath"
