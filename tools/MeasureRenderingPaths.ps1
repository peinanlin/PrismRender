#requires -Version 7.0
[CmdletBinding()]
param(
    [ValidateSet('d3d12', 'vulkan')][string]$Backend = 'd3d12',
    [string]$BinaryPath = 'build-windows-ci/RelWithDebInfo/PrismRender.exe',
    [Parameter(Mandatory)][string]$OutputDirectory,
    [ValidateRange(64, 8192)][int]$Width = 1600,
    [ValidateRange(64, 8192)][int]$Height = 900,
    [ValidateRange(1, 10000)][int]$WarmupFrames = 120,
    [ValidateRange(2, 10000)][int]$SampleFrames = 600,
    [ValidateRange(1, 20)][int]$Repetitions = 3,
    [ValidateSet('forward', 'forward-plus', 'deferred')]
    [string[]]$Paths = @('forward', 'forward-plus', 'deferred'),
    [ValidateRange(1, 32)][int[]]$LightCounts = @(4, 32),
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent $PSScriptRoot
$helper = Join-Path $root 'scripts/ArchitectureValidation.Common.ps1'
. $helper

if (-not $Paths.Count -or -not $LightCounts.Count -or
    @($Paths | Select-Object -Unique).Count -ne $Paths.Count -or
    @($LightCounts | Select-Object -Unique).Count -ne $LightCounts.Count) {
    throw 'Paths and light counts must be nonempty and unique.'
}
$binary = [IO.Path]::GetFullPath($BinaryPath, $root)
$output = [IO.Path]::GetFullPath($OutputDirectory, $root)
if (-not (Test-Path -LiteralPath $binary -PathType Leaf)) {
    throw "Missing renderer: $binary"
}
if (Test-Path -LiteralPath $output) {
    throw "Refusing to overwrite rendering-path evidence: $output"
}
$drainFrames = 8
$totalFrames = $WarmupFrames + $SampleFrames + $drainFrames
$cases = @(
    foreach ($count in $LightCounts) {
        foreach ($path in $Paths) {
            # Legacy Forward evaluates at most four local lights. Do not time
            # a truncated workload against the full clustered-light result.
            if ($path -eq 'forward' -and $count -gt 4) { continue }
            [ordered]@{ path = $path; lightCount = $count }
        }
    }
)
if (-not $cases.Count) { throw 'No comparable rendering paths remain for the requested light counts.' }
$record = [ordered]@{
    format = 'PrismRenderingPathMeasurement'; version = 1; status = 'planned'
    backend = $Backend; scene = 'lights'; width = $Width; height = $Height
    binary = $binary; binarySha256 = (Get-FileHash -LiteralPath $binary).Hash
    driverSha256 = (Get-FileHash -LiteralPath $PSCommandPath).Hash
    processHelperSha256 = (Get-FileHash -LiteralPath $helper).Hash
    warmupFrames = $WarmupFrames; sampleFrames = $SampleFrames
    drainFrames = $drainFrames; repetitions = $Repetitions
    paths = $Paths; lightCounts = $LightCounts; cases = $cases
    outputDirectory = $output; runs = @()
    conditions = [ordered]@{
        activeView = 'game'; editorEnabled = $false; deterministic = $true
        validation = $false; profilingLevel = 'basic'; pacingProfile = 'benchmark'
        renderExecution = 'threaded'; rhiExecution = 'native-direct'; queueMode = 'native'
        pathOrder = 'rotate paths by repetition within each light-count group'
        frameSelection = 'CPU frameId and GPU gpuResolvedFrameId in warmup+1 through warmup+samples'
    }
    limitations = @(
        'Forward is omitted above four local lights because its legacy shader cannot evaluate the complete workload.',
        'Benchmark pacing requests Immediate presentation; inspect actual framePacing before publishing numbers.',
        'GPU validation is disabled; visual correctness is verified separately.',
        'Each run uses a fresh process; OS and driver caches are not cleared.',
        'GPU clocks are not locked. Preserve all repetitions to expose run-to-run variation.',
        'editorLoopMs measures completed application-loop intervals, not display scanout or pure CPU active work.',
        'GPU samples are asynchronous; use their resolved source frame IDs, including results observed during drain frames.'
    )
}
if ($DryRun) { $record | ConvertTo-Json -Depth 10; return }

[IO.Directory]::CreateDirectory($output) | Out-Null
$lockDirectory = Join-Path $root 'artifacts/architecture-refactor'
[IO.Directory]::CreateDirectory($lockDirectory) | Out-Null
$lock = $null
try {
    $lock = [IO.File]::Open((Join-Path $lockDirectory 'gpu-validation.lock'),
        [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    $record.status = 'running'
    $record | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $output 'index.json') -Encoding utf8
    for ($repetition = 1; $repetition -le $Repetitions; ++$repetition) {
        foreach ($count in $LightCounts) {
            $eligiblePaths = @($Paths | Where-Object { $_ -ne 'forward' -or $count -le 4 })
            for ($offset = 0; $offset -lt $eligiblePaths.Count; ++$offset) {
                $path = $eligiblePaths[($offset + $repetition - 1) % $eligiblePaths.Count]
                if ((Get-FileHash -LiteralPath $binary).Hash -ne $record.binarySha256) {
                    throw 'Renderer binary changed during the measurements.'
                }
                $label = "$Backend-lights$count-$path-r$repetition"
                $run = Join-Path $output $label
                [IO.Directory]::CreateDirectory($run) | Out-Null
                $environment = @{
                    PRISM_RENDER_HEADLESS = '1'; PRISM_RENDER_DETERMINISTIC = '1'
                    PRISM_RENDER_GPU_VALIDATION = '0'; PRISM_RENDER_WIDTH = "$Width"
                    PRISM_RENDER_HEIGHT = "$Height"; PRISM_RENDER_MAX_FRAMES = "$totalFrames"
                    PRISM_RENDER_EXECUTION_MODE = 'threaded'; PRISM_RENDER_TASK_EXECUTOR = 'pool'
                    PRISM_RHI_EXECUTION_MODE = 'native-direct'; PRISM_RENDER_RDG_QUEUE_MODE = 'native'
                    PRISM_RENDER_FRAME_PACING_PROFILE = 'benchmark'; PRISM_RENDER_PROFILING_LEVEL = 'basic'
                    PRISM_RENDER_PERFORMANCE_STANDALONE = '1'; PRISM_RENDER_PERFORMANCE_ACTIVE_VIEWS = 'game'
                    PRISM_RENDER_LIGHTING_PATH = $path; PRISM_RENDER_LIGHTING_COUNT = "$count"
                    PRISM_RENDER_FRAME_PROFILER_PATH = Join-Path $run 'frames.jsonl'
                    PRISM_RENDER_PERFORMANCE_IDENTITY_PATH = Join-Path $run 'identity.json'
                    PRISM_RENDER_QUEUE_COST_MODEL_PATH = Join-Path $run 'queue-cache.json'
                    PRISM_RENDER_LOG_PATH = Join-Path $run 'application.log'
                    PRISM_RENDER_CRASH_REPORT_PATH = Join-Path $run 'crash.json'
                    PRISM_RENDER_MINIDUMP_PATH = Join-Path $run 'crash.dmp'
                }
                $runRecord = [ordered]@{
                    label = $label; repetition = $repetition; order = $offset + 1
                    path = $path; lightCount = $count; status = 'running'; directory = $run
                    binarySha256 = $record.binarySha256; arguments = @("--api=$Backend", '--scene=lights')
                    environment = $environment; startedUtc = [DateTime]::UtcNow.ToString('o')
                }
                $record.runs += $runRecord
                $environment | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $run 'environment.json') -Encoding utf8
                $runRecord | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $run 'run.json') -Encoding utf8
                $record | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $output 'index.json') -Encoding utf8
                Write-Output "Rendering paths: $Backend / $count lights / $path / repetition $repetition"
                try {
                    # The shared launcher creates an isolated ProcessStartInfo,
                    # removes inherited PRISM_RENDER_/PRISM_RHI_ overrides, and
                    # waits for this process before the next GPU run begins.
                    Invoke-ArchitectureProcess -Binary $binary -Arguments $runRecord.arguments -Environment $environment -WorkingDirectory $run -OutputBase (Join-Path $run 'process') -TimeoutSeconds 900
                    if ((Get-FileHash -LiteralPath $binary).Hash -ne $record.binarySha256) {
                        throw 'Renderer binary changed during this run.'
                    }
                    $stderr = Get-Content -LiteralPath (Join-Path $run 'process.stderr.log') -Raw
                    $deferred = if ($path -eq 'deferred') { 1 } else { 0 }
                    $forwardPlus = if ($path -eq 'forward') { 0 } else { 1 }
                    $expectedPath = "[LightingBenchmark] path=$path deferred=$deferred forwardPlus=$forwardPlus GTAO=0 TAA=0"
                    $expectedLights = "[LightingBenchmark] pointLights=$count objects=84"
                    if (-not $stderr.Contains($expectedPath) -or -not $stderr.Contains($expectedLights)) {
                        throw 'Lighting benchmark overrides were not confirmed by the renderer; rebuild the executable.'
                    }
                    foreach ($name in @('frames.jsonl', 'identity.json')) {
                        if (-not (Test-Path -LiteralPath (Join-Path $run $name) -PathType Leaf)) {
                            throw "Required performance evidence was not written: $name"
                        }
                    }
                    $runRecord.status = 'captured'
                } catch {
                    $runRecord.status = 'failed'; $runRecord['error'] = $_.Exception.Message
                    throw
                } finally {
                    $runRecord['finishedUtc'] = [DateTime]::UtcNow.ToString('o')
                    $runRecord['artifacts'] = @(Get-ChildItem -LiteralPath $run -File |
                        Where-Object Name -ne 'run.json' | Sort-Object Name | ForEach-Object {
                            [ordered]@{ path = $_.Name; sha256 = (Get-FileHash -LiteralPath $_.FullName).Hash }
                        })
                    $runRecord | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $run 'run.json') -Encoding utf8
                    $record | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $output 'index.json') -Encoding utf8
                }
            }
        }
    }
    $record.status = 'captured'
} catch {
    $record.status = 'failed'; $record['error'] = $_.Exception.Message
    throw
} finally {
    if ($null -ne $lock) { $lock.Dispose() }
    $record | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $output 'index.json') -Encoding utf8
}
Write-Output (Join-Path $output 'index.json')
