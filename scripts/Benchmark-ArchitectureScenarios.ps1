[CmdletBinding()]
param(
    [ValidateSet('d3d12', 'vulkan')][string]$Backend = 'vulkan',
    [string]$EditorBinary = 'build-windows-ci/RelWithDebInfo/PrismRender.exe',
    [string]$StandaloneBinary = 'build-windows-vulkan-ci/RelWithDebInfo/PrismRender.exe',
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$BuildConfiguration = 'RelWithDebInfo',
    [ValidateSet('off', 'basic', 'detailed', 'capture')]
    [string]$ProfilingLevel = 'basic',
    [ValidateSet('native', 'serial')][string]$QueueMode = 'serial',
    [ValidateSet('inline', 'threaded')][string]$ExecutionMode = 'threaded',
    [ValidateSet('inline', 'pool')][string]$TaskExecutor = 'pool',
    [ValidateSet('all', 'empty', 'preview')][string]$Workload = 'all',
    [ValidateSet('all', 'editor', 'standalone')][string]$BinaryKind = 'all',
    [ValidateSet('all', 'game+scene', 'game')][string]$ActiveViews = 'all',
    [ValidateSet('default', 'focused', 'unfocused')]
    [string]$WindowFocusPolicy = 'default',
    [ValidateSet('interactive-smooth', 'low-latency', 'benchmark', 'custom')]
    [string]$FramePacingProfile = 'interactive-smooth',
    [ValidateSet('', 'synchronized', 'low-latency-synchronized', 'immediate')]
    [string]$PresentationIntent = '',
    [string]$TargetFps = '',
    [ValidateRange(0, 8)][int]$MaxQueuedFrames = 0,
    [ValidateRange(1, 10000)][int]$WarmupFrames = 30,
    [ValidateRange(1, 10000)][int]$SampleFrames = 120,
    [ValidateRange(0, 10000)][int]$DrainFrames = 8,
    [ValidateRange(64, 8192)][int]$Width = 1280,
    [ValidateRange(64, 8192)][int]$Height = 800,
    [bool]$Validation = $true,
    [switch]$VisibleWindow,
    [string]$ScenarioPath = 'scripts/ArchitecturePerformanceScenarios.json',
    [Parameter(Mandatory)][string]$OutputDirectory,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'ArchitectureValidation.Common.ps1')
. (Join-Path $PSScriptRoot 'ArchitectureFrameProfiler.Common.ps1')

$scenarioFile = [IO.Path]::GetFullPath($ScenarioPath, $root)
$configuration = Get-Content -LiteralPath $scenarioFile -Raw | ConvertFrom-Json
if ($configuration.format -ne 'PrismArchitecturePerformanceScenarios' -or
    $configuration.version -ne 1 -or @($configuration.scenarios).Count -eq 0) {
    throw 'Architecture performance scenario configuration is incompatible.'
}
$scenarios = @($configuration.scenarios | Where-Object {
        ($Workload -eq 'all' -or $_.workload -eq $Workload) -and
        ($BinaryKind -eq 'all' -or $_.binaryKind -eq $BinaryKind) -and
        ($ActiveViews -eq 'all' -or $_.activeViews -eq $ActiveViews)
    })
