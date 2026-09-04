[CmdletBinding()]
param(
    [ValidateSet('d3d12', 'vulkan')]
    [string]$Backend = 'd3d12',
    [string]$Scene = 'preview',
    [ValidateSet('default', 'full-rebuild', 'versioned')]
    [string]$PublicationMode = 'default',
    [ValidateRange(2, 10000)]
    [int]$Frames = 120,
    [string]$BinaryPath = 'build-windows-ci/RelWithDebInfo/PrismRender.exe',
    [Parameter(Mandatory)]
    [string]$OutputDirectory,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent $PSScriptRoot
$binary = [IO.Path]::GetFullPath($BinaryPath, $root)
$output = [IO.Path]::GetFullPath($OutputDirectory, $root)
$report = Join-Path $output 'scene-publication.json'
$log = Join-Path $output 'application.log'

if (-not (Test-Path -LiteralPath $binary -PathType Leaf)) {
    throw "Missing renderer: $binary"
}
if (Test-Path -LiteralPath $output) {
    throw "Refusing to overwrite scene publication evidence: $output"
}

$environment = [ordered]@{
    PRISM_RENDER_DETERMINISTIC = '1'
    PRISM_RENDER_MAX_FRAMES = "$Frames"
    PRISM_RENDER_GPU_VALIDATION = '0'
    PRISM_RENDER_RDG_QUEUE_MODE = 'native'
    PRISM_RENDER_SCENE_PUBLICATION_STATS_PATH = $report
    PRISM_RENDER_LOG_PATH = $log
}
if ($PublicationMode -ne 'default') {
    $environment.PRISM_RENDER_SCENE_PUBLICATION_MODE = $PublicationMode
}
$plan = [ordered]@{
    format = 'PrismScenePublicationBenchmarkPlan'
    version = 1
    backend = $Backend
    scene = $Scene
    publicationMode = $PublicationMode
    frames = $Frames
    binary = $binary
    outputDirectory = $output
    environment = $environment
    arguments = @("--api=$Backend", "--scene=$Scene")
}
if ($DryRun) {
    $plan | ConvertTo-Json -Depth 5
    return
}

New-Item -ItemType Directory -Path $output | Out-Null
$plan | ConvertTo-Json -Depth 5 |
    Set-Content -LiteralPath (Join-Path $output 'plan.json') -Encoding utf8
$previous = @{}
try {
    foreach ($name in $environment.Keys) {
        $previous[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
        [Environment]::SetEnvironmentVariable($name, $environment[$name], 'Process')
    }
    & $binary "--api=$Backend" "--scene=$Scene"
    if ($LASTEXITCODE -ne 0) {
        throw "Renderer exited with code $LASTEXITCODE."
    }
} finally {
    foreach ($name in $environment.Keys) {
        [Environment]::SetEnvironmentVariable($name, $previous[$name], 'Process')
    }
}

if (-not (Test-Path -LiteralPath $report -PathType Leaf)) {
    throw 'Renderer did not write the scene publication report.'
}
$result = Get-Content -LiteralPath $report -Raw | ConvertFrom-Json
if ($result.format -ne 'PrismRenderScenePublicationStatistics' -or
    $result.version -ne 1 -or
    $result.model -ne 'immutable-frame-packet' -or
    $result.frameCount -ne $Frames) {
    throw 'Scene publication report identity or frame count is invalid.'
}
if ($PublicationMode -eq 'full-rebuild') {
    if ($result.totals.sceneDataBuildCount -ne $Frames -or
        $result.totals.sceneDataReuseCount -ne 0) {
        throw 'Full-rebuild publication counters are inconsistent.'
    }
} elseif ($result.totals.sceneDataBuildCount -ne 1 -or
    $result.totals.sceneDataReuseCount -ne ($Frames - 1) -or
    $result.totals.sourceObjectVisitCount -ne $result.frames[0].sourceObjectCount) {
    throw 'Static versioned publication did not rebuild once then reuse.'
}
if ($result.totals.framePacketSmallObjectCount -ne $Frames -or
    $result.totals.copies.publication -ne 0 -or
    $result.totals.copies.gameView -ne 0 -or
    $result.totals.copies.sceneView -ne 0 -or
    $result.totals.copies.estimatedObjects -ne 0 -or
    $result.totals.copies.estimatedBytes -ne 0) {
    throw 'Immutable packet publication unexpectedly copied a full scene.'
}

Write-Output $report
