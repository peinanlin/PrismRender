[CmdletBinding()]
param([Parameter(Mandatory)][string]$InputDirectory, [Parameter(Mandatory)][string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'ArchitectureValidation.Common.ps1')
. (Join-Path $PSScriptRoot 'ArchitectureGpuClockDiagnostic.Common.ps1')
$input = Resolve-ArchitectureOutputPath $root $InputDirectory
$output = Resolve-ArchitectureOutputPath $root $OutputDirectory
if (-not (Test-Path -LiteralPath $input -PathType Container)) { throw "Missing diagnostic input: $input" }
if (Test-Path -LiteralPath $output) { throw "Refusing to overwrite GPU diagnostic summary: $output" }
if ($output.StartsWith($input + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Diagnostic summary output cannot be inside its input.'
}
$indexPath = Join-Path $input 'index.json'
$index = Get-Content $indexPath -Raw | ConvertFrom-Json -AsHashtable
if ($index.format -ne 'PrismArchitectureGpuClockDiagnostic' -or $index.version -ne 1 -or -not $index.diagnosticOnly) {
    throw 'Invalid GPU clock diagnostic index.'
}
foreach ($artifact in $index.artifacts) {
    $path = [IO.Path]::GetFullPath($artifact.path, $input)
    if (-not $path.StartsWith($input + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) -or
        (Get-FileHash -LiteralPath $path).Hash -ne $artifact.sha256) { throw 'GPU diagnostic artifact path/hash mismatch.' }
}
$runs = @()
foreach ($entry in $index.runs) {
    $runDirectory = Join-Path $input "run-$($entry.run)"
    $summaryPath = Join-Path $runDirectory 'summary.json'
    $summary = Get-Content $summaryPath -Raw | ConvertFrom-Json -AsHashtable
    $telemetry = @(ConvertFrom-ArchitectureNvidiaSmiCsv (Join-Path $runDirectory 'nvidia-smi.csv'))
    $events = @(Get-Content (Join-Path $runDirectory 'performance/application.log') | ForEach-Object { $_ | ConvertFrom-Json })
    $initialize = @($events | Where-Object event -eq 'renderer.initialize.completed')
    $exitEvent = @($events | Where-Object event -eq 'process.exit')
    $firstUtc = ConvertTo-ArchitectureNvidiaTimestampUtc $telemetry[0].timestamp
    $lastUtc = ConvertTo-ArchitectureNvidiaTimestampUtc $telemetry[-1].timestamp
    $coverage = $initialize.Count -eq 1 -and $exitEvent.Count -eq 1 -and
        $firstUtc -le $initialize[0].timestampUtc.ToUniversalTime() -and
        $lastUtc -ge $exitEvent[0].timestampUtc.ToUniversalTime()
    $performanceIndex = Get-Content (Join-Path $runDirectory 'performance/index.json') -Raw | ConvertFrom-Json -AsHashtable
    $valid = $summary.status -eq 'valid' -and $performanceIndex.status -eq 'measured-and-validated' -and $coverage
    $row = [ordered]@{ run = $entry.run; valid = $valid; telemetryCoverageComplete = $coverage
        performanceStatus = $performanceIndex.status; telemetrySamples = $telemetry.Count
        graphicsClockMedianMHz = $summary.telemetry.graphicsClockMHz.median
        powerMedianWatts = $summary.telemetry.powerWatts.median
        temperatureMedianCelsius = $summary.telemetry.temperatureCelsius.median }
    if ($valid) {
        $stored = Get-Content (Join-Path $runDirectory 'performance/summary.json') -Raw | ConvertFrom-Json -AsHashtable
        $row['sceneRendererMedianMs'] = $stored.distributions['gpu.scene.Graphics.Renderer'].median
        $row['gameRendererMedianMs'] = $stored.distributions['gpu.game.Graphics.Renderer'].median
        $row['cpuFrameMedianMs'] = $stored.distributions['cpu.frameMs'].median
    } else { $row['reason'] = if (-not $coverage) { 'telemetry-incomplete' } else { $summary.error } }
    $runs += $row
}
$validRuns = @($runs | Where-Object valid)
$report = [ordered]@{ format = 'PrismArchitectureGpuClockDiagnosticSummary'; version = 1; diagnosticOnly = $true
    sourceDirectory = $input; sourceIndexSha256 = (Get-FileHash $indexPath).Hash
    totalRuns = $runs.Count; validCorrelatedRuns = $validRuns.Count; invalidRuns = $runs.Count - $validRuns.Count
    correlations = @{ sceneRendererVsGraphicsClock = Get-ArchitecturePearsonCorrelation $validRuns sceneRendererMedianMs graphicsClockMedianMHz
        gameRendererVsGraphicsClock = Get-ArchitecturePearsonCorrelation $validRuns gameRendererMedianMs graphicsClockMedianMHz
        cpuFrameVsGraphicsClock = Get-ArchitecturePearsonCorrelation $validRuns cpuFrameMedianMs graphicsClockMedianMHz }
    runs = $runs; limitations = @(
        'Correlation is process-level and does not prove that GPU dynamic frequency causes the renderer timing mode.',
        'nvidia-smi polling includes initialization and post-processing; only runs covering renderer initialization through process exit are correlated.',
        'Observer runs are diagnostic-only, may perturb timing, and never replace the unobserved V3 baseline.') }
New-Item -ItemType Directory -Path $output | Out-Null
$report | ConvertTo-Json -Depth 10 | Set-Content (Join-Path $output 'report.json') -Encoding utf8
Write-Output (Join-Path $output 'report.json')