if ($scenarios.Count -eq 0) { throw 'No performance scenarios match the requested workload.' }
if ($FramePacingProfile -eq 'custom') {
    if (-not $PresentationIntent -or $MaxQueuedFrames -eq 0 -or
        ($TargetFps -and $TargetFps -ne 'uncapped' -and
            $TargetFps -notmatch '^[1-9][0-9]*$')) {
        throw 'Custom frame pacing requires a presentation intent, queue depth 1-8, and optional positive target FPS/uncapped.'
    }
} elseif ($PresentationIntent -or $TargetFps -or $MaxQueuedFrames -ne 0) {
    throw 'Preset frame pacing cannot be overridden; select custom.'
}
$output = Resolve-ArchitectureOutputPath $root $OutputDirectory
if (Test-Path -LiteralPath $output) { throw "Refusing to overwrite benchmark evidence: $output" }
$editorPath = [IO.Path]::GetFullPath($EditorBinary, $root)
$standalonePath = [IO.Path]::GetFullPath($StandaloneBinary, $root)
foreach ($binary in @(
        if (@($scenarios | Where-Object binaryKind -eq 'editor').Count) { $editorPath }
        if (@($scenarios | Where-Object binaryKind -eq 'standalone').Count) { $standalonePath }
    )) {
    if (-not (Test-Path -LiteralPath $binary -PathType Leaf)) {
        throw "Missing architecture benchmark binary: $binary"
    }
}
$totalFrames = $WarmupFrames + $SampleFrames + $DrainFrames
$plan = [ordered]@{
    format = 'PrismArchitecturePerformanceBenchmark'
    version = 1
    status = 'planned'
    backend = $Backend
    buildConfiguration = $BuildConfiguration
    validation = $Validation
    profilingLevel = $ProfilingLevel
    queueMode = $QueueMode
    executionMode = $ExecutionMode
    taskExecutor = $TaskExecutor
    workload = $Workload
    binaryKind = $BinaryKind
    activeViews = $ActiveViews
    windowFocusPolicy = $WindowFocusPolicy
    framePacingProfile = $FramePacingProfile
    presentationIntent = $PresentationIntent
    targetFps = $TargetFps
    maxQueuedFrames = $MaxQueuedFrames
    width = $Width
    height = $Height
    visibleWindow = $VisibleWindow.IsPresent
    warmupFrames = $WarmupFrames
    sampleFrames = $SampleFrames
    drainFrames = $DrainFrames
    scenarioDefinition = $scenarioFile
    scenarios = $scenarios
}
if ($DryRun) { $plan | ConvertTo-Json -Depth 8; return }

