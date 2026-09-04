Set-StrictMode -Version Latest

function Get-ArchitectureDistribution {
    param([object[]]$Values)

    $numbers = @($Values | Where-Object { $null -ne $_ } |
        ForEach-Object { [double]$_ } | Sort-Object)
    if ($numbers.Count -eq 0) { return $null }

    return [ordered]@{
        minimum = $numbers[0]
        average = ($numbers | Measure-Object -Average).Average
        median = $numbers[[int][math]::Floor(($numbers.Count - 1) * 0.50)]
        p95 = $numbers[[int][math]::Floor(($numbers.Count - 1) * 0.95)]
        p99 = $numbers[[int][math]::Floor(($numbers.Count - 1) * 0.99)]
        maximum = $numbers[-1]
    }
}

function Get-ArchitectureScenarioRunSummary {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][object]$Scenario,
        [Parameter(Mandatory)][int]$Warmup,
        [Parameter(Mandatory)][int]$Samples,
        [Parameter(Mandatory)][string]$ExpectedBackend,
        [Parameter(Mandatory)][string]$ExpectedBuild,
        [Parameter(Mandatory)][string]$ExpectedLevel,
        [Parameter(Mandatory)][bool]$ExpectedValidation,
        [Parameter(Mandatory)][bool]$ExpectedVisible,
        [Parameter(Mandatory)][string]$ExpectedFocusPolicy,
        [Parameter(Mandatory)][int]$ExpectedWidth,
        [Parameter(Mandatory)][int]$ExpectedHeight
    )

    $rows = @([IO.File]::ReadLines($Path) |
        ForEach-Object { $_ | ConvertFrom-Json })
    if ($rows.Count -lt 3 -or $rows[0].format -ne 'PrismFrameProfiler' -or
        $rows[0].version -ne 1 -or $rows[-1].type -ne 'footer' -or
        $rows[-1].status -ne 'complete') {
        throw "Incomplete frame profiler report: $Path"
    }

    $frames = @($rows | Where-Object type -eq 'frame')
    if ([int]$rows[-1].completedFrames -ne $frames.Count) {
        throw "Frame profiler footer count does not match the report: $Path"
    }
    for ($index = 1; $index -lt $frames.Count; ++$index) {
        if ([uint64]$frames[$index].frameId -le
            [uint64]$frames[$index - 1].frameId) {
            throw "Frame profiler report is not strictly ordered: $Path"
        }
    }

    $metadata = $rows[0].metadata
    $backendName = if ($ExpectedBackend -eq 'd3d12') {
        'Direct3D 12'
    } elseif ($ExpectedBackend -eq 'vulkan') {
        'Vulkan'
    } else {
        throw "Unsupported expected backend: $ExpectedBackend"
    }
    if ($metadata.backend -ne $backendName) {
        throw "Scenario '$($Scenario.id)' backend mismatch."
    }
    if ($metadata.identity.identity.buildConfiguration -ne $ExpectedBuild) {
        throw "Scenario '$($Scenario.id)' build configuration mismatch."
    }
    if ([bool]$metadata.validationRequested -ne $ExpectedValidation) {
        throw "Scenario '$($Scenario.id)' validation setting mismatch."
    }
    if (-not ($metadata.PSObject.Properties.Name -contains 'headless') -or
        [bool]$metadata.headless -eq $ExpectedVisible) {
        throw "Scenario '$($Scenario.id)' window visibility policy mismatch."
    }
    if (-not ($metadata.PSObject.Properties.Name -contains 'windowFocusPolicy') -or
        $metadata.windowFocusPolicy -ne $ExpectedFocusPolicy) {
        throw "Scenario '$($Scenario.id)' window focus policy mismatch."
    }
    if ([int]$metadata.width -ne $ExpectedWidth -or
        [int]$metadata.height -ne $ExpectedHeight) {
        throw "Scenario '$($Scenario.id)' render extent mismatch."
    }
    if ([bool]$metadata.editorEnabled -ne
        [bool]$Scenario.expectedEditorEnabled) {
        throw "Scenario '$($Scenario.id)' application mode mismatch."
    }

    $sample = @($frames | Select-Object -Skip $Warmup -First $Samples)
    if ($sample.Count -ne $Samples) {
        throw "Scenario '$($Scenario.id)' sample count mismatch."
    }
    if (@($sample | Where-Object actualLevel -ne $ExpectedLevel).Count -ne 0) {
        throw "Scenario '$($Scenario.id)' profiling level mismatch."
    }
    $expectedMask = if ($Scenario.activeViews -eq 'game+scene') { 3 } else { 1 }
    if (@($sample | Where-Object {
                [int]$_.activeViewMask -ne $expectedMask
            }).Count -ne 0) {
        throw "Scenario '$($Scenario.id)' active view mismatch."
    }

    $fps = @($sample | ForEach-Object {
            if ($null -eq $_.editorLoopMs -or
                [double]$_.editorLoopMs -le 0.0) {
                throw "Scenario '$($Scenario.id)' contains an invalid Editor Loop duration."
            }
            1000.0 / [double]$_.editorLoopMs
        })
    $waits = [ordered]@{}
    foreach ($name in @('display-admission', 'frame-limiter',
            'frame-fence', 'reclaim', 'acquire', 'image-fence',
            'upload', 'prepare', 'submit', 'native-present', 'signal',
            'queue-backpressure')) {
        $waits[$name] = Get-ArchitectureDistribution @(
            $sample | ForEach-Object { $_.waits.$name })
    }
    $framePacing = $sample[0].framePacing
    if ($null -eq $framePacing -or -not [bool]$framePacing.available) {
        throw "Scenario '$($Scenario.id)' is missing frame-pacing state."
    }
    $framePacingJson = $framePacing | ConvertTo-Json -Depth 6 -Compress
    if (@($sample | Where-Object {
                ($_.framePacing | ConvertTo-Json -Depth 6 -Compress) -ne
                    $framePacingJson
            }).Count -ne 0) {
        throw "Scenario '$($Scenario.id)' changed frame-pacing state during measurement."
    }

    return [ordered]@{
        id = $Scenario.id
        workload = $Scenario.workload
        binaryKind = $Scenario.binaryKind
        activeViews = $Scenario.activeViews
        editorEnabled = [bool]$metadata.editorEnabled
        actualLoopFps = Get-ArchitectureDistribution $fps
        editorLoopMs = Get-ArchitectureDistribution @($sample.editorLoopMs)
        lanes = [ordered]@{
            mainActiveMs = Get-ArchitectureDistribution @($sample.lanes.main.activeMs)
            mainWaitMs = Get-ArchitectureDistribution @($sample.lanes.main.waitMs)
            renderActiveMs = Get-ArchitectureDistribution @($sample.lanes.render.activeMs)
            workerActiveMs = Get-ArchitectureDistribution @($sample.lanes.worker.activeMs)
        }
        views = [ordered]@{
            gameCpuMs = Get-ArchitectureDistribution @($sample.views.game.cpuRenderMs)
            gameGpuMs = Get-ArchitectureDistribution @($sample.views.game.gpuTotalMs)
            sceneCpuMs = Get-ArchitectureDistribution @($sample.views.scene.cpuRenderMs)
            sceneGpuMs = Get-ArchitectureDistribution @($sample.views.scene.gpuTotalMs)
            sceneRenderedFrames = @($sample | Where-Object {
                    $_.views.scene.rendered
                }).Count
        }
        gpuCriticalHintMs = Get-ArchitectureDistribution @($sample.gpuFrameMs)
        pipeline = [ordered]@{
            inputToPresentMs = Get-ArchitectureDistribution @(
                $sample.pipeline.inputToPresentMs)
            queueWaitMs = Get-ArchitectureDistribution @(
                $sample.pipeline.queueWaitMs)
            waitingDepthAtAcceptance = Get-ArchitectureDistribution @(
                $sample.pipeline.waitingDepthAtAcceptance)
            peakWaitingDepth = Get-ArchitectureDistribution @(
                $sample.pipeline.peakWaitingDepth)
        }
        framePacing = $framePacing
        waits = $waits
        profilerOverheadMs = Get-ArchitectureDistribution @(
            $sample.profilerOverheadMs)
        adapter = [ordered]@{
            name = $metadata.identity.identity.adapterName
            driverVersion = $metadata.identity.identity.driverVersion
            deviceId = $metadata.identity.identity.deviceId
            vendorId = $metadata.identity.identity.vendorId
        }
        executableHash = $metadata.identity.identity.executableHash
    }
}
