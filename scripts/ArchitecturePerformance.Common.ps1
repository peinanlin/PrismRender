# CPU-only validation and statistics; importing this file has no side effects.
function Get-ArchitectureDistribution([double[]]$Values) {
    if (-not $Values.Count) { throw 'Cannot summarize an empty distribution.' }
    foreach ($value in $Values) { if (-not [double]::IsFinite($value) -or $value -lt 0) { throw 'Invalid nonnegative timing/counter.' } }
    $sorted = @($Values | Sort-Object)
    $middle = [int][Math]::Floor($sorted.Count / 2)
    return [ordered]@{ count = $sorted.Count; minimum = $sorted[0]; maximum = $sorted[-1]
        median = $(if ($sorted.Count % 2) { $sorted[$middle] } else { ($sorted[$middle - 1] + $sorted[$middle]) / 2 })
        p95 = $sorted[[int][Math]::Ceiling(0.95 * $sorted.Count) - 1]
        mean = ($sorted | Measure-Object -Average).Average }
}

function Add-ArchitectureMetric($Metrics, [string]$Key, $Value) {
    if ($null -eq $Value -or $Value -is [string] -or $Value -is [bool] -or -not [double]::IsFinite([double]$Value) -or [double]$Value -lt 0) {
        throw "Invalid performance metric: $Key"
    }
    if (-not $Metrics.Contains($Key)) { $Metrics[$Key] = [Collections.Generic.List[double]]::new() }
    $Metrics[$Key].Add([double]$Value)
}

function Assert-ArchitecturePerformanceInteger($Value, [double]$Minimum, [double]$Maximum) {
    if ($null -eq $Value -or $Value -is [string] -or $Value -is [bool] -or -not [double]::IsFinite([double]$Value) -or
        $Value -lt $Minimum -or $Value -gt $Maximum -or [Math]::Truncate([double]$Value) -ne [double]$Value) {
        throw 'Performance identity/counter must be an integer in range.'
    }
}

function Get-ArchitecturePacingConditions($Row) {
    $p = $Row.resources.pacing; $w = $Row.resources.window
    if ($null -eq $p -or $null -eq $w) { throw 'Missing frame pacing/window diagnostics.' }
    foreach ($name in @('presentResult','acquireResult','waitResult','presentMode')) {
        Assert-ArchitecturePerformanceInteger $p[$name] ([int]::MinValue) ([uint32]::MaxValue)
    }
    foreach ($name in @('syncInterval','presentFlags','slot')) { Assert-ArchitecturePerformanceInteger $p[$name] 0 ([uint32]::MaxValue) }
    foreach ($name in @('x','y','monitorX','monitorY')) { Assert-ArchitecturePerformanceInteger $w[$name] ([int]::MinValue) ([int]::MaxValue) }
    foreach ($name in @('width','height','framebufferWidth','framebufferHeight','monitorWidth','monitorHeight','refreshHz')) {
        Assert-ArchitecturePerformanceInteger $w[$name] 1 ([int]::MaxValue)
    }
    foreach ($name in @('visible','focused','minimized','displayAvailable')) {
        if ($w[$name] -isnot [bool]) { throw "Invalid window state: $name" }
    }
    foreach ($name in @('scaleX','scaleY')) { Add-ArchitectureMetric ([ordered]@{}) $name $w[$name] }
    if ($p.api -notin @('d3d12','vulkan') -or $p.beginCompleted -ne $true -or $p.endCompleted -ne $true -or
        $p.presentResult -ne 0 -or $p.acquireResult -ne 0 -or $p.waitResult -ne 0) {
        throw "Non-success/occluded/suboptimal presentation is not a stable performance input: $($p.presentResult) / $($p.acquireResult)"
    }
    Assert-ArchitecturePerformanceInteger $p.generation 1 ([long]::MaxValue)
    Assert-ArchitecturePerformanceInteger $p.imageCount 1 16
    Assert-ArchitecturePerformanceInteger $p.image 0 ($p.imageCount - 1)
    if ($p.slot -ne $Row.views.game.slot) { throw 'Pacing source slot differs from rendered frame.' }
    if ($w.displayAvailable -ne $true -or -not $w.displayName -or $w.refreshHz -le 1 -or $w.minimized -ne $false -or
        $w.framebufferWidth -le 0 -or $w.framebufferHeight -le 0 -or $w.scaleX -le 0 -or $w.scaleY -le 0) {
        throw 'Actual display/window state is unavailable or minimized.'
    }
    if (($p.api -eq 'd3d12' -and ($p.presentMode -ne -1 -or $p.syncInterval -ne 1 -or $p.presentFlags -ne 0)) -or
        ($p.api -eq 'vulkan' -and $p.presentMode -notin @(0,1,2,3))) { throw 'Unexpected presentation policy.' }
    $condition = [ordered]@{}
    foreach ($name in @('api','presentMode','syncInterval','presentFlags','imageCount')) { $condition[$name] = $p[$name] }
    foreach ($name in @('x','y','width','height','framebufferWidth','framebufferHeight','scaleX','scaleY','visible','focused',
            'minimized','displayName','monitorX','monitorY','monitorWidth','monitorHeight','refreshHz')) { $condition[$name] = $w[$name] }
    return $condition
}

