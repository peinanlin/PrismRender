# Offline evidence checks. These helpers never launch a renderer or change a gate.
function Assert-ArchitecturePerformanceArtifacts([string]$Directory, $Artifacts) {
    $base = [IO.Path]::GetFullPath($Directory)
    $seen = @{}
    foreach ($artifact in $Artifacts) {
        $path = [IO.Path]::GetFullPath($artifact.path, $base)
        if (-not $path.StartsWith($base + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) -or
            $seen.ContainsKey($path) -or (Get-FileHash -LiteralPath $path).Hash -ne $artifact.sha256) {
            throw 'Invalid or changed baseline artifact.'
        }
        $seen[$path] = $true
    }
    return $seen.Count
}

function Get-ArchitecturePerformanceChanges($Before, $After) {
    $changes = @()
    foreach ($key in $Before.distributions.Keys | Sort-Object) {
        if (-not $After.distributions.Contains($key)) { throw "Missing diagnostic metric: $key" }
        $a = $Before.distributions[$key]; $b = $After.distributions[$key]
        $changes += [ordered]@{ metric = $key
            medianBefore = $a.median; medianAfter = $b.median; medianDelta = $b.median - $a.median
            medianChange = $(if ($a.median -gt 0) { $b.median / $a.median - 1 } else { $null })
            p95Before = $a.p95; p95After = $b.p95; p95Delta = $b.p95 - $a.p95
            p95Change = $(if ($a.p95 -gt 0) { $b.p95 / $a.p95 - 1 } else { $null }) }
    }
    return ,$changes
}

function Assert-ArchitecturePerformanceBatch($Expected, $Stored) {
    foreach ($key in @('activeViews', 'independentRuns', 'aggregation')) {
        if ($Expected[$key] -ne $Stored[$key]) { throw "Stored batch differs: $key" }
    }
    if ($Expected.distributions.Count -ne $Stored.distributions.Count) { throw 'Stored batch metric count differs.' }
    foreach ($key in $Expected.distributions.Keys) {
        if (-not $Stored.distributions.Contains($key)) { throw "Missing stored batch metric: $key" }
        foreach ($field in @('median', 'p95', 'runMedians', 'runP95')) {
            $a = @($Expected.distributions[$key][$field]); $b = @($Stored.distributions[$key][$field])
            if ($a.Count -ne $b.Count) { throw "Stored batch length differs: $key/$field" }
            for ($i = 0; $i -lt $a.Count; ++$i) {
                if ($null -eq $b[$i] -or $b[$i] -is [bool] -or $b[$i] -is [string] -or
                    -not [double]::IsFinite([double]$b[$i]) -or [Math]::Abs($a[$i] - $b[$i]) -gt 1e-9) {
                    throw "Stored batch value differs: $key/$field"
                }
            }
        }
    }
}

function Get-ArchitecturePerformanceConditionTransitions([string]$Path, [int]$WarmupFrames, [int]$SampleFrames) {
    $first = $null; $previous = $null; $changes = @(); $changeCount = 0; $invalid = @(); $invalidCount = 0
    $observed = 0
    foreach ($line in [IO.File]::ReadLines($Path)) {
        $row = $line | ConvertFrom-Json -AsHashtable
        if ($row.type -ne 'cpu' -or $row.frameId -le $WarmupFrames -or $row.frameId -gt $WarmupFrames + $SampleFrames) { continue }
        ++$observed
        try { $current = Get-ArchitecturePacingConditions $row } catch {
            ++$invalidCount
            if ($invalid.Count -lt 64) { $invalid += @{ frame = $row.frameId; error = $_.Exception.Message } }
            $previous = $null
            continue
        }
        if ($null -eq $first) { $first = $current }
        if ($null -ne $previous) {
            $fields = @($current.Keys | Where-Object { $current[$_] -ne $previous[$_] })
            if ($fields.Count) {
                ++$changeCount
                if ($changes.Count -lt 64) {
                    $changes += @{ frame = $row.frameId; fields = $fields; before = $previous; after = $current }
                }
            }
        }
        $previous = $current
    }
    return @{ observedCpuRows = $observed; firstConditions = $first; transitionCount = $changeCount; transitions = $changes
        invalidConditionRows = $invalidCount; invalidExamples = $invalid; exampleLimit = 64
        limitation = 'Diagnostic only, not sample acceptance; examples capped at 64, totals are not filtered.' }
}

