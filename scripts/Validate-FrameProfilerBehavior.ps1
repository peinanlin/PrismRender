[CmdletBinding()]
param(
    [ValidateSet('d3d12', 'vulkan')][string]$Backend = 'vulkan',
    [Parameter(Mandatory)][string]$BinaryPath,
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$BuildConfiguration = 'RelWithDebInfo',
    [bool]$EditorEnabled = $true,
    [string]$Scene = 'preview',
    [ValidateSet('native', 'serial')][string]$QueueMode = 'serial',
    [ValidateRange(64, 8192)][int]$Width = 640,
    [ValidateRange(64, 8192)][int]$Height = 360,
    [ValidateRange(3, 10000)][int]$CaptureFrame = 30,
    [bool]$Validation = $true,
    [string]$ImageCompareBinary =
        'build-windows-ci/RelWithDebInfo/PrismGoldenImageCompare.exe',
    [string]$DiagnosticsCompareBinary =
        'build-windows-ci/RelWithDebInfo/PrismFrameDiagnosticsCompare.exe',
    [Parameter(Mandatory)][string]$OutputDirectory,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'ArchitectureValidation.Common.ps1')
. (Join-Path $PSScriptRoot 'ArchitectureFrameProfiler.Common.ps1')

$binary = [IO.Path]::GetFullPath($BinaryPath, $root)
$imageCompare = [IO.Path]::GetFullPath($ImageCompareBinary, $root)
$diagnosticsCompare = [IO.Path]::GetFullPath(
    $DiagnosticsCompareBinary, $root)
$output = Resolve-ArchitectureOutputPath $root $OutputDirectory
foreach ($path in @($binary, $imageCompare, $diagnosticsCompare)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing profiler behavior input: $path"
    }
}
if (Test-Path -LiteralPath $output) {
    throw "Refusing to overwrite profiler behavior evidence: $output"
}

$levels = @('off', 'basic', 'detailed')
$totalFrames = $CaptureFrame
$plan = [ordered]@{
    format = 'PrismFrameProfilerBehaviorValidation'
    version = 1
    status = 'planned'
    backend = $Backend
    buildConfiguration = $BuildConfiguration
    editorEnabled = $EditorEnabled
    validation = $Validation
    scene = $Scene
    queueMode = $QueueMode
    width = $Width
    height = $Height
    captureFrame = $CaptureFrame
    levels = $levels
    binary = $binary
    binarySha256 = (Get-FileHash -LiteralPath $binary).Hash
}
if ($DryRun) {
    $plan | ConvertTo-Json -Depth 6
    return
}

function Read-LevelSummary {
    param([string]$Path, [string]$ExpectedLevel)

    $rows = @([IO.File]::ReadLines($Path) |
        ForEach-Object { $_ | ConvertFrom-Json })
    if ($rows.Count -lt 3 -or $rows[0].format -ne 'PrismFrameProfiler' -or
        $rows[-1].status -ne 'complete') {
        throw "Incomplete frame profiler behavior report: $Path"
    }
    $metadata = $rows[0].metadata
    $backendName = if ($Backend -eq 'd3d12') { 'Direct3D 12' } else { 'Vulkan' }
    if ($metadata.backend -ne $backendName -or
        $metadata.identity.identity.buildConfiguration -ne $BuildConfiguration -or
        [bool]$metadata.validationRequested -ne $Validation -or
        [bool]$metadata.editorEnabled -ne $EditorEnabled -or
        [int]$metadata.width -ne $Width -or
        [int]$metadata.height -ne $Height) {
        throw "Profiler behavior report conditions do not match for '$ExpectedLevel'."
    }
    $frames = @($rows | Where-Object type -eq 'frame')
    if ($frames.Count -ne [int]$rows[-1].completedFrames -or
        @($frames | Where-Object {
                $_.requestedLevel -ne $ExpectedLevel -or
                $_.actualLevel -ne $ExpectedLevel
            }).Count -ne 0) {
        throw "Profiler behavior level or frame count mismatch for '$ExpectedLevel'."
    }
    $gpuSamples = @($frames | Where-Object { $null -ne $_.gpuFrameMs }).Count
    $viewGpuSamples = @($frames | Where-Object {
            $null -ne $_.views.game.gpuTotalMs -or
            $null -ne $_.views.scene.gpuTotalMs
        }).Count
    if ($ExpectedLevel -eq 'off' -and
        ($gpuSamples -ne 0 -or $viewGpuSamples -ne 0)) {
        throw 'Off profiling emitted optional GPU timing samples.'
    }
    if ($ExpectedLevel -ne 'off' -and $gpuSamples -eq 0) {
        throw "Profiling level '$ExpectedLevel' produced no resolved GPU samples."
    }
    return [ordered]@{
        frameCount = $frames.Count
        gpuSampleFrames = $gpuSamples
        editorLoopMs = Get-ArchitectureDistribution @($frames.editorLoopMs)
        profilerOverheadMs = Get-ArchitectureDistribution @(
            $frames.profilerOverheadMs)
    }
}