function Get-ArchitecturePerformanceFocusPolicy($Metadata) {
    # Reports produced before this additive field retain the original policy.
    if (-not $Metadata.Contains('windowFocusPolicy')) { return 'default' }
    $policy = $Metadata.windowFocusPolicy
    if ($policy -isnot [string] -or $policy -cnotin @('default','focused','unfocused')) { throw 'Invalid performance window focus policy.' }
    return $policy
}

function Get-ArchitecturePerformanceActiveViewPolicy($Metadata) {
    # Reports produced before this additive diagnostic retain normal behavior.
    if (-not $Metadata.Contains('activeViewPolicy')) { return 'default' }
    $policy = $Metadata.activeViewPolicy
    if ($policy -isnot [string] -or $policy -cnotin @('default','game')) {
        throw 'Invalid performance active-view policy.'
    }
    return $policy
}

function Read-ArchitecturePerformanceSamples {
    param([string]$Path, [int]$WarmupFrames, [int]$SampleFrames, [int]$DrainFrames, [int]$Width, [int]$Height)
    $rows = @(Get-Content -LiteralPath $Path | ForEach-Object { $_ | ConvertFrom-Json -AsHashtable })
    $total = $WarmupFrames + $SampleFrames + $DrainFrames
    if ($rows.Count -lt 3 -or $rows[0].type -ne 'header' -or $rows[0].format -ne 'PrismFramePerformance' -or $rows[0].version -ne 1 -or
        $rows[-1].type -ne 'footer' -or $rows[-1].status -ne 'complete' -or $rows[-1].completedFrames -ne $total -or
        $rows[0].metadata.maximumFrames -ne $total -or -not $rows[0].metadata.deterministic) { throw 'Incomplete/invalid performance envelope.' }
    $cpu = @{}; $gpu = @{}; $generations = @{}; $metrics = [ordered]@{}
    $hasPacing = $rows[0].metadata.ContainsKey('pacingVersion')
    $focusPolicy = Get-ArchitecturePerformanceFocusPolicy $rows[0].metadata
    $activeViewPolicy = Get-ArchitecturePerformanceActiveViewPolicy $rows[0].metadata
    if ($focusPolicy -eq 'unfocused' -and -not $hasPacing) { throw 'Unfocused policy requires actual pacing/window diagnostics.' }
    if ($hasPacing -and $rows[0].metadata.pacingVersion -ne 1) { throw 'Unknown pacing diagnostics version.' }
    $pacingNames = @('frameFenceMs','reclaimMs','acquireMs','imageFenceMs','uploadMs','prepareMs','submitMs','nativePresentMs','signalMs')
    $previousPacingGeneration = 0; $conditions = $null
    $cpuNames = @('frameMs','loopIntervalMs','extractionMs','uiBuildMs','uiDrawMs','gameRenderMs','sceneRenderMs','beginFrameMs','presentMs')
    $previousPso = @{ graphicsCreated = 0; computeCreated = 0 }
    for ($i = 1; $i -lt $rows.Count - 1; ++$i) {
        $row = $rows[$i]
        if ($row.type -eq 'cpu') {
            Assert-ArchitecturePerformanceInteger $row.frameId 1 $total
            $frame = [long]$row.frameId
            if ($frame -ne $cpu.Count + 1 -or -not $row.views.ContainsKey('game')) { throw 'Missing/repeated CPU frame or Game view.' }
            $cpu[$frame] = $row
            foreach ($name in $cpuNames) { Add-ArchitectureMetric ([ordered]@{}) $name $row.cpu[$name] }
            foreach ($view in $row.views.Keys) {
                $v = $row.views[$view]
                Assert-ArchitecturePerformanceInteger $v.slot 0 15
                if ($view -notin @('game','scene') -or $v.width -ne $Width -or $v.height -ne $Height -or
                    [Math]::Abs([double]$v.simulationTimeSeconds - ($frame - 1) / 60.0) -gt 1e-8 -or $v.sceneGeneration -le 0 -or
                    $v.slot -lt 0 -or $v.slot -ge 16) { throw "Invalid performance view/time/extent: $frame/$view" }
            }
            foreach ($name in @($previousPso.Keys)) {
                $value = $row.resources.pso[$name]
                Assert-ArchitecturePerformanceInteger $value 0 ([long]::MaxValue)
                if ($null -eq $value -or $value -lt $previousPso[$name]) { throw 'PSO creation counter moved backwards/missing.' }
                $previousPso[$name] = $value
            }
            if ($row.memory.peakWorkingSetBytes -le 0) { throw 'Process memory measurement unavailable.' }
        } elseif ($row.type -eq 'gpu') {
            Assert-ArchitecturePerformanceInteger $row.frameId 1 $total
            Assert-ArchitecturePerformanceInteger $row.observedFrameId 1 $total
            Assert-ArchitecturePerformanceInteger $row.generation 1 ([long]::MaxValue)
            Assert-ArchitecturePerformanceInteger $row.slot 0 15
            $key = "$($row.frameId)/$($row.view)"
            $frame = [long]$row.frameId; $observed = [long]$row.observedFrameId
            if ($gpu.ContainsKey($key) -or -not $cpu.ContainsKey($frame) -or -not $cpu.ContainsKey($observed) -or $observed -le $frame -or
                -not $cpu[$frame].views.ContainsKey($row.view) -or -not $cpu[$observed].views.ContainsKey($row.view) -or
                $cpu[$frame].views[$row.view].slot -ne $row.slot -or $row.generation -le 0 -or
                ($generations.ContainsKey($row.view) -and $row.generation -le $generations[$row.view])) { throw "Invalid GPU source/generation: $key" }
            if ($row.report.format -ne 'PrismGpuTimingReport' -or $row.report.version -ne 2) { throw 'Invalid GPU timing schema.' }
            $passes = @($row.report.passes)
            $totalPass = @($passes | Where-Object name -eq 'Renderer')
            if ($totalPass.Count -ne 1 -or $totalPass[0].gpuMilliseconds -le 0) { throw 'Missing/invalid GPU Renderer interval.' }
            $passNames = @{}
            foreach ($pass in $passes) {
                $passKey = "$($pass.name)/$($pass.queue)"
                if ($passNames.ContainsKey($passKey) -or $pass.queue -notin @('Graphics','Compute')) { throw 'Duplicate/invalid GPU pass.' }
                $passNames[$passKey] = $true
                Add-ArchitectureMetric ([ordered]@{}) $passKey $pass.gpuMilliseconds
            }
            $generations[$row.view] = $row.generation; $gpu[$key] = $row
        } else { throw "Unexpected performance row: $($row.type)" }
    }
    if ($cpu.Count -ne $total) { throw 'CPU frame count does not match requested run.' }
    $viewSet = @($cpu[[long]($WarmupFrames + 1)].views.Keys | Sort-Object) -join ','
    for ($frame = $WarmupFrames + 1; $frame -le $WarmupFrames + $SampleFrames; ++$frame) {
        $row = $cpu[[long]$frame]
        if ($hasPacing) {
            $currentConditions = Get-ArchitecturePacingConditions $row
            if ($focusPolicy -eq 'unfocused' -and ($currentConditions.focused -or -not $currentConditions.visible)) {
                throw "Unfocused policy requires visible, unfocused measured frame $frame."
            }
            if ($focusPolicy -eq 'focused' -and (-not $currentConditions.focused -or -not $currentConditions.visible)) {
                throw "Focused policy requires visible, focused measured frame $frame."
            }
            if ($null -eq $conditions) { $conditions = $currentConditions }
            elseif (($conditions | ConvertTo-Json -Compress) -ne ($currentConditions | ConvertTo-Json -Compress)) {
                throw "Window/display/presentation conditions changed during measured frame $frame."
            }
            if ($row.resources.pacing.generation -le $previousPacingGeneration) { throw 'Pacing generation did not advance.' }
            $previousPacingGeneration = $row.resources.pacing.generation
            foreach ($name in $pacingNames) { Add-ArchitectureMetric $metrics "pacing.$name" $row.resources.pacing.times[$name] }
            Add-ArchitectureMetric $metrics 'observer.windowMs' $row.resources.windowObservationMs
        }
        if ((@($row.views.Keys | Sort-Object) -join ',') -ne $viewSet) { throw 'Active views changed during measurement.' }
        foreach ($name in $cpuNames) { Add-ArchitectureMetric $metrics "cpu.$name" $row.cpu[$name] }
        foreach ($name in @('workingSetBytes','peakWorkingSetBytes','privateCommitBytes','peakCommitBytes')) {
            if ($null -ne $row.memory[$name]) { Add-ArchitectureMetric $metrics "memory.$name" $row.memory[$name] }
        }
        foreach ($name in @('pending','highWatermark','retired','reclaimed')) { Add-ArchitectureMetric $metrics "retirement.$name" $row.resources.retirement[$name] }
        foreach ($view in $row.views.Keys) {
            $key = "$frame/$view"
            if (-not $gpu.ContainsKey($key)) { throw "No completed GPU timing for measured frame: $key" }
            foreach ($pass in $gpu[$key].report.passes) {
                Add-ArchitectureMetric $metrics "gpu.$view.$($pass.queue).$($pass.name)" $pass.gpuMilliseconds
            }
        }
    }
    $distributions = [ordered]@{}
    foreach ($key in $metrics.Keys) { $distributions[$key] = Get-ArchitectureDistribution $metrics[$key].ToArray() }
    $first = $cpu[[long]$WarmupFrames].resources.pso; $last = $cpu[[long]($WarmupFrames + $SampleFrames)].resources.pso
    return [ordered]@{ format = 'PrismPerformanceSummary'; version = 1; completedFrames = $total
        pacingVersion = $(if ($hasPacing) { 1 } else { 0 }); presentationConditions = $conditions
        windowFocusPolicy = $focusPolicy; activeViewPolicy = $activeViewPolicy
        warmupFrames = $WarmupFrames; sampleFrames = $SampleFrames; drainFrames = $DrainFrames; activeViews = $viewSet
        quantileMethod = 'median-average-middle; p95-nearest-rank'; distributions = $distributions
        pso = @{ beforeMeasurement = $first; afterMeasurement = $last
            graphicsCreatedDuringMeasurement = $last.graphicsCreated - $first.graphicsCreated
            computeCreatedDuringMeasurement = $last.computeCreated - $first.computeCreated }
        gpuTailPolicy = 'Unresolved tail excluded; every measured frame/view requires one completed GPU sample.' }
}

