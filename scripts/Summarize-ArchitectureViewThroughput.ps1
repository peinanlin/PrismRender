[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$EditorRunDirectory,
    [Parameter(Mandatory)][string]$StandaloneRunDirectory,
    [Parameter(Mandatory)][string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'ArchitectureValidation.Common.ps1')
. (Join-Path $PSScriptRoot 'ArchitecturePerformance.Common.ps1')
$editorPath = Resolve-ArchitectureOutputPath $root $EditorRunDirectory
$standalonePath = Resolve-ArchitectureOutputPath $root $StandaloneRunDirectory
$output = Resolve-ArchitectureOutputPath $root $OutputDirectory
if (Test-Path -LiteralPath $output) { throw "Refusing to overwrite view-throughput report: $output" }
$editor = Read-ArchitecturePerformanceRun $editorPath
$standalone = Read-ArchitecturePerformanceRun $standalonePath
foreach ($key in @('backend','scene','view','queueMode','width','height','warmupFrames','sampleFrames','drainFrames')) {
    if ($editor.index[$key] -ne $standalone.index[$key]) { throw "View-throughput input mismatch: $key" }
}
if ($editor.summary.activeViews -ne 'game,scene' -or $standalone.summary.activeViews -ne 'game') {
    throw 'View-throughput report requires an Editor game+scene run and a standalone game-only run.'
}
if ($editor.identity.vendorId -ne $standalone.identity.vendorId -or $editor.identity.deviceId -ne $standalone.identity.deviceId -or
    $editor.identity.driverVersion -ne $standalone.identity.driverVersion) { throw 'View-throughput runs used different GPU/driver identities.' }

function Get-ActualLoopFpsDistribution([string]$Directory, $Run) {
    $start = [int]$Run.index.warmupFrames + 1
    $end = [int]$Run.index.warmupFrames + [int]$Run.index.sampleFrames
    $values = @(Get-Content -LiteralPath (Join-Path $Directory 'frames.jsonl') | ForEach-Object { $_ | ConvertFrom-Json -AsHashtable } |
        Where-Object { $_.type -eq 'cpu' -and $_.frameId -ge $start -and $_.frameId -le $end } |
        ForEach-Object {
            $interval = [double]$_.cpu.loopIntervalMs
            if (-not [double]::IsFinite($interval) -or $interval -le 0) { throw 'Invalid loop interval for actual FPS.' }
            1000.0 / $interval
        })
    if ($values.Count -ne $Run.index.sampleFrames) { throw 'Actual FPS sample count differs from the sealed run.' }
    $distribution = Get-ArchitectureDistribution $values
    $sorted = @($values | Sort-Object)
    $distribution['p05'] = $sorted[[Math]::Max(0, [Math]::Ceiling(0.05 * $sorted.Count) - 1)]
    return $distribution
}

$editorFps = Get-ActualLoopFpsDistribution $editorPath $editor
$standaloneFps = Get-ActualLoopFpsDistribution $standalonePath $standalone
$report = [ordered]@{
    format = 'PrismArchitectureViewThroughputComparison'; version = 1
    backend = $editor.index.backend; scene = $editor.index.scene; width = $editor.index.width; height = $editor.index.height
    queueMode = $editor.index.queueMode; strictGpuValidation = $true
    editor = @{ source = $editorPath; sourceIndexSha256 = (Get-FileHash (Join-Path $editorPath 'index.json')).Hash
        activeViews = $editor.summary.activeViews; actualLoopFps = $editorFps
        cpuLoopIntervalMs = $editor.summary.distributions['cpu.loopIntervalMs']
        cpuFrameMs = $editor.summary.distributions['cpu.frameMs']
        cpuGameRenderMs = $editor.summary.distributions['cpu.gameRenderMs']
        cpuSceneRenderMs = $editor.summary.distributions['cpu.sceneRenderMs']
        gpuGameRendererMs = $editor.summary.distributions['gpu.game.Graphics.Renderer']
        gpuSceneRendererMs = $editor.summary.distributions['gpu.scene.Graphics.Renderer'] }
    standalone = @{ source = $standalonePath; sourceIndexSha256 = (Get-FileHash (Join-Path $standalonePath 'index.json')).Hash
        activeViews = $standalone.summary.activeViews; actualLoopFps = $standaloneFps
        cpuLoopIntervalMs = $standalone.summary.distributions['cpu.loopIntervalMs']
        cpuFrameMs = $standalone.summary.distributions['cpu.frameMs']
        cpuGameRenderMs = $standalone.summary.distributions['cpu.gameRenderMs']
        gpuGameRendererMs = $standalone.summary.distributions['gpu.game.Graphics.Renderer'] }
    medianFpsRatioStandaloneToEditor = $standaloneFps.median / $editorFps.median
    limitations = @(
        'Actual FPS is derived only from consecutive frame-start loop intervals, never from a GPU pass duration.',
        'The comparison isolates complete Editor dual-view versus no-Editor game-only operation; it does not attribute the whole difference to Scene rendering alone.',
        'Strict validation overhead and the full preview Demo remain enabled, so this is not an empty-scene retail benchmark.')
}
New-Item -ItemType Directory -Path $output | Out-Null
$report | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $output 'report.json') -Encoding utf8
Write-Output (Join-Path $output 'report.json')
