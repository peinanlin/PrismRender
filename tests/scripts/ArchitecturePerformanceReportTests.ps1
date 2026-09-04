$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. (Join-Path $root 'scripts/ArchitecturePerformance.Common.ps1')
. (Join-Path $root 'scripts/ArchitecturePerformanceReport.Common.ps1')
$output = Join-Path $root ('artifacts/architecture-refactor/tool-tests/performance-report-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $output | Out-Null
$results = [Collections.Generic.List[object]]::new()
function Check([string]$Name, [scriptblock]$Body, [switch]$Reject) {
    $errorText = ''
    try { & $Body | Out-Null } catch { $errorText = $_.Exception.Message }
    $passed = if ($Reject) { [bool]$errorText } else { -not $errorText }
    $results.Add(@{ name = $Name; passed = $passed; error = $errorText })
    if (-not $passed) { throw "Failed report test $Name : $errorText" }
}
function Save-Json($Value, [string]$Path) { $Value | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $Path -Encoding utf8 }
function Load-Json([string]$Path) { Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json -AsHashtable }
function Seal-Child([string]$Path) {
    $child = Load-Json (Join-Path $Path 'index.json')
    $child.artifacts = @(Get-ChildItem -LiteralPath $Path -File | Where-Object Name -ne 'index.json' | ForEach-Object {
        @{ path = $_.Name; sha256 = (Get-FileHash $_.FullName).Hash }
    })
    Save-Json $child (Join-Path $Path 'index.json')
}
function Seal-Root([string]$Path) {
    $index = Load-Json (Join-Path $Path 'index.json')
    foreach ($run in $index.runs) { $run.indexSha256 = (Get-FileHash (Join-Path $run.path 'index.json')).Hash }
    $index.artifacts = @(Get-ChildItem -LiteralPath $Path -File | Where-Object Name -ne 'index.json' | ForEach-Object {
        @{ path = $_.Name; sha256 = (Get-FileHash $_.FullName).Hash }
    })
    Save-Json $index (Join-Path $Path 'index.json')
}
# Isolate batch/audit rules from the raw JSONL reader; the latter has its own
# positive/negative suite. Real finalized evidence is also audited separately.
function Read-ArchitecturePerformanceRun([string]$Directory) {
    return @{ index = (Load-Json (Join-Path $Directory 'index.json'))
        summary = (Load-Json (Join-Path $Directory 'fixture-summary.json'))
        sourceHash = 'fixed-source'; identity = @{ graphicsApi = 'd3d12'; adapterName = 'fixture'; vendorId = 1; deviceId = 2
            driverVersion = 'fixture'; apiVersion = 'fixture'; cpuName = 'fixture'; buildConfiguration = 'fixture'; compiler = 'fixture'; architecture = 'fixture' } }
}
function Make-Baseline([string]$Name, [double]$SecondBatchScale = 1, [switch]$Interrupted) {
    $path = Join-Path $output $Name
    New-Item -ItemType Directory -Path $path | Out-Null
    $index = @{ format = 'PrismArchitecturePerformanceBaseline'; version = 1; status = 'baseline-repeatability-passed'
        backends = @('d3d12'); scenes = @('preview'); batches = 2; warmupRunsPerBatch = 3; measurementRunsPerBatch = 5
        warmupFramesPerRun = 60; sampleFramesPerRun = 180; drainFramesPerRun = 8; runs = @(); comparisons = @(); artifacts = @() }
    $batches = @{}
    for ($batch = 1; $batch -le 2; ++$batch) {
        $samples = @()
        for ($run = 1; $run -le 8; ++$run) {
            if ($Interrupted -and $batch -eq 2 -and $run -gt 2) { break }
            $name = "d3d12-preview-b$batch-r$run"; $childPath = Join-Path $path $name
            New-Item -ItemType Directory -Path $childPath | Out-Null
            $scale = if ($batch -eq 2) { $SecondBatchScale } else { 1 }
            $summary = @{ sampleFrames = 180; activeViews = 'game'; pacingVersion = 1; presentationConditions = @{ focused = $true }
                pso = @{ graphicsCreatedDuringMeasurement = 0 }; distributions = [ordered]@{} }
            foreach ($metric in @('cpu.frameMs','cpu.loopIntervalMs','gpu.game.Graphics.Renderer','pacing.frameFenceMs')) {
                $summary.distributions[$metric] = Get-ArchitectureDistribution @((10 * $scale), (10 * $scale))
            }
            $summary.distributions['pacing.acquireMs'] = Get-ArchitectureDistribution @(0,0)
            Save-Json $summary (Join-Path $childPath 'fixture-summary.json')
            $child = @{ format = 'PrismArchitecturePerformanceRun'; version = 1; status = 'measured-and-validated'; backend = 'd3d12'
                scene = 'preview'; view = 'game'; queueMode = 'native'; width = 1280; height = 800; warmupFrames = 60; sampleFrames = 180; drainFrames = 8
                cachePolicy = 'fixture'; initialImGuiSha256 = 'fixture'; binarySha256 = 'fixture'; driverSha256 = 'fixture'; helperSha256 = 'fixture'
                environment = @{}; artifacts = @() }
            if ($Interrupted -and $batch -eq 2 -and $run -eq 2) { $child.status = 'failed'; $child.error = 'Fixture focus changed.' }
            Save-Json $child (Join-Path $childPath 'index.json'); Seal-Child $childPath
            if ($child.status -eq 'failed') { continue }
            $index.runs += @{ case = 'd3d12-preview'; batch = $batch; run = $run; role = $(if ($run -le 3) { 'warmup' } else { 'measurement' })
                path = $childPath; indexSha256 = (Get-FileHash (Join-Path $childPath 'index.json')).Hash }
            if ($run -gt 3) { $samples += $summary }
        }
        if ($samples.Count -eq 5) {
            $batches[$batch] = Get-ArchitecturePerformanceBatchSummary $samples
            Save-Json $batches[$batch] (Join-Path $path "d3d12-preview-b$batch-summary.json")
        }
    }
    if ($batches.Count -eq 2) {
        $forward = Compare-ArchitecturePerformanceDistributions $batches[1] $batches[2]
        $reverse = Compare-ArchitecturePerformanceDistributions $batches[2] $batches[1]
        $index.comparisons += @{ case = 'd3d12-preview'; passed = $forward.passed -and $reverse.passed; batch1To2 = $forward; batch2To1 = $reverse }
        if (-not $index.comparisons[0].passed) { $index.status = 'failed'; $index.error = 'V3 fixture regression.' }
    } else { $index.status = 'failed'; $index.error = 'Fixture interrupted.' }
    Save-Json $index (Join-Path $path 'index.json'); Seal-Root $path
    return $path
}
function Make-ConditionRow([int]$Frame, [bool]$Focused) {
    return @{ type = 'cpu'; frameId = $Frame; views = @{ game = @{ slot = 0 } }; resources = @{
        pacing = @{ api = 'd3d12'; presentResult = 0; acquireResult = 0; waitResult = 0; presentMode = -1
            syncInterval = 1; presentFlags = 0; slot = 0; generation = $Frame; image = 0; imageCount = 2
            beginCompleted = $true; endCompleted = $true }
        window = @{ x = 100; y = 100; monitorX = 0; monitorY = 0; width = 1280; height = 800
            framebufferWidth = 1280; framebufferHeight = 800; monitorWidth = 2560; monitorHeight = 1440; refreshHz = 200
            visible = $true; focused = $Focused; minimized = $false; displayAvailable = $true; scaleX = 1; scaleY = 1; displayName = 'fixture' } } }
}
try {
    $valid = Make-Baseline 'valid'
    Check 'complete-and-zero-phase' {
        $report = Read-ArchitecturePerformanceBaseline $valid
        if (-not $report.passed -or $report.formalFrames -ne 1800 -or $report.executedRuns -ne 16) { throw 'Wrong complete report.' }
        $zero = $report.cases[0].diagnosticChanges | Where-Object metric -eq 'pacing.acquireMs'
        if ($null -ne $zero.medianChange -or $zero.medianDelta -ne 0) { throw 'Zero baseline ratio must be null.' }
        $report | ConvertTo-Json -Depth 20 | Out-Null
    }
    $regression = Make-Baseline 'regression' 1.06
    Check 'complete-but-failed-v3' {
        $report = Read-ArchitecturePerformanceBaseline $regression
        if ($report.passed -or -not $report.complete) { throw 'Unstable baseline accepted.' }
    }
    $reverse = Make-Baseline 'reverse-regression' 0.94
    Check 'bidirectional-repeatability' { if ((Read-ArchitecturePerformanceBaseline $reverse).passed) { throw 'Reverse regression missed.' } }
    $partial = Make-Baseline 'partial' -Interrupted
    Check 'failed-child-not-lost' {
        $report = Read-ArchitecturePerformanceBaseline $partial
        if ($report.complete -or $report.passed -or $report.executedRuns -ne 10 -or $report.registeredValidRuns -ne 9 -or
            $report.failedRuns -ne 1 -or $report.missingRuns -ne 6 -or $report.formalFrames -ne 900) { throw 'Wrong partial counts.' }
    }
    $conditionFile = Join-Path $output 'conditions.jsonl'
    @((Make-ConditionRow 1 $false), (Make-ConditionRow 2 $true), (Make-ConditionRow 3 $false), (Make-ConditionRow 4 $true)) |
        ForEach-Object { $_ | ConvertTo-Json -Compress -Depth 8 } | Set-Content $conditionFile
    Check 'exact-condition-boundary-excludes-warmup-drain' {
        $d = Get-ArchitecturePerformanceConditionTransitions $conditionFile 1 2
        if ($d.observedCpuRows -ne 2 -or $d.transitionCount -ne 1 -or $d.transitions[0].frame -ne 3 -or
            $d.transitions[0].fields.Count -ne 1 -or $d.transitions[0].fields[0] -ne 'focused') { throw 'Wrong condition boundary.' }
    }
    $bad = Make-ConditionRow 2 $true; $bad.resources.pacing.presentResult = 142213121
    $bad | ConvertTo-Json -Compress -Depth 8 | Set-Content $conditionFile
    Check 'occluded-remains-invalid' {
        $d = Get-ArchitecturePerformanceConditionTransitions $conditionFile 1 2
        if ($d.invalidConditionRows -ne 1 -or $d.invalidExamples[0].frame -ne 2 -or $null -ne $d.firstConditions) { throw 'Occluded sample accepted.' }
    }
    @(1..140 | ForEach-Object { Make-ConditionRow $_ ([bool]($_ % 2)) }) |
        ForEach-Object { $_ | ConvertTo-Json -Compress -Depth 8 } | Set-Content $conditionFile
    Check 'bounded-examples-retain-total' {
        $d = Get-ArchitecturePerformanceConditionTransitions $conditionFile 0 140
        if ($d.transitionCount -ne 139 -or $d.transitions.Count -ne 64) { throw 'Condition totals truncated.' }
    }
    foreach ($mutation in @('running','false-success','duplicate-run','role','outside-run','index-hash','child-hash','batch-value','gate-value','binary-change','extra-folder','missing-comparison','missing-warmup','unsealed-summary')) {
        $path = Make-Baseline $mutation
        $indexPath = Join-Path $path 'index.json'; $index = Load-Json $indexPath
        $firstPath = Join-Path $path 'd3d12-preview-b1-r1'
        switch ($mutation) {
            'running' { $index.status = 'running' }
            'false-success' { $index.status = 'failed' }
            'duplicate-run' { $index.runs += $index.runs[0] }
            'role' { $index.runs[0].role = 'measurement' }
            'outside-run' { $index.runs[0].path = $output }
            'index-hash' { $index.runs[0].indexSha256 = 'wrong' }
            'child-hash' { Add-Content -LiteralPath (Join-Path $firstPath 'fixture-summary.json') -Value ' ' }
            'batch-value' {
                $batchPath = Join-Path $path 'd3d12-preview-b1-summary.json'; $batch = Load-Json $batchPath
                $batch.distributions['cpu.frameMs'].runMedians[0] = 99; Save-Json $batch $batchPath
                Seal-Root $path; $index = Load-Json $indexPath
            }
            'gate-value' { $index.comparisons[0].batch1To2.metrics[0].medianChange = 0.5 }
            'binary-change' {
                $childPath = Join-Path $firstPath 'index.json'; $child = Load-Json $childPath
                $child.binarySha256 = 'changed'; Save-Json $child $childPath
                Seal-Root $path; $index = Load-Json $indexPath
            }
            'extra-folder' { New-Item -ItemType Directory -Path (Join-Path $path 'unexpected') | Out-Null }
            'missing-comparison' { $index.comparisons = @() }
            'missing-warmup' { $index.runs = @($index.runs | Where-Object { -not ($_.batch -eq 1 -and $_.run -eq 1) }) }
            'unsealed-summary' { $index.artifacts = @($index.artifacts | Where-Object path -ne 'd3d12-preview-b1-summary.json') }
        }
        Save-Json $index $indexPath
        Check "reject-$mutation" { Read-ArchitecturePerformanceBaseline $path } -Reject
    }
    Check 'artifact-traversal' { Assert-ArchitecturePerformanceArtifacts $valid @(@{path='../escape';sha256='fake'}) } -Reject
    Check 'artifact-duplicate' { $i=Load-Json (Join-Path $valid 'index.json'); Assert-ArchitecturePerformanceArtifacts $valid @($i.artifacts[0],$i.artifacts[0]) } -Reject
    $driver = Join-Path $root 'scripts/Summarize-ArchitecturePerformanceBaseline.ps1'
    Check 'cli-refuses-overwrite' { & $driver -InputDirectory $valid -OutputDirectory $valid } -Reject
    Check 'cli-keeps-baseline-immutable' { & $driver -InputDirectory $valid -OutputDirectory (Join-Path $valid 'new-report') } -Reject
    Check 'cli-rejects-snapshot-output' { & $driver -InputDirectory $valid -OutputDirectory (Join-Path $output 'snapshot/new-report') } -Reject
} finally {
    Save-Json @($results) (Join-Path $output 'results.json')
    Write-Output "$($results.Count) report tests; evidence: $output"
}