function Compare-ArchitecturePerformanceDistributions($Reference, $Candidate) {
    if ($Reference.activeViews -ne $Candidate.activeViews) { throw 'Performance active-view mismatch.' }
    $keys = @('cpu.frameMs', 'cpu.loopIntervalMs') + @($Reference.distributions.Keys | Where-Object { $_ -match '^gpu\..*\.Renderer$' })
    if ($keys.Count -lt 3) { throw 'No GPU total metric for performance comparison.' }
    $results = @()
    foreach ($key in $keys) {
        if (-not $Candidate.distributions.Contains($key)) { throw "Missing performance distribution: $key" }
        $a = $Reference.distributions[$key]; $b = $Candidate.distributions[$key]
        if ($a.median -le 0 -or $a.p95 -le 0) { throw 'Cannot compare zero performance reference.' }
        $medianChange = $b.median / $a.median - 1
        $p95Change = $b.p95 / $a.p95 - 1
        $results += @{ metric = $key; medianChange = $medianChange; p95Change = $p95Change
            # Compare against scaled bounds directly: subtraction after division
            # can turn the exact 5%/10% boundary into a spurious failure.
            passed = $b.median -le ($a.median * 1.05) -and $b.p95 -le ($a.p95 * 1.10) }
    }
    return @{ passed = @($results | Where-Object { -not $_.passed }).Count -eq 0; metrics = $results }
}

