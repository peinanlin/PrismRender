[CmdletBinding()]
param(
    [Parameter(Mandatory)][string[]]$RunDirectories,
    [Parameter(Mandatory)][string]$ProfilerLevelSummary,
    [Parameter(Mandatory)][string]$DetailedPerformanceSummary,
    [Parameter(Mandatory)][string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'ArchitectureValidation.Common.ps1')

function Get-RunDistribution([object[]]$Values) {
    $numbers = @($Values | Where-Object { $null -ne $_ } |
        ForEach-Object { [double]$_ } | Sort-Object)
    if ($numbers.Count -eq 0) { return $null }
    return [ordered]@{
        count = $numbers.Count
        minimum = $numbers[0]
        median = $numbers[[math]::Floor(($numbers.Count - 1) * 0.5)]
        maximum = $numbers[-1]
        values = $numbers
    }
}

function Read-RequiredJson([string]$Path, [string]$Description) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Missing ${Description}: $Path"
    }
    return Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
}

if ($RunDirectories.Count -lt 3) {
    throw 'At least three independent performance scenario matrices are required.'
}
$runs = @()
foreach ($directory in $RunDirectories) {
    $resolved = Resolve-ArchitectureOutputPath $root $directory
    $summaryPath = Join-Path $resolved 'summary.json'
    $summary = Read-RequiredJson $summaryPath 'scenario summary'
    if ($summary.format -ne 'PrismArchitecturePerformanceBenchmark' -or
        $summary.version -ne 1 -or
        $summary.status -ne 'measured-and-validated' -or
        @($summary.results).Count -ne 6) {
        throw "Incomplete performance scenario matrix: $summaryPath"
    }
    $runs += [pscustomobject]@{
        directory = $resolved
        summaryPath = $summaryPath
        summary = $summary
    }
}

$reference = $runs[0].summary
$matchedFields = @('backend', 'buildConfiguration', 'validation',
    'profilingLevel', 'queueMode', 'width', 'height', 'warmupFrames',
    'sampleFrames', 'drainFrames')
foreach ($run in $runs | Select-Object -Skip 1) {
    foreach ($field in $matchedFields) {
        if (($reference.$field | ConvertTo-Json -Compress) -ne
            ($run.summary.$field | ConvertTo-Json -Compress)) {
            throw "Performance scenario input mismatch: $field"
        }
    }
    if ((Get-FileHash -LiteralPath $reference.scenarioDefinition).Hash -ne
        (Get-FileHash -LiteralPath $run.summary.scenarioDefinition).Hash) {
        throw 'Performance scenario definitions differ between runs.'
    }
}

$scenarioIds = @($reference.results.id | Sort-Object)
if ($scenarioIds.Count -ne 6 -or
    @($scenarioIds | Select-Object -Unique).Count -ne 6) {
    throw 'Reference scenario matrix does not contain six unique scenarios.'
}
foreach ($run in $runs) {
    $candidateIds = @($run.summary.results.id | Sort-Object)
    if (@(Compare-Object $scenarioIds $candidateIds).Count -ne 0) {
        throw 'Performance scenario coverage differs between runs.'
    }
}

$scenarioResults = [ordered]@{}
foreach ($id in $scenarioIds) {
    $samples = @($runs | ForEach-Object {
        @($_.summary.results | Where-Object id -eq $id)[0]
    })
    $executableHashes = @($samples.executableHash | Select-Object -Unique)
    $adapterIdentities = @($samples | ForEach-Object {
        "$($_.adapter.vendorId):$($_.adapter.deviceId):$($_.adapter.driverVersion)"
    } | Select-Object -Unique)
    if ($executableHashes.Count -ne 1 -or $adapterIdentities.Count -ne 1) {
        throw "Runtime identity changed between scenario repeats: $id"
    }
    $scenarioResults[$id] = [ordered]@{
        workload = $samples[0].workload
        binaryKind = $samples[0].binaryKind
        activeViews = $samples[0].activeViews
        executableHash = $executableHashes[0]
        adapterIdentity = $adapterIdentities[0]
        actualLoopFps = Get-RunDistribution @($samples.actualLoopFps.median)
        editorLoopMs = Get-RunDistribution @($samples.editorLoopMs.median)
        mainActiveMs = Get-RunDistribution @($samples.lanes.mainActiveMs.median)
        mainWaitMs = Get-RunDistribution @($samples.lanes.mainWaitMs.median)
        renderActiveMs = Get-RunDistribution @($samples.lanes.renderActiveMs.median)
        workerActiveMs = Get-RunDistribution @($samples | ForEach-Object {
            if ($null -eq $_.lanes.workerActiveMs) { $null }
            else { $_.lanes.workerActiveMs.median }
        })
        gameCpuMs = Get-RunDistribution @($samples.views.gameCpuMs.median)
        sceneCpuMs = Get-RunDistribution @($samples | ForEach-Object {
            if ($null -eq $_.views.sceneCpuMs) { $null }
            else { $_.views.sceneCpuMs.median }
        })
        gpuCriticalHintMs = Get-RunDistribution @($samples.gpuCriticalHintMs.median)
        profilerOverheadMs = Get-RunDistribution @($samples.profilerOverheadMs.median)
        waits = [ordered]@{
            frameFenceMs = Get-RunDistribution @($samples.waits.'frame-fence'.median)
            imageFenceMs = Get-RunDistribution @($samples.waits.'image-fence'.median)
            submitMs = Get-RunDistribution @($samples.waits.submit.median)
            nativePresentMs = Get-RunDistribution @($samples.waits.'native-present'.median)
        }
    }
}

