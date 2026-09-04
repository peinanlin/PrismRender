<#
.SYNOPSIS
Builds and runs one PrismRender feature lab.

.DESCRIPTION
Scene names are validated before launch and forwarded through the renderer's
--scene command-line option. The four independent Fluid demos are:
pbf, fluid-render, fluid-caustics, and fluid-toon. The legacy fluid name is an
alias for fluid-render.

.EXAMPLE
.\tools\RunRendererLab.ps1 -Scene pbf -Api d3d12 -Build

.EXAMPLE
.\tools\RunRendererLab.ps1 -Scene fluid-caustics -Api vulkan -MaxFrames 120
#>
[CmdletBinding()]
param(
    [ValidateSet(
        'preview',
        'showcase',
        'reflections',
        'shadows',
        'lights',
        'gpu-driven',
        'post-process',
        'render-graph',
        'materials',
        'streaming',
        'atmosphere',
        'large-world',
        'terrain-vt',
        'ocean',
        'pbf',
        'fluid-render',
        'fluid-caustics',
        'fluid-toon',
        'fluid')]
    [string]$Scene = 'reflections',

    [ValidateSet('d3d12', 'vulkan')]
    [string]$Api = 'd3d12',

    [ValidateSet('Debug', 'RelWithDebInfo', 'Release')]
    [string]$Configuration = 'Debug',

    [switch]$Build,

    [uint32]$MaxFrames = 0
)

$projectRoot = Split-Path -Parent $PSScriptRoot
$buildDirectory = Join-Path $projectRoot 'build-windows-ci'
$executable = Join-Path $buildDirectory "$Configuration\PrismRender.exe"

if ($Build)
{
    & cmake --build $buildDirectory --config $Configuration --target PrismRender
    if ($LASTEXITCODE -ne 0)
    {
        exit $LASTEXITCODE
    }
}

if (-not (Test-Path -LiteralPath $executable))
{
    throw "PrismRender executable was not found: $executable"
}

if ($MaxFrames -gt 0)
{
    $env:PRISM_RENDER_MAX_FRAMES = $MaxFrames.ToString()
}
else
{
    Remove-Item Env:PRISM_RENDER_MAX_FRAMES -ErrorAction SilentlyContinue
}

Push-Location $projectRoot
try
{
    & $executable "--api=$Api" "--scene=$Scene"
    exit $LASTEXITCODE
}
finally
{
    Pop-Location
}