function Read-ArchitecturePerformanceBaseline([string]$Directory) {
    $base = [IO.Path]::GetFullPath($Directory)
    $indexPath = Join-Path $base 'index.json'
    $sourceIndexHash = (Get-FileHash $indexPath).Hash
    $index = Get-Content -LiteralPath $indexPath -Raw | ConvertFrom-Json -AsHashtable
    if ($index.format -ne 'PrismArchitecturePerformanceBaseline' -or $index.version -ne 1 -or
        $index.status -notin @('failed', 'baseline-repeatability-passed') -or
        $index.batches -ne 2 -or $index.warmupRunsPerBatch -ne 3 -or $index.measurementRunsPerBatch -ne 5 -or
        $index.warmupFramesPerRun -lt 60 -or $index.sampleFramesPerRun -lt 180) {
        throw 'Not a finalized two-batch performance baseline.'
    }
    $artifactCount = Assert-ArchitecturePerformanceArtifacts $base $index.artifacts
    $expected = [ordered]@{}
    for ($batch = 1; $batch -le 2; ++$batch) {
        foreach ($backend in $index.backends) { foreach ($scene in $index.scenes) {
            if ($backend -notin @('d3d12', 'vulkan') -or $scene -notmatch '^[a-z0-9]+(-[a-z0-9]+)*$') { throw 'Invalid baseline case.' }
            for ($run = 1; $run -le 8; ++$run) {
                $name = "$backend-$scene-b$batch-r$run"
                if ($expected.Contains($name)) { throw 'Duplicate baseline case.' }
                $expected[$name] = @{ case = "$backend-$scene"; backend = $backend; scene = $scene; batch = $batch; run = $run
                    role = $(if ($run -le 3) { 'warmup' } else { 'measurement' }) }
            }
        } }
    }
    if (-not $expected.Count) { throw 'Empty baseline case list.' }
    $registered = @{}
    foreach ($entry in $index.runs) {
        $name = "$($entry.case)-b$($entry.batch)-r$($entry.run)"
        if (-not $expected.Contains($name) -or $registered.ContainsKey($name) -or
            $entry.role -ne $expected[$name].role -or
            [IO.Path]::GetFullPath($entry.path, $base) -ne (Join-Path $base $name) -or
            (Get-FileHash (Join-Path $base "$name/index.json")).Hash -ne $entry.indexSha256) {
            throw 'Invalid registered baseline run identity/hash.'
        }
        $registered[$name] = $entry
    }
    foreach ($directoryEntry in Get-ChildItem -LiteralPath $base -Directory) {
        if (-not $expected.Contains($directoryEntry.Name)) { throw "Unexpected run directory: $($directoryEntry.Name)" }
    }
    $runs = @(); $references = @{}; $groups = @{}; $valid = 0; $formal = 0; $failed = 0; $missing = 0
    foreach ($name in $expected.Keys) {
        $spec = $expected[$name]; $path = Join-Path $base $name
        if (-not (Test-Path -LiteralPath $path)) { ++$missing; continue }
        $child = Get-Content -LiteralPath (Join-Path $path 'index.json') -Raw | ConvertFrom-Json -AsHashtable
        if ($child.format -ne 'PrismArchitecturePerformanceRun' -or $child.version -ne 1 -or
            $child.status -notin @('failed', 'measured-and-validated') -or $child.backend -ne $spec.backend -or $child.scene -ne $spec.scene -or
            $child.warmupFrames -ne $index.warmupFramesPerRun -or $child.sampleFrames -ne $index.sampleFramesPerRun -or
            $child.drainFrames -ne $index.drainFramesPerRun) { throw "Invalid child run: $name" }
        $artifactCount += Assert-ArchitecturePerformanceArtifacts $path $child.artifacts
        $row = [ordered]@{ name = $name; case = $spec.case; batch = $spec.batch; run = $spec.run; role = $spec.role
            status = $child.status; registered = $registered.ContainsKey($name)
            indexSha256 = (Get-FileHash (Join-Path $path 'index.json')).Hash }
        if ($child.status -eq 'failed') {
            if ($registered.ContainsKey($name)) { throw 'Failed child was registered as valid.' }
            ++$failed; $row['error'] = $child.error
            if (@($child.artifacts).Count -gt 0 -and 'frames.jsonl' -in $child.artifacts.path) {
                try {
                    $row['conditionDiagnostics'] = Get-ArchitecturePerformanceConditionTransitions (Join-Path $path 'frames.jsonl') $child.warmupFrames $child.sampleFrames
                } catch {
                    # A crashed process may leave a sealed but truncated JSONL.
                    # Keep the failed run; this diagnostic must not promote it.
                    $row['conditionDiagnosticError'] = $_.Exception.Message
                }
            }
        } else {
            # Recompute from raw samples, not just the serialized summary.
            $sample = Read-ArchitecturePerformanceRun $path
            if ($references.ContainsKey($spec.case)) {
                $reference = $references[$spec.case]
                try {
                    Assert-ArchitecturePerformanceCompatibility $reference $sample
                    foreach ($field in @('binarySha256', 'driverSha256', 'helperSha256')) {
                        if ($reference.index[$field] -ne $sample.index[$field]) { throw "Changed baseline $field" }
                    }
                    if ($reference.sourceHash -ne $sample.sourceHash) { throw 'Changed baseline source inputs.' }
                } catch {
                    if ($registered.ContainsKey($name)) { throw }
                    $row['compatibilityError'] = $_.Exception.Message
                }
            } else { $references[$spec.case] = $sample }
            if ($registered.ContainsKey($name)) {
                ++$valid
                if ($spec.role -eq 'measurement') {
                    ++$formal; $group = "$($spec.case)-b$($spec.batch)"
                    if (-not $groups.ContainsKey($group)) { $groups[$group] = @() }
                    $groups[$group] += $sample.summary
                }
            }
            # A valid but unregistered child may be the run rejected by cross-run compatibility.
            $row['conditions'] = $sample.summary.presentationConditions
            $row['pso'] = $sample.summary.pso
        }
        $runs += $row
    }
    $cases = @(); $computedComparisons = @{}
    foreach ($case in $expected.Values.case | Select-Object -Unique) {
        $batches = @{}
        for ($batch = 1; $batch -le 2; ++$batch) {
            $key = "$case-b$batch"
            if ($groups.ContainsKey($key) -and $groups[$key].Count -eq 5) {
                $batches["$batch"] = Get-ArchitecturePerformanceBatchSummary $groups[$key]
                $summaryFile = "$key-summary.json"
                if ($summaryFile -notin $index.artifacts.path) { throw "Unsealed batch summary: $summaryFile" }
                $stored = Get-Content -LiteralPath (Join-Path $base $summaryFile) -Raw | ConvertFrom-Json -AsHashtable
                Assert-ArchitecturePerformanceBatch $batches["$batch"] $stored
            }
        }
        $row = [ordered]@{ case = $case; complete = $batches.Count -eq 2; batches = $batches }
        if ($batches.Count -eq 2) {
            $forward = Compare-ArchitecturePerformanceDistributions $batches['1'] $batches['2']
            $reverse = Compare-ArchitecturePerformanceDistributions $batches['2'] $batches['1']
            $row['passed'] = $forward.passed -and $reverse.passed
            $row['batch1To2'] = $forward; $row['batch2To1'] = $reverse
            $row['diagnosticChanges'] = Get-ArchitecturePerformanceChanges $batches['1'] $batches['2']
            $computedComparisons[$case] = $row
        }
        $cases += $row
    }
    $seenComparisons = @{}
    foreach ($comparison in $index.comparisons) {
        if ($seenComparisons.ContainsKey($comparison.case) -or -not $computedComparisons.ContainsKey($comparison.case)) { throw 'Unexpected stored comparison.' }
        $actual = $computedComparisons[$comparison.case]
        if ($comparison.passed -ne $actual.passed) { throw 'Stored gate differs from raw samples.' }
        foreach ($direction in @('batch1To2', 'batch2To1')) {
            if ($comparison[$direction].passed -ne $actual[$direction].passed -or
                $comparison[$direction].metrics.Count -ne $actual[$direction].metrics.Count) { throw 'Stored directional gate differs.' }
            foreach ($metric in $actual[$direction].metrics) {
                $stored = @($comparison[$direction].metrics | Where-Object metric -eq $metric.metric)
                if ($stored.Count -ne 1 -or $stored[0].passed -ne $metric.passed) { throw 'Stored metric gate differs.' }
                foreach ($field in @('medianChange','p95Change')) {
                    if ($null -eq $stored[0][$field] -or -not [double]::IsFinite([double]$stored[0][$field]) -or
                        [Math]::Abs($stored[0][$field] - $metric[$field]) -gt 1e-9) { throw 'Stored metric change differs.' }
                }
            }
        }
        $seenComparisons[$comparison.case] = $true
    }
    if ($seenComparisons.Count -ne $computedComparisons.Count) { throw 'Missing stored comparison.' }
    $complete = $valid -eq $expected.Count
    $passed = $complete -and @($cases | Where-Object { -not $_.passed }).Count -eq 0
    if (($index.status -eq 'baseline-repeatability-passed') -ne $passed) { throw 'Aggregate status contradicts recomputed gate.' }
    if ((Get-FileHash $indexPath).Hash -ne $sourceIndexHash) { throw 'Baseline index changed during audit.' }
    return [ordered]@{ format = 'PrismArchitecturePerformanceAudit'; version = 1; sourceDirectory = $base
        sourceIndexSha256 = $sourceIndexHash; originalStatus = $index.status
        complete = $complete; passed = $passed; expectedRuns = $expected.Count; executedRuns = $runs.Count
        registeredValidRuns = $valid; failedRuns = $failed; missingRuns = $missing
        formalRuns = $formal; formalFrames = $formal * $index.sampleFramesPerRun; verifiedArtifacts = $artifactCount
        runs = $runs; cases = $cases; limitations = @('An incomplete or unstable baseline is not P0 acceptance.',
            'Phase changes are descriptive, not a causal attribution; GPU stage sums are not critical-path time.',
            'Failed child hashes are reported separately even when the original driver omitted that child from its valid-run index.') }
}