function Read-ArchitecturePerformanceRun([string]$Directory) {
    $directoryPath = [IO.Path]::GetFullPath($Directory)
    $run = Get-Content (Join-Path $directoryPath 'index.json') -Raw | ConvertFrom-Json -AsHashtable
    if ($run.format -ne 'PrismArchitecturePerformanceRun' -or $run.version -ne 1 -or $run.status -ne 'measured-and-validated') {
        throw 'Performance run was not completed and validated.'
    }
    $paths = @{}
    foreach ($artifact in $run.artifacts) {
        $path = [IO.Path]::GetFullPath($artifact.path, $directoryPath)
        if (-not $path.StartsWith($directoryPath + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) -or
            $paths.ContainsKey($path) -or (Get-FileHash -LiteralPath $path).Hash -ne $artifact.sha256) { throw 'Invalid performance artifact path/hash.' }
        $paths[$path] = $true
    }
    foreach ($required in @('frames.jsonl','summary.json','source-inputs.json','identity.json','process.stderr.log')) {
        if (-not $paths.ContainsKey((Join-Path $directoryPath $required))) { throw "Unsealed performance artifact: $required" }
    }
    Assert-ArchitectureValidationLog (Get-Content (Join-Path $directoryPath 'process.stderr.log') -Raw) $run.backend
    $summary = Read-ArchitecturePerformanceSamples (Join-Path $directoryPath 'frames.jsonl') $run.warmupFrames $run.sampleFrames $run.drainFrames $run.width $run.height
    $stored = Get-Content (Join-Path $directoryPath 'summary.json') -Raw | ConvertFrom-Json -AsHashtable
    $requestedFocus = if ($run.environment.ContainsKey('PRISM_RENDER_PERFORMANCE_WINDOW_FOCUS')) {
        $run.environment.PRISM_RENDER_PERFORMANCE_WINDOW_FOCUS
    } else { 'default' }
    if ($summary.windowFocusPolicy -cne $requestedFocus -or
        $summary.windowFocusPolicy -cne (Get-ArchitecturePerformanceFocusPolicy $stored)) {
        throw 'Focus policy differs between requested input, raw samples and stored summary.'
    }
    $requestedActiveViews = if ($run.environment.ContainsKey('PRISM_RENDER_PERFORMANCE_ACTIVE_VIEWS')) {
        $run.environment.PRISM_RENDER_PERFORMANCE_ACTIVE_VIEWS
    } else { 'default' }
    if ($summary.activeViewPolicy -cne $requestedActiveViews -or
        $summary.activeViewPolicy -cne (Get-ArchitecturePerformanceActiveViewPolicy $stored)) {
        throw 'Active-view policy differs between requested input, raw samples and stored summary.'
    }
    # Legacy runs lack this additive extension; never compare them as equal inputs to pacing runs.
    if ($summary.pacingVersion -eq 1 -and ($stored.pacingVersion -ne 1 -or
        ($stored.presentationConditions | ConvertTo-Json -Compress) -ne ($summary.presentationConditions | ConvertTo-Json -Compress))) {
        throw 'Stored presentation conditions differ from raw samples.'
    }
    if ($summary.pacingVersion -eq 1 -and $summary.presentationConditions.api -ne $run.backend) { throw 'Pacing backend mismatch.' }
    # Numeric JSON may round-trip integral doubles as integers; compare values, not serialized spelling.
    if ($stored.activeViews -ne $summary.activeViews -or $stored.sampleFrames -ne $summary.sampleFrames -or
        $stored.distributions.Count -ne $summary.distributions.Count) { throw 'Performance summary does not match raw samples.' }
    foreach ($key in $summary.distributions.Keys) {
        foreach ($field in @('count','minimum','maximum','median','p95','mean')) {
            if (-not $stored.distributions.Contains($key) -or [Math]::Abs($stored.distributions[$key][$field] - $summary.distributions[$key][$field]) -gt 1e-9) {
                throw "Performance summary differs from raw samples: $key/$field"
            }
        }
    }
    $identityDocument = Get-Content (Join-Path $directoryPath 'identity.json') -Raw | ConvertFrom-Json -AsHashtable
    if ($identityDocument.format -ne 'PrismRuntimePerformanceIdentity' -or $identityDocument.version -ne 1) { throw 'Invalid runtime performance identity.' }
    foreach ($key in @('graphicsApi','adapterName','vendorId','deviceId','driverVersion','apiVersion','cpuName','buildConfiguration','compiler','architecture','shaderRevision','executableHash')) {
        if (-not $identityDocument.identity.ContainsKey($key) -or [string]::IsNullOrEmpty([string]$identityDocument.identity[$key])) { throw "Missing runtime identity: $key" }
    }
    if ($identityDocument.identity.graphicsApi -ne $run.backend) { throw 'Runtime backend differs from requested backend.' }
    return @{ index = $run; summary = $summary
        identity = $identityDocument.identity
        sourceHash = (Get-FileHash (Join-Path $directoryPath 'source-inputs.json')).Hash }
}