$comparisons = [ordered]@{}
foreach ($workload in @('empty', 'preview')) {
    $dual = $scenarioResults["$workload-editor-dual-view"]
    $editorGame = $scenarioResults["$workload-editor-game-only"]
    $standalone = $scenarioResults["$workload-standalone-game-only"]
    $comparisons[$workload] = [ordered]@{
        dualViewFps = $dual.actualLoopFps.median
        editorGameOnlyFps = $editorGame.actualLoopFps.median
        standaloneFps = $standalone.actualLoopFps.median
        editorGameOnlyToDualViewFpsRatio =
            $editorGame.actualLoopFps.median / $dual.actualLoopFps.median
        standaloneToEditorGameOnlyFpsRatio =
            $standalone.actualLoopFps.median / $editorGame.actualLoopFps.median
        dualViewSceneCpuMs = $dual.sceneCpuMs.median
        dualViewMainWaitMs = $dual.mainWaitMs.median
        dualViewGpuCriticalHintMs = $dual.gpuCriticalHintMs.median
    }
}

$levelPath = [IO.Path]::GetFullPath($ProfilerLevelSummary, $root)
$levels = Read-RequiredJson $levelPath 'same-process profiler level summary'
if ($levels.format -ne 'PrismFrameProfilerLevelSummary' -or
    $levels.version -ne 1 -or
    $levels.metadata.profilingSequence -ne
        'off:60,basic:60,detailed:60,capture:60') {
    throw 'Profiler sidecar is not the required 240-frame same-process sequence.'
}
$detailedPath = [IO.Path]::GetFullPath($DetailedPerformanceSummary, $root)
$detailed = Read-RequiredJson $detailedPath 'memory/PSO performance summary'
if ($detailed.format -ne 'PrismPerformanceSummary' -or
    $detailed.version -ne 1 -or
    $null -eq $detailed.distributions.'memory.workingSetBytes' -or
    $null -eq $detailed.pso) {
    throw 'Detailed performance sidecar lacks memory or PSO evidence.'
}

$output = Resolve-ArchitectureOutputPath $root $OutputDirectory
if (Test-Path -LiteralPath $output) {
    throw "Refusing to overwrite performance scenario aggregate: $output"
}
[IO.Directory]::CreateDirectory($output) | Out-Null
$report = [ordered]@{
    format = 'PrismArchitecturePerformanceScenarioAggregate'
    version = 1
    status = 'passed'
    independentMatrixCount = $runs.Count
    protocol = [ordered]@{
        backend = $reference.backend
        buildConfiguration = $reference.buildConfiguration
        validation = $reference.validation
        profilingLevel = $reference.profilingLevel
        queueMode = $reference.queueMode
        width = $reference.width
        height = $reference.height
        warmupFrames = $reference.warmupFrames
        sampleFrames = $reference.sampleFrames
        drainFrames = $reference.drainFrames
    }
    sources = @($runs | ForEach-Object {
        [ordered]@{
            directory = $_.directory
            summarySha256 = (Get-FileHash -LiteralPath $_.summaryPath).Hash
        }
    })
    scenarios = $scenarioResults
    comparisons = $comparisons
    profilerLevelSidecar = [ordered]@{
        source = $levelPath
        sha256 = (Get-FileHash -LiteralPath $levelPath).Hash
        processScope = 'one adjacent 240-frame process'
        levels = $levels.levels
    }
    memoryAndPsoSidecar = [ordered]@{
        source = $detailedPath
        sha256 = (Get-FileHash -LiteralPath $detailedPath).Hash
        protocolNote = 'RelWithDebInfo detailed recorder; condition evidence, not directly compared with Release profiler matrices.'
        workingSetBytes = $detailed.distributions.'memory.workingSetBytes'
        peakWorkingSetBytes = $detailed.distributions.'memory.peakWorkingSetBytes'
        privateCommitBytes = $detailed.distributions.'memory.privateCommitBytes'
        retirementPending = $detailed.distributions.'retirement.pending'
        pso = $detailed.pso
    }
    conclusions = @(
        'Editor dual-view FPS includes two complete renderer views and editor work.',
        'Editor game-only retains the editor shell but does not submit Scene renderer work.',
        'Standalone removes editor and second-view CPU work; it is the closest Player-style throughput comparison.',
        'Render remains an inline Main-lane sub-interval until P7; Worker is correctly unavailable.',
        'GPU view/pass intervals are historical resolved samples and are never summed or inverted into FPS.',
        'DVFS and display state remain reported conditions, not a machine-level prerequisite.'
    )
}
$reportPath = Join-Path $output 'report.json'
[IO.File]::WriteAllText(
    $reportPath,
    ($report | ConvertTo-Json -Depth 20),
    [Text.UTF8Encoding]::new($false))
Write-Output $reportPath
