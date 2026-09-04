[CmdletBinding()]
param(
    [ValidateSet('d3d12','vulkan')][string]$Backend = 'd3d12',
    [string]$Scene = 'preview',
    [ValidateRange(1,32)][int]$Runs = 8,
    [ValidateRange(60,10000)][int]$WarmupFrames = 180,
    [ValidateRange(180,10000)][int]$SampleFrames = 900,
    [ValidateRange(0,31)][int]$GpuIndex = 0,
    [ValidateRange(20,1000)][int]$PollingMilliseconds = 100,
    [string]$BinaryPath = 'build-windows-ci/RelWithDebInfo/PrismRender.exe',
    [Parameter(Mandatory)][string]$OutputDirectory,
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'ArchitectureValidation.Common.ps1')
. (Join-Path $PSScriptRoot 'ArchitecturePerformance.Common.ps1')
. (Join-Path $PSScriptRoot 'ArchitectureGpuClockDiagnostic.Common.ps1')
$output = Resolve-ArchitectureOutputPath $root $OutputDirectory
if (Test-Path -LiteralPath $output) { throw "Refusing to overwrite GPU diagnostic: $output" }
$binary = [IO.Path]::GetFullPath($BinaryPath, $root)
$smi = Get-Command nvidia-smi -ErrorAction SilentlyContinue
if (-not $smi) { throw 'nvidia-smi is required for this NVIDIA-only diagnostic.' }
$query = 'timestamp,index,name,pstate,clocks.current.graphics,clocks.current.sm,clocks.current.memory,utilization.gpu,power.draw,power.limit,temperature.gpu'
$record = [ordered]@{ format = 'PrismArchitectureGpuClockDiagnostic'; version = 1; status = 'planned'
    diagnosticOnly = $true; backend = $Backend; scene = $Scene; runsRequested = $Runs; gpuIndex = $GpuIndex
    pollingMilliseconds = $PollingMilliseconds; warmupFrames = $WarmupFrames; sampleFrames = $SampleFrames
    binary = $binary; binarySha256 = if (Test-Path -LiteralPath $binary) { (Get-FileHash $binary).Hash } else { $null }
    driverSha256 = (Get-FileHash $PSCommandPath).Hash
    commonSha256 = (Get-FileHash (Join-Path $PSScriptRoot 'ArchitectureGpuClockDiagnostic.Common.ps1')).Hash
    runs = @(); limitations = @(
        'NVIDIA telemetry polling is an observer and may perturb timing; these samples are diagnostic-only and cannot pass V3.',
        'Telemetry is correlated at process granularity, not assigned to individual renderer frames.',
        'This tool never changes clocks, power policy, rendering settings, synchronization, or thresholds.') }