function Assert-ArchitecturePerformanceCompatibility($Reference, $Candidate) {
    if ((Get-ArchitecturePerformanceFocusPolicy $Reference.summary) -cne (Get-ArchitecturePerformanceFocusPolicy $Candidate.summary)) {
        throw 'Performance window focus policy mismatch.'
    }
    if ((Get-ArchitecturePerformanceActiveViewPolicy $Reference.summary) -cne
        (Get-ArchitecturePerformanceActiveViewPolicy $Candidate.summary)) {
        throw 'Performance active-view policy mismatch.'
    }
    if (($Reference.summary.presentationConditions | ConvertTo-Json -Compress) -ne
        ($Candidate.summary.presentationConditions | ConvertTo-Json -Compress) -or
        $Reference.summary.pacingVersion -ne $Candidate.summary.pacingVersion) {
        throw 'Actual window/display/presentation conditions or observation protocol mismatch.'
    }
    foreach ($key in @('backend','scene','view','queueMode','width','height','warmupFrames','sampleFrames','drainFrames','cachePolicy','initialImGuiSha256')) {
        if ($Reference.index[$key] -ne $Candidate.index[$key]) { throw "Performance input mismatch: $key" }
    }
    $ignored = @('PRISM_RENDER_FRAME_PERFORMANCE_PATH','PRISM_RENDER_PERFORMANCE_IDENTITY_PATH','PRISM_RENDER_QUEUE_COST_MODEL_PATH',
        'PRISM_RENDER_LOG_PATH','PRISM_RENDER_CRASH_REPORT_PATH','PRISM_RENDER_MINIDUMP_PATH','VK_LAYER_PATH')
    $keys = @(@($Reference.index.environment.Keys) + @($Candidate.index.environment.Keys) | Select-Object -Unique)
    foreach ($key in $keys) {
        if ($key -notin $ignored -and $Reference.index.environment[$key] -ne $Candidate.index.environment[$key]) { throw "Performance environment mismatch: $key" }
    }
    # executable/shader hashes are allowed to differ for later architecture comparisons;
    # the batch driver additionally pins both for baseline repeatability.
    foreach ($key in @('graphicsApi','adapterName','vendorId','deviceId','driverVersion','apiVersion','cpuName','buildConfiguration','compiler','architecture')) {
        if (($Reference.identity[$key] | ConvertTo-Json -Depth 8 -Compress) -ne ($Candidate.identity[$key] | ConvertTo-Json -Depth 8 -Compress)) {
            throw "Performance hardware/build identity mismatch: $key"
        }
    }
}

function Get-ArchitecturePerformanceBatchSummary([object[]]$Summaries) {
    if ($Summaries.Count -lt 5) { throw 'A V3 batch needs at least five independent measurements.' }
    $result = [ordered]@{ activeViews = $Summaries[0].activeViews; independentRuns = $Summaries.Count
        aggregation = 'median-of-run-medians; median-of-run-p95; raw-per-run-distributions-retained'
        distributions = [ordered]@{} }
    foreach ($summary in $Summaries) {
        if ($summary.sampleFrames -lt 180 -or $summary.activeViews -ne $result.activeViews) { throw 'Insufficient samples or changing views in V3 batch.' }
    }
    foreach ($key in $Summaries[0].distributions.Keys) {
        $medians = @(); $tails = @()
        foreach ($summary in $Summaries) {
            if (-not $summary.distributions.Contains($key)) { throw "Missing batch metric: $key" }
            $medians += $summary.distributions[$key].median
            $tails += $summary.distributions[$key].p95
        }
        $result.distributions[$key] = @{ median = (Get-ArchitectureDistribution $medians).median
            p95 = (Get-ArchitectureDistribution $tails).median
            runMedians = $medians; runP95 = $tails }
    }
    return $result
}