[IO.Directory]::CreateDirectory($output) | Out-Null
$plan.status = 'running'
try {
    $levelResults = [ordered]@{}
    foreach ($level in $levels) {
        $run = Join-Path $output $level
        [IO.Directory]::CreateDirectory($run) | Out-Null
        $captureSequencePath = Join-Path $run 'capture-sequence.json'
        $captureOutput = Join-Path $run 'capture'
        [IO.File]::WriteAllText(
            $captureSequencePath,
            (@{ version = 1; frames = @($CaptureFrame) } |
                ConvertTo-Json -Compress),
            [Text.UTF8Encoding]::new($false))
        $environment = @{
            PRISM_RENDER_DETERMINISTIC = '1'
            PRISM_RENDER_GPU_VALIDATION = if ($Validation) { '1' } else { '0' }
            PRISM_RENDER_WIDTH = "$Width"
            PRISM_RENDER_HEIGHT = "$Height"
            PRISM_RENDER_MAX_FRAMES = "$totalFrames"
            PRISM_RENDER_RDG_QUEUE_MODE = $QueueMode
            PRISM_RENDER_PROFILING_LEVEL = $level
            PRISM_RENDER_PROFILING_LEVEL_SEQUENCE = "${level}:${totalFrames}"
            PRISM_RENDER_FRAME_PROFILER_PATH = Join-Path $run 'profiler.jsonl'
            PRISM_RENDER_CAPTURE_SEQUENCE_PATH = $captureSequencePath
            PRISM_RENDER_CAPTURE_SEQUENCE_OUTPUT = $captureOutput
            PRISM_RENDER_CAPTURE_VIEW = 'game'
            PRISM_RENDER_EXIT_AFTER_CAPTURE = '1'
            PRISM_RENDER_PERFORMANCE_ACTIVE_VIEWS = 'game'
            PRISM_RENDER_SCENE_VIEW_REFRESH_POLICY = 'catalog-default'
        }
        if ($Validation -and $Backend -eq 'vulkan') {
            $layerPath = Join-Path $root 'artifacts/vulkan-validation-tools/sdk/Bin'
            if (-not (Test-Path -LiteralPath (
                        Join-Path $layerPath 'VkLayer_khronos_validation.json'))) {
                throw 'Project-local Vulkan validation layer is unavailable.'
            }
            $environment.VK_LAYER_PATH = $layerPath
        }
        $processArguments = @{
            Binary = $binary
            Arguments = @("--api=$Backend", "--scene=$Scene")
            Environment = $environment
            WorkingDirectory = $run
            OutputBase = Join-Path $run 'process'
            TimeoutSeconds = 300
        }
        Invoke-ArchitectureProcess @processArguments
        if ($Validation) {
            $validationLogPath = Join-Path $run 'process.stderr.log'
            $validationLog = Get-Content -LiteralPath $validationLogPath -Raw
            Assert-ArchitectureValidationLog $validationLog $Backend
        }
        foreach ($required in @('profiler.jsonl',
                'capture/frames.jsonl',
                "capture/frame-$CaptureFrame.bmp",
                "capture/frame-$CaptureFrame.capture.json")) {
            $requiredPath = Join-Path $run $required
            if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
                throw "Profiler behavior run '$level' missed $required."
            }
        }
        $levelSummaryArguments = @{
            Path = Join-Path $run 'profiler.jsonl'
            ExpectedLevel = $level
        }
        $levelResults[$level] = Read-LevelSummary @levelSummaryArguments
    }

    $comparisons = @()
    foreach ($candidate in @('basic', 'detailed')) {
        $comparisonRoot = Join-Path $output "compare-off-$candidate"
        [IO.Directory]::CreateDirectory($comparisonRoot) | Out-Null
        $imageArguments = @(
            (Join-Path $output "off/capture/frame-$CaptureFrame.bmp"),
            (Join-Path $output "$candidate/capture/frame-$CaptureFrame.bmp"),
            "--report=$(Join-Path $comparisonRoot 'image-metrics.txt')") +
            @(Get-ArchitectureImageThresholds 'same-backend')
        $imageProcessArguments = @{
            Binary = $imageCompare
            Arguments = $imageArguments
            WorkingDirectory = $comparisonRoot
            OutputBase = Join-Path $comparisonRoot 'image'
        }
        Invoke-ArchitectureProcess @imageProcessArguments
        $diagnosticsProcessArguments = @{
            Binary = $diagnosticsCompare
            Arguments = @(
                (Join-Path $output "off/capture/frame-$CaptureFrame.capture.json"),
                (Join-Path $output "$candidate/capture/frame-$CaptureFrame.capture.json"),
                (Join-Path $comparisonRoot 'diagnostics.json'))
            WorkingDirectory = $comparisonRoot
            OutputBase = Join-Path $comparisonRoot 'diagnostics'
        }
        Invoke-ArchitectureProcess @diagnosticsProcessArguments
        $diagnosticsPath = Join-Path $comparisonRoot 'diagnostics.json'
        $diagnostics = Get-Content $diagnosticsPath -Raw | ConvertFrom-Json
        if (-not [bool]$diagnostics.passed) {
            throw "Profiler level '$candidate' changed frame diagnostics."
        }
        $comparisons += [ordered]@{
            reference = 'off'
            candidate = $candidate
            imageMetrics = 'image-metrics.txt'
            diagnostics = 'diagnostics.json'
        }
    }
    $plan.status = 'passed'
    $plan['levelResults'] = $levelResults
    $plan['comparisons'] = $comparisons
    $plan['contract'] = @(
        'Fixed-input images use same-backend V2 thresholds.',
        'Capture diagnostics compare pass/resource structure, simulation counts and history.',
        'Detailed profiler overhead is reported separately and is not subtracted from FPS.'
    )
} catch {
    $plan.status = 'failed'
    $plan['error'] = $_.Exception.Message
    throw
} finally {
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