if ($DryRun) { $record | ConvertTo-Json -Depth 8; return }
if (-not (Test-Path -LiteralPath $binary -PathType Leaf)) { throw "Missing renderer: $binary" }
New-Item -ItemType Directory -Path $output | Out-Null
$record.status = 'running'
$record | ConvertTo-Json -Depth 10 | Set-Content (Join-Path $output 'index.json') -Encoding utf8
try {
    $validRuns = 0
    for ($run = 1; $run -le $Runs; ++$run) {
        $runDirectory = Join-Path $output "run-$run"
        New-Item -ItemType Directory -Path $runDirectory | Out-Null
        $telemetryPath = Join-Path $runDirectory 'nvidia-smi.csv'
        $arguments = @("--id=$GpuIndex", "--query-gpu=$query", '--format=csv,noheader,nounits',
            "--loop-ms=$PollingMilliseconds", "--filename=$telemetryPath")
        $observer = $null
        $runError = $null
        $startedUtc = [DateTime]::UtcNow.ToString('o')
        try {
            $observer = Start-Process -FilePath $smi.Source -ArgumentList $arguments -WindowStyle Hidden -PassThru
            & (Join-Path $PSScriptRoot 'Measure-ArchitecturePerformance.ps1') -Backend $Backend -Scene $Scene `
                -VisibleWindow -UnfocusedWindow -WarmupFrames $WarmupFrames -SampleFrames $SampleFrames `
                -WindowX 100 -WindowY 100 -BinaryPath $binary -OutputDirectory (Join-Path $runDirectory 'performance')
        } catch {
            $runError = $_.Exception.Message
        } finally {
            if ($observer -and -not $observer.HasExited) { Stop-Process -Id $observer.Id -ErrorAction Stop; $observer.WaitForExit() }
        }
        $finishedUtc = [DateTime]::UtcNow.ToString('o')
        $telemetry = @(ConvertFrom-ArchitectureNvidiaSmiCsv $telemetryPath)
        $telemetrySummary = Get-ArchitectureNvidiaSmiSummary $telemetry
        $applicationLog = Join-Path $runDirectory 'performance/application.log'
        $events = if (Test-Path -LiteralPath $applicationLog) {
            @(Get-Content $applicationLog | ForEach-Object { $_ | ConvertFrom-Json })
        } else { @() }
        $initialize = @($events | Where-Object event -eq 'renderer.initialize.completed')
        $exitEvent = @($events | Where-Object event -eq 'process.exit')
        $firstTelemetryUtc = ConvertTo-ArchitectureNvidiaTimestampUtc $telemetry[0].timestamp
        $lastTelemetryUtc = ConvertTo-ArchitectureNvidiaTimestampUtc $telemetry[-1].timestamp
        $coverageComplete = $initialize.Count -eq 1 -and $exitEvent.Count -eq 1 -and
            $firstTelemetryUtc -le $initialize[0].timestampUtc.ToUniversalTime() -and
            $lastTelemetryUtc -ge $exitEvent[0].timestampUtc.ToUniversalTime()
        $performanceIndex = Join-Path $runDirectory 'performance/index.json'
        $entry = [ordered]@{ run = $run; status = if ($runError -or -not $coverageComplete) { 'failed' } else { 'valid' }
            startedUtc = $startedUtc; finishedUtc = $finishedUtc
            performanceIndexSha256 = (Get-FileHash $performanceIndex).Hash
            telemetrySha256 = (Get-FileHash $telemetryPath).Hash; telemetry = $telemetrySummary
            telemetryCoverage = @{ complete = $coverageComplete; firstUtc = $firstTelemetryUtc.ToString('o')
                lastUtc = $lastTelemetryUtc.ToString('o'); rendererInitializeUtc = if ($initialize.Count -eq 1) { $initialize[0].timestampUtc.ToUniversalTime().ToString('o') } else { $null }
                processExitUtc = if ($exitEvent.Count -eq 1) { $exitEvent[0].timestampUtc.ToUniversalTime().ToString('o') } else { $null } } }
        if ($runError) { $entry['error'] = $runError }
        elseif (-not $coverageComplete) { $entry['error'] = 'NVIDIA telemetry did not cover the renderer execution interval.' }
        else {
            # Measure-ArchitecturePerformance already parsed every raw frame and sealed
            # this summary. Avoid immediately reparsing a large file while Windows may
            # still expose a transient cached view after the child process exits.
            $child = Get-Content $performanceIndex -Raw | ConvertFrom-Json -AsHashtable
            if ($child.status -ne 'measured-and-validated') { throw 'Diagnostic child was not validated.' }
            $summaryPath = Join-Path $runDirectory 'performance/summary.json'
            $sealedSummary = @($child.artifacts | Where-Object path -eq 'summary.json')
            if ($sealedSummary.Count -ne 1 -or (Get-FileHash $summaryPath).Hash -ne $sealedSummary[0].sha256) {
                throw 'Diagnostic child summary is not sealed.'
            }
            $summary = Get-Content $summaryPath -Raw | ConvertFrom-Json -AsHashtable
            $metric = "gpu.$($summary.activeViews.Split(',')[-1]).Graphics.Renderer"
            if (-not $summary.distributions.Contains($metric)) { throw "Missing target GPU metric: $metric" }
            $entry['targetMetric'] = $metric; $entry['targetDistribution'] = $summary.distributions[$metric]
            ++$validRuns
        }
        $entry | ConvertTo-Json -Depth 10 | Set-Content (Join-Path $runDirectory 'summary.json') -Encoding utf8
        $record.runs += $entry
        $record | ConvertTo-Json -Depth 12 | Set-Content (Join-Path $output 'index.json') -Encoding utf8
    }
    if ($validRuns -eq 0) { throw 'Every GPU clock diagnostic run failed performance validation.' }
    $record.status = if ($validRuns -eq $Runs) { 'diagnostic-complete' } else { 'diagnostic-complete-with-run-failures' }
} catch {
    $record.status = 'failed'; $record['error'] = $_.Exception.Message; throw
} finally {
    $record['artifacts'] = @(Get-ChildItem -LiteralPath $output -Recurse -File | Where-Object FullName -ne (Join-Path $output 'index.json') |
        ForEach-Object { @{ path = [IO.Path]::GetRelativePath($output, $_.FullName); sha256 = (Get-FileHash $_.FullName).Hash } })
    $record | ConvertTo-Json -Depth 12 | Set-Content (Join-Path $output 'index.json') -Encoding utf8
}
Write-Output (Join-Path $output 'index.json')