[IO.Directory]::CreateDirectory($output) | Out-Null
$lock = $null
try {
    $lock = [IO.File]::Open(
        (Join-Path $root 'artifacts/architecture-refactor/gpu-validation.lock'),
        [IO.FileMode]::OpenOrCreate,
        [IO.FileAccess]::ReadWrite,
        [IO.FileShare]::None)
    $plan.status = 'running'
    $summaries = @()
    foreach ($scenario in $scenarios) {
        $runDirectory = Join-Path $output $scenario.id
        [IO.Directory]::CreateDirectory($runDirectory) | Out-Null
        $binary = if ($scenario.binaryKind -eq 'editor') { $editorPath } else { $standalonePath }
        $environment = @{
            PRISM_RENDER_DETERMINISTIC = '1'
            PRISM_RENDER_GPU_VALIDATION = if ($Validation) { '1' } else { '0' }
            PRISM_RENDER_WIDTH = "$Width"
            PRISM_RENDER_HEIGHT = "$Height"
            PRISM_RENDER_MAX_FRAMES = "$totalFrames"
            PRISM_RENDER_RDG_QUEUE_MODE = $QueueMode
            PRISM_RENDER_EXECUTION_MODE = $ExecutionMode
            PRISM_RENDER_TASK_EXECUTOR = $TaskExecutor
            PRISM_RENDER_PROFILING_LEVEL = $ProfilingLevel
            PRISM_RENDER_FRAME_PROFILER_PATH = Join-Path $runDirectory 'frames.jsonl'
            PRISM_RENDER_SCENE_VIEW_REFRESH_POLICY = 'live'
            PRISM_RENDER_FRAME_PACING_PROFILE = $FramePacingProfile
            PRISM_RENDER_LOG_PATH = Join-Path $runDirectory 'application.log'
            PRISM_RENDER_CRASH_REPORT_PATH = Join-Path $runDirectory 'crash.json'
            PRISM_RENDER_MINIDUMP_PATH = Join-Path $runDirectory 'crash.dmp'
        }
        if (-not $VisibleWindow) {
            $environment.PRISM_RENDER_HEADLESS = '1'
        }
        if ($WindowFocusPolicy -ne 'default') {
            $environment.PRISM_RENDER_PERFORMANCE_WINDOW_FOCUS =
                $WindowFocusPolicy
        }
        if ($FramePacingProfile -eq 'custom') {
            $environment.PRISM_RENDER_PRESENTATION_INTENT = $PresentationIntent
            $environment.PRISM_RENDER_MAX_QUEUED_FRAMES = "$MaxQueuedFrames"
            if ($TargetFps) { $environment.PRISM_RENDER_TARGET_FPS = $TargetFps }
        }
        if ($scenario.activeViews -eq 'game') {
            $environment.PRISM_RENDER_PERFORMANCE_ACTIVE_VIEWS = 'game'
        }
        if ($scenario.binaryKind -eq 'standalone') {
            # A full Editor build can enter the benchmark-only standalone shell.
            # This permits a genuinely same-binary Editor/Game/standalone matrix.
            $environment.PRISM_RENDER_PERFORMANCE_STANDALONE = '1'
        }
        if ($scenario.workload -eq 'empty') {
            $environment.PRISM_RENDER_PERFORMANCE_WORKLOAD = 'empty'
        }
        $arguments = @("--api=$Backend", '--scene=preview')
        $processArguments = @{
            Binary = $binary
            Arguments = $arguments
            Environment = $environment
            WorkingDirectory = $runDirectory
            OutputBase = Join-Path $runDirectory 'process'
            TimeoutSeconds = 900
        }
        Invoke-ArchitectureProcess @processArguments
        if ($Validation) {
            $validationLogPath = Join-Path $runDirectory 'process.stderr.log'
            $validationLog = Get-Content $validationLogPath -Raw
            Assert-ArchitectureValidationLog $validationLog $Backend
        }
        $summaryArguments = @{
            Path = $environment.PRISM_RENDER_FRAME_PROFILER_PATH
            Scenario = $scenario
            Warmup = $WarmupFrames
            Samples = $SampleFrames
            ExpectedBackend = $Backend
            ExpectedBuild = $BuildConfiguration
            ExpectedLevel = $ProfilingLevel
            ExpectedValidation = $Validation
            ExpectedVisible = $VisibleWindow.IsPresent
            ExpectedFocusPolicy = $WindowFocusPolicy
            ExpectedWidth = $Width
            ExpectedHeight = $Height
        }
        $summaries += Get-ArchitectureScenarioRunSummary @summaryArguments
    }
    $plan.status = 'measured-and-validated'
    $plan['results'] = $summaries
    $plan['comparisonContract'] = [ordered]@{
        matchedFields = @('backend', 'buildConfiguration', 'validation',
            'profilingLevel', 'queueMode', 'executionMode',
            'taskExecutor', 'width', 'height', 'visibleWindow',
            'windowFocusPolicy')
        fpsMeaning = 'actual completed Editor/Application loop frames per second'
        gpuMeaning = 'resolved historical-frame critical-view hint; views are not summed'
        prohibitedInference = @('inverse single-pass time', 'sum of overlapping phases')
    }
} catch {
    $plan.status = 'failed'
    $plan['error'] = $_.Exception.Message
    throw
} finally {
    if ($null -ne $lock) { $lock.Dispose() }
    $plan['artifacts'] = @(
        Get-ChildItem -LiteralPath $output -Recurse -File |
        ForEach-Object {
            [ordered]@{
                path = [IO.Path]::GetRelativePath($output, $_.FullName)
                sha256 = (Get-FileHash -LiteralPath $_.FullName).Hash
            }
        })
    [IO.File]::WriteAllText(
        (Join-Path $output 'summary.json'),
        ($plan | ConvertTo-Json -Depth 16),
        [Text.UTF8Encoding]::new($false))
}
Write-Output (Join-Path $output 'summary.json')
