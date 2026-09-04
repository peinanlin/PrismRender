[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$InputPath,
    [Parameter(Mandatory)][string]$OutputPath,
    [ValidateRange(0, 10000)][int]$WarmupFramesPerLevel = 2
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Get-Distribution {
    param([object[]]$Values)
    $numbers = @($Values | Where-Object { $null -ne $_ } |
        ForEach-Object { [double]$_ } | Sort-Object)
    if ($numbers.Count -eq 0) { return $null }
    $medianIndex = [int][math]::Floor(($numbers.Count - 1) * 0.50)
    $p95Index = [int][math]::Floor(($numbers.Count - 1) * 0.95)
    return [ordered]@{
        minimum = $numbers[0]
        median = $numbers[$medianIndex]
        p95 = $numbers[$p95Index]
        maximum = $numbers[-1]
    }
}

$resolvedInput = [IO.Path]::GetFullPath($InputPath)
$resolvedOutput = [IO.Path]::GetFullPath($OutputPath)
if (-not (Test-Path -LiteralPath $resolvedInput -PathType Leaf)) {
    throw "Missing frame profiler report: $resolvedInput"
}
$rows = @([IO.File]::ReadLines($resolvedInput) |
    ForEach-Object { $_ | ConvertFrom-Json })
if ($rows.Count -lt 3 -or $rows[0].type -ne 'header' -or
    $rows[0].format -ne 'PrismFrameProfiler' -or
    $rows[0].version -ne 1 -or $rows[-1].type -ne 'footer' -or
    $rows[-1].status -ne 'complete') {
    throw 'Frame profiler report header/footer is incomplete or incompatible.'
}
$frames = @($rows | Where-Object type -eq 'frame')
if ($frames.Count -ne [int]$rows[-1].completedFrames) {
    throw 'Frame profiler report frame count does not match its footer.'
}
for ($index = 0; $index -lt $frames.Count; ++$index) {
    if ([uint64]$frames[$index].frameId -ne [uint64]($index + 1)) {
        throw 'Frame profiler report IDs are not contiguous and ordered.'
    }
    if ($frames[$index].requestedLevel -ne $frames[$index].actualLevel) {
        throw "Requested/actual profiling level mismatch at frame $($index + 1)."
    }
}

$levelSummaries = [ordered]@{}
foreach ($level in @('off', 'basic', 'detailed', 'capture')) {
    $levelFrames = @($frames | Where-Object actualLevel -eq $level)
    if ($levelFrames.Count -le $WarmupFramesPerLevel) {
        throw "Profiling level '$level' lacks samples after its warmup."
    }
    $samples = @($levelFrames | Select-Object -Skip $WarmupFramesPerLevel)
    $waitTotals = @($samples | ForEach-Object {
        $total = 0.0
        $available = $false
        foreach ($property in $_.waits.PSObject.Properties) {
            if ($null -ne $property.Value) {
                $available = $true
                $total += [double]$property.Value
            }
        }
        if ($available) { $total } else { $null }
    })
    $levelSummaries[$level] = [ordered]@{
        frameCount = $levelFrames.Count
        sampleCount = $samples.Count
        editorLoopMs = Get-Distribution @($samples.editorLoopMs)
        mainActiveMs = Get-Distribution @($samples.lanes.main.activeMs)
        renderActiveMs = Get-Distribution @($samples.lanes.render.activeMs)
        workerActiveMs = Get-Distribution @($samples.lanes.worker.activeMs)
        gpuFrameMs = Get-Distribution @($samples.gpuFrameMs)
        waitTotalMs = Get-Distribution $waitTotals
        profilerOverheadMs = Get-Distribution @($samples.profilerOverheadMs)
        gpuAvailableFrames = @($samples | Where-Object { $null -ne $_.gpuFrameMs }).Count
    }
}

$summary = [ordered]@{
    format = 'PrismFrameProfilerLevelSummary'
    version = 1
    source = $resolvedInput
    metadata = $rows[0].metadata
    exporterTiming = $rows[0].exporterTiming
    warmupFramesPerLevel = $WarmupFramesPerLevel
    levels = $levelSummaries
    notes = @(
        'Editor Loop is measured independently for each completed frame.',
        'Render is a Main-lane sub-interval until the threaded execution phase.',
        'GPU is a resolved historical-frame sample and is never summed across views.',
        'JSONL export occurs outside Editor Loop and is excluded from profilerOverheadMs.'
    )
}
$parent = Split-Path -Parent $resolvedOutput
if ($parent) { [IO.Directory]::CreateDirectory($parent) | Out-Null }
[IO.File]::WriteAllText(
    $resolvedOutput,
    ($summary | ConvertTo-Json -Depth 12),
    [Text.UTF8Encoding]::new($false))
Write-Output $resolvedOutput
