param(
    [string]$BuildDirectory = "build-windows-ci\RelWithDebInfo",
    [ValidateSet("d3d12", "vulkan")]
    [string]$Api = "d3d12",
    [int]$Width = 1600,
    [int]$Height = 900,
    [string]$CapturePath = ""
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot
$renderer = Join-Path $projectRoot (
    Join-Path $BuildDirectory "PrismRender.exe")

if (!(Test-Path -LiteralPath $renderer)) {
    throw "PrismRender.exe was not found at $renderer. Build the requested configuration first."
}
if ($Width -lt 64 -or $Height -lt 64) {
    throw "Width and Height must both be at least 64 pixels."
}

$env:PRISM_RENDER_SHOWCASE_SCENE = "1"
$env:PRISM_RENDER_WIDTH = $Width.ToString()
$env:PRISM_RENDER_HEIGHT = $Height.ToString()

if ($CapturePath -ne "") {
    $resolvedCapturePath = if ([System.IO.Path]::IsPathRooted($CapturePath)) {
        $CapturePath
    } else {
        Join-Path $projectRoot $CapturePath
    }
    $captureDirectory = Split-Path -Parent $resolvedCapturePath
    if ($captureDirectory -ne "") {
        New-Item -ItemType Directory -Force -Path $captureDirectory | Out-Null
    }
    $env:PRISM_RENDER_CAPTURE_PATH = $resolvedCapturePath
    $env:PRISM_RENDER_EXIT_AFTER_CAPTURE = "1"
    $env:PRISM_RENDER_DETERMINISTIC = "1"
}

& $renderer "--api=$Api"
if ($LASTEXITCODE -ne 0) {
    throw "PrismRender Showcase Scene exited with code $LASTEXITCODE."
}
