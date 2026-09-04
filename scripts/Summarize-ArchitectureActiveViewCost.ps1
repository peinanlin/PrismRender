[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$DualViewRunDirectory,
    [Parameter(Mandatory)][string]$GameOnlyRunDirectory,
    [Parameter(Mandatory)][string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'ArchitectureValidation.Common.ps1')
. (Join-Path $PSScriptRoot 'ArchitecturePerformance.Common.ps1')
$dualPath = Resolve-ArchitectureOutputPath $root $DualViewRunDirectory
$gamePath = Resolve-ArchitectureOutputPath $root $GameOnlyRunDirectory
$output = Resolve-ArchitectureOutputPath $root $OutputDirectory
if (Test-Path -LiteralPath $output) { throw "Refusing to overwrite active-view report: $output" }
$dual = Read-ArchitecturePerformanceRun $dualPath
$game = Read-ArchitecturePerformanceRun $gamePath

foreach ($key in @('backend','scene','view','queueMode','width','height','warmupFrames','sampleFrames','drainFrames',
        'binarySha256','cachePolicy','initialImGuiSha256')) {
    if ($dual.index[$key] -ne $game.index[$key]) { throw "Active-view input mismatch: $key" }
}
if ($dual.sourceHash -ne $game.sourceHash) { throw 'Active-view runs used different production inputs.' }
if ($dual.summary.activeViews -ne 'game,scene' -or $dual.summary.activeViewPolicy -ne 'default' -or
    $game.summary.activeViews -ne 'game' -or $game.summary.activeViewPolicy -ne 'game') {
    throw 'Active-view report requires normal Editor game+scene and explicit same-binary game-only runs.'
}
if (($dual.summary.presentationConditions | ConvertTo-Json -Compress) -ne
    ($game.summary.presentationConditions | ConvertTo-Json -Compress) -or
    $dual.summary.windowFocusPolicy -ne $game.summary.windowFocusPolicy) {
    throw 'Active-view runs used different observed window/display conditions.'
}
foreach ($key in @('graphicsApi','adapterName','vendorId','deviceId','driverVersion','apiVersion','cpuName',
        'buildConfiguration','compiler','architecture','shaderRevision','executableHash')) {
    if (($dual.identity[$key] | ConvertTo-Json -Compress) -ne ($game.identity[$key] | ConvertTo-Json -Compress)) {
        throw "Active-view runtime identity mismatch: $key"
    }
}
$ignoredEnvironment = @('PRISM_RENDER_FRAME_PERFORMANCE_PATH','PRISM_RENDER_PERFORMANCE_IDENTITY_PATH',
    'PRISM_RENDER_QUEUE_COST_MODEL_PATH','PRISM_RENDER_LOG_PATH','PRISM_RENDER_CRASH_REPORT_PATH',
    'PRISM_RENDER_MINIDUMP_PATH','PRISM_RENDER_PERFORMANCE_ACTIVE_VIEWS')
$environmentKeys = @(@($dual.index.environment.Keys) + @($game.index.environment.Keys) | Select-Object -Unique)
foreach ($key in $environmentKeys) {
    if ($key -notin $ignoredEnvironment -and $dual.index.environment[$key] -ne $game.index.environment[$key]) {
        throw "Active-view environment mismatch: $key"
    }
}

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

function Get-TopGpuPasses($Summary, [string]$View) {
    return @($Summary.distributions.Keys | Where-Object {
            $_ -match "^gpu\.$View\." -and $_ -notmatch '\.Renderer$'
        } | ForEach-Object {
            @{ metric = $_; medianMs = $Summary.distributions[$_].median; p95Ms = $Summary.distributions[$_].p95 }
        } | Sort-Object medianMs -Descending | Select-Object -First 10)
}

$dualFps = Get-ActualLoopFpsDistribution $dualPath $dual
$gameFps = Get-ActualLoopFpsDistribution $gamePath $game
$phaseMetrics = @('loopIntervalMs','frameMs','gameRenderMs','sceneRenderMs','uiBuildMs','uiDrawMs','beginFrameMs','presentMs','extractionMs')
$phases = [ordered]@{}
foreach ($metric in $phaseMetrics) {
    $key = "cpu.$metric"
    $phases[$metric] = @{
        dualViewMedianMs = $dual.summary.distributions[$key].median
        gameOnlyMedianMs = $game.summary.distributions[$key].median
        medianSavedMs = $dual.summary.distributions[$key].median - $game.summary.distributions[$key].median
    }
}
$loopSaved = $phases.loopIntervalMs.medianSavedMs
$report = [ordered]@{
    format = 'PrismArchitectureActiveViewCost'; version = 1
    backend = $dual.index.backend; scene = $dual.index.scene; width = $dual.index.width; height = $dual.index.height
    binarySha256 = $dual.index.binarySha256; sourceInputsSha256 = $dual.sourceHash
    queueMode = $dual.index.queueMode; strictGpuValidation = $true
    observedPresentationConditions = $dual.summary.presentationConditions
    dualView = @{ source = $dualPath; sourceIndexSha256 = (Get-FileHash (Join-Path $dualPath 'index.json')).Hash
        activeViews = $dual.summary.activeViews; actualLoopFps = $dualFps }
    gameOnly = @{ source = $gamePath; sourceIndexSha256 = (Get-FileHash (Join-Path $gamePath 'index.json')).Hash
        activeViews = $game.summary.activeViews; actualLoopFps = $gameFps }
    cpuPhaseMedians = $phases
    attribution = @{
        loopMedianSavedMs = $loopSaved
        sceneRenderMedianSavedMs = $phases.sceneRenderMs.medianSavedMs
        beginFrameMedianSavedMs = $phases.beginFrameMs.medianSavedMs
        residualMedianSavedMs = $loopSaved - $phases.sceneRenderMs.medianSavedMs - $phases.beginFrameMs.medianSavedMs
        medianFpsRatioGameOnlyToDualView = $gameFps.median / $dualFps.median
    }
    gpu = @{
        dualGameRenderer = $dual.summary.distributions['gpu.game.Graphics.Renderer']
        gameOnlyRenderer = $game.summary.distributions['gpu.game.Graphics.Renderer']
        dualSceneRenderer = $dual.summary.distributions['gpu.scene.Graphics.Renderer']
        dualGameTopPasses = Get-TopGpuPasses $dual.summary 'game'
        gameOnlyTopPasses = Get-TopGpuPasses $game.summary 'game'
        dualSceneTopPasses = Get-TopGpuPasses $dual.summary 'scene'
    }
    limitations = @(
        'The two sealed processes are independent samples; differences are between distribution medians, not paired frame subtraction.',
        'Game-only keeps the Editor shell and UI but aliases the Scene panel image to the transitioned Game output; it submits no Scene renderer work.',
        'Strict validation and the complete Demo feature set remain enabled; this is not an empty-scene retail benchmark.',
        'GPU pass intervals can overlap and must not be summed to derive frame time or FPS.')
}
New-Item -ItemType Directory -Path $output | Out-Null
$report | ConvertTo-Json -Depth 14 | Set-Content -LiteralPath (Join-Path $output 'report.json') -Encoding utf8
Write-Output (Join-Path $output 'report.json')
