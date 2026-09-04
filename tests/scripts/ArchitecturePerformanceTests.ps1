$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. (Join-Path $root 'scripts/ArchitectureValidation.Common.ps1')
. (Join-Path $root 'scripts/ArchitecturePerformance.Common.ps1')
$output = Join-Path $root ('artifacts/architecture-refactor/tool-tests/performance-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $output | Out-Null
$results = [Collections.Generic.List[object]]::new()
function Check([string]$Name, [scriptblock]$Body, [switch]$Reject) {
    $errorText = ''
    try { & $Body | Out-Null } catch { $errorText = $_.Exception.Message }
    $passed = if ($Reject) { [bool]$errorText } else { -not $errorText }
    $results.Add(@{ name = $Name; passed = $passed; error = $errorText })
    if (-not $passed) { throw "Failed performance tool test $Name : $errorText" }
}
function Make-Rows([int]$Count = 10) {
    $rows = [Collections.Generic.List[object]]::new()
    $rows.Add(@{ type='header'; format='PrismFramePerformance'; version=1; metadata=@{maximumFrames=$Count; deterministic=$true} })
    for ($f = 1; $f -le $Count; ++$f) {
        $rows.Add(@{type='cpu'; frameId=$f; cpu=@{frameMs=10.0;loopIntervalMs=11.0;extractionMs=1.0;uiBuildMs=1.0;uiDrawMs=1.0;gameRenderMs=3.0;sceneRenderMs=0.0;beginFrameMs=1.0;presentMs=1.0}
            views=@{game=@{width=1280;height=800;simulationTimeSeconds=($f-1)/60.0;sceneGeneration=$f;slot=($f-1)%2}}
            memory=@{workingSetBytes=100;peakWorkingSetBytes=120;privateCommitBytes=140;peakCommitBytes=160}
            resources=@{pso=@{graphicsCreated=10;computeCreated=20};retirement=@{pending=2;highWatermark=4;retired=6;reclaimed=4}}})
        if ($f -gt 2) { $rows.Add(@{type='gpu';frameId=$f-2;observedFrameId=$f;view='game';slot=($f-3)%2;generation=$f-2
            report=@{format='PrismGpuTimingReport';version=2;passes=@(@{name='Renderer';queue='Graphics';gpuMilliseconds=5.0},@{name='Shadow';queue='Graphics';gpuMilliseconds=1.0})}}) }
    }
    $rows.Add(@{type='footer';status='complete';completedFrames=$Count})
    return ,$rows
}
function Write-Rows($Rows, [string]$Name) {
    $path = Join-Path $output "$Name.jsonl"
    @($Rows | ForEach-Object { $_ | ConvertTo-Json -Compress -Depth 10 }) | Set-Content $path -Encoding utf8
    return $path
}
try {
    Check 'quantile-even-nearest-rank' { $d=Get-ArchitectureDistribution @(1,2,3,4); if ($d.median -ne 2.5 -or $d.p95 -ne 4) { throw 'Wrong quantile.' } }
    Check 'reject-empty' { Get-ArchitectureDistribution @() } -Reject
    Check 'reject-negative' { Get-ArchitectureDistribution @(1,-1) } -Reject
    Check 'reject-nan' { Get-ArchitectureDistribution @([double]::NaN) } -Reject
    Check 'reject-fractional-frame' { Assert-ArchitecturePerformanceInteger 2.4 1 10 } -Reject
    Check 'reject-boolean-counter' { Assert-ArchitecturePerformanceInteger $true 0 10 } -Reject
    $valid = Write-Rows (Make-Rows) 'valid'
    Check 'valid-full-coverage' {
        $s=Read-ArchitecturePerformanceSamples $valid 2 4 4 1280 800
        if ($s.distributions['gpu.game.Graphics.Renderer'].count -ne 4 -or $s.pso.computeCreatedDuringMeasurement -ne 0) { throw 'Wrong summary.' }
    }
    foreach ($test in @('footer','missing-gpu','duplicate-gpu','wrong-slot','wrong-time','extent','negative','unknown-view','generation','missing-memory','pso-backwards')) {
        $rows=Make-Rows
        switch ($test) {
            'footer' { $rows.RemoveAt($rows.Count-1) }
            'missing-gpu' { $rows.RemoveAt(8) }
            'duplicate-gpu' { $rows.Insert(9,$rows[8]) }
            'wrong-slot' { $rows[8].slot=1 }
            'wrong-time' { $rows[1].views.game.simulationTimeSeconds=99 }
            'extent' { $rows[1].views.game.width=1279 }
            'negative' { $rows[1].cpu.frameMs=-1 }
            'unknown-view' { $rows[8].view='other' }
            'generation' { $rows[8].generation=1 }
            'missing-memory' { $rows[1].memory.peakWorkingSetBytes=0 }
            'pso-backwards' { $rows[2].resources.pso.graphicsCreated=9 }
        }
        $path=Write-Rows $rows $test
        Check "reject-$test" { Read-ArchitecturePerformanceSamples $path 2 4 4 1280 800 } -Reject
    }
    $summary=Read-ArchitecturePerformanceSamples $valid 2 4 4 1280 800
    Check 'same-distributions' { if (-not (Compare-ArchitecturePerformanceDistributions $summary $summary).passed) { throw 'Equal samples failed.' } }
    $changed=($summary | ConvertTo-Json -Depth 12 | ConvertFrom-Json -AsHashtable)
    $changed.distributions['gpu.game.Graphics.Renderer'].median=5.26
    Check 'reject-5-percent-regression' { if ((Compare-ArchitecturePerformanceDistributions $summary $changed).passed) { throw 'Regression passed.' } }
    $changed.distributions['gpu.game.Graphics.Renderer'].median=5.0
    $changed.distributions['gpu.game.Graphics.Renderer'].p95=5.51
    Check 'reject-10-percent-tail' { if ((Compare-ArchitecturePerformanceDistributions $summary $changed).passed) { throw 'Tail regression passed.' } }
    $changed.distributions['gpu.game.Graphics.Renderer'].median=5.25
    $changed.distributions['gpu.game.Graphics.Renderer'].p95=5.5
    Check 'accept-exact-5-and-10-percent-boundary' { if (-not (Compare-ArchitecturePerformanceDistributions $summary $changed).passed) { throw 'Exact threshold was rejected by rounding.' } }
    Check 'reject-cli-short-warmup' { & (Join-Path $root 'scripts/Benchmark-ArchitecturePerformance.ps1') -WarmupFrames 59 -OutputDirectory (Join-Path $output 'invalid-warmup') } -Reject
    Check 'reject-cli-short-measurement' { & (Join-Path $root 'scripts/Benchmark-ArchitecturePerformance.ps1') -SampleFrames 179 -OutputDirectory (Join-Path $output 'invalid-samples') } -Reject
    Check 'reject-too-few-runs' { Get-ArchitecturePerformanceBatchSummary @($summary,$summary) } -Reject
    Check 'reject-too-few-frames' { Get-ArchitecturePerformanceBatchSummary @($summary,$summary,$summary,$summary,$summary) } -Reject
    $longPath=Write-Rows (Make-Rows 186) 'long'
    $longSummary=Read-ArchitecturePerformanceSamples $longPath 2 180 4 1280 800
    Check 'valid-five-run-batch' {
        $batch=Get-ArchitecturePerformanceBatchSummary @($longSummary,$longSummary,$longSummary,$longSummary,$longSummary)
        if ($batch.independentRuns -ne 5 -or $batch.distributions['cpu.frameMs'].median -ne 10 -or
            -not (Compare-ArchitecturePerformanceDistributions $batch $batch).passed) { throw 'Batch aggregate failed.' }
    }
    $fixture=Join-Path $output 'sealed-run'
    New-Item -ItemType Directory -Path $fixture | Out-Null
    Copy-Item $valid (Join-Path $fixture 'frames.jsonl')
    $summary | ConvertTo-Json -Depth 12 | Set-Content (Join-Path $fixture 'summary.json')
    '[]' | Set-Content (Join-Path $fixture 'source-inputs.json')
    'D3D12 debug layer enabled.' | Set-Content (Join-Path $fixture 'process.stderr.log')
    @{format='PrismRuntimePerformanceIdentity';version=1;identity=@{graphicsApi='d3d12';adapterName='fake';vendorId=1;deviceId=2
        driverVersion='test';apiVersion='test';cpuName='fake';buildConfiguration='test';compiler='test';architecture='test';shaderRevision='test';executableHash='test'}} |
        ConvertTo-Json | Set-Content (Join-Path $fixture 'identity.json')
    $sealed=@{format='PrismArchitecturePerformanceRun';version=1;status='measured-and-validated';backend='d3d12';warmupFrames=2;sampleFrames=4;drainFrames=4;width=1280;height=800
        scene='preview';view='game';queueMode='native';cachePolicy='empty';initialImGuiSha256='test';environment=@{PRISM_RENDER_HEADLESS='1'}
        artifacts=@(Get-ChildItem $fixture -File | ForEach-Object { @{path=$_.Name;sha256=(Get-FileHash $_.FullName).Hash} })}
    $sealed | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $fixture 'index.json')
    Check 'sealed-run-and-compatible-inputs' { $run=Read-ArchitecturePerformanceRun $fixture; Assert-ArchitecturePerformanceCompatibility $run $run }
    $reference=Read-ArchitecturePerformanceRun $fixture
    $candidate=Read-ArchitecturePerformanceRun $fixture
    $candidate.identity.driverVersion='changed'
    Check 'reject-driver-change' { Assert-ArchitecturePerformanceCompatibility $reference $candidate } -Reject
    $candidate=Read-ArchitecturePerformanceRun $fixture
    $candidate.index.environment.PRISM_RENDER_HEADLESS='0'
    Check 'reject-window-policy-change' { Assert-ArchitecturePerformanceCompatibility $reference $candidate } -Reject
    function Make-PacingRows {
        $rows = Make-Rows
        $rows[0].metadata.pacingVersion = 1
        foreach ($row in $rows) {
            if ($row.type -ne 'cpu') { continue }
            $row.resources.pacing = @{generation=$row.frameId; slot=$row.views.game.slot; image=$row.views.game.slot; imageCount=2
                api='d3d12';presentMode=-1;syncInterval=1;presentFlags=0;presentResult=0;acquireResult=0;waitResult=0
                beginCompleted=$true;endCompleted=$true;times=@{frameFenceMs=0.2;reclaimMs=0.1;acquireMs=0;imageFenceMs=0
                    uploadMs=0.1;prepareMs=0.1;submitMs=0.1;nativePresentMs=0.1;signalMs=0.1}}
            $row.resources.window = @{x=10;y=20;width=1280;height=800;framebufferWidth=1280;framebufferHeight=800
                scaleX=1;scaleY=1;visible=$false;focused=$false;minimized=$false;displayAvailable=$true;displayName='test-display'
                monitorX=0;monitorY=0;monitorWidth=2560;monitorHeight=1440;refreshHz=144}
            $row.resources.windowObservationMs=0.1
        }
        return ,$rows
    }
    $pacingValid = Write-Rows (Make-PacingRows) 'pacing-valid'
    Check 'pacing-d3d12-valid' {
        $s=Read-ArchitecturePerformanceSamples $pacingValid 2 4 4 1280 800
        if ($s.pacingVersion -ne 1 -or $s.distributions['pacing.frameFenceMs'].count -ne 4) { throw 'Missing pacing metrics.' }
    }
    $vkRows=Make-PacingRows
    foreach ($row in $vkRows) { if ($row.type -eq 'cpu') { $row.resources.pacing.api='vulkan'; $row.resources.pacing.presentMode=2; $row.resources.pacing.syncInterval=0 } }
    $vkPath=Write-Rows $vkRows 'pacing-vulkan'
    Check 'pacing-vulkan-valid' { Read-ArchitecturePerformanceSamples $vkPath 2 4 4 1280 800 }
    foreach ($api in @('d3d12','vulkan')) {
        $rows = Make-PacingRows
        $rows[0].metadata.windowFocusPolicy = 'unfocused'
        foreach ($row in $rows) { if ($row.type -eq 'cpu') {
            $row.resources.window.visible = $true
            if ($api -eq 'vulkan') { $row.resources.pacing.api='vulkan'; $row.resources.pacing.presentMode=1; $row.resources.pacing.syncInterval=0 }
        } }
        $path = Write-Rows $rows "unfocused-$api"
        Check "unfocused-$api-valid" {
            $s = Read-ArchitecturePerformanceSamples $path 2 4 4 1280 800
            if ($s.windowFocusPolicy -ne 'unfocused' -or $s.presentationConditions.focused) { throw 'Wrong focus policy.' }
        }
    }
    foreach ($api in @('d3d12','vulkan')) {
        $rows = Make-PacingRows
        $rows[0].metadata.windowFocusPolicy = 'focused'
        foreach ($row in $rows) { if ($row.type -eq 'cpu') {
            $row.resources.window.visible = $true
            $row.resources.window.focused = $true
            if ($api -eq 'vulkan') { $row.resources.pacing.api='vulkan'; $row.resources.pacing.presentMode=1; $row.resources.pacing.syncInterval=0 }
        } }
        $path = Write-Rows $rows "focused-$api"
        Check "focused-$api-valid" {
            $s = Read-ArchitecturePerformanceSamples $path 2 4 4 1280 800
            if ($s.windowFocusPolicy -ne 'focused' -or -not $s.presentationConditions.focused) { throw 'Wrong focused policy.' }
        }
        @($rows | Where-Object type -eq 'cpu')[2].resources.window.focused = $false
        $badPath = Write-Rows $rows "focused-fault-$api"
        Check "reject-focused-fault-$api" { Read-ArchitecturePerformanceSamples $badPath 2 4 4 1280 800 } -Reject
    }
    foreach ($fault in @('focused','hidden','unknown-policy','no-observer')) {
        $rows = Make-PacingRows
        $rows[0].metadata.windowFocusPolicy = 'unfocused'
        foreach ($row in $rows) { if ($row.type -eq 'cpu') {
            $row.resources.window.visible = $fault -ne 'hidden'
            $row.resources.window.focused = $fault -eq 'focused'
        } }
        if ($fault -eq 'unknown-policy') { $rows[0].metadata.windowFocusPolicy = 'UNFOCUSED' }
        if ($fault -eq 'no-observer') { $rows[0].metadata.Remove('pacingVersion') }
        $path = Write-Rows $rows "unfocused-$fault"
        Check "reject-unfocused-$fault" { Read-ArchitecturePerformanceSamples $path 2 4 4 1280 800 } -Reject
    }
    Check 'reject-unfocused-hidden-cli' { & (Join-Path $root 'scripts/Measure-ArchitecturePerformance.ps1') -UnfocusedWindow -OutputDirectory (Join-Path $output 'bad-focus') -DryRun } -Reject
    Check 'reject-unfocused-hidden-benchmark-cli' { & (Join-Path $root 'scripts/Benchmark-ArchitecturePerformance.ps1') -UnfocusedWindow -OutputDirectory (Join-Path $output 'bad-focus-batch') } -Reject
    Check 'reject-focused-hidden-cli' { & (Join-Path $root 'scripts/Measure-ArchitecturePerformance.ps1') -FocusedWindow -OutputDirectory (Join-Path $output 'bad-focused') -DryRun } -Reject
    Check 'reject-conflicting-focus-cli' { & (Join-Path $root 'scripts/Measure-ArchitecturePerformance.ps1') -FocusedWindow -UnfocusedWindow -VisibleWindow -OutputDirectory (Join-Path $output 'bad-focus-conflict') -DryRun } -Reject
    Check 'unfocused-visible-cli' {
        $request = & (Join-Path $root 'scripts/Measure-ArchitecturePerformance.ps1') -UnfocusedWindow -VisibleWindow -OutputDirectory (Join-Path $output 'focus-dry') -DryRun | ConvertFrom-Json
        if ($request.environment.PRISM_RENDER_PERFORMANCE_WINDOW_FOCUS -ne 'unfocused') { throw 'Focus option was not forwarded.' }
    }
    Check 'focused-visible-cli' {
        $request = & (Join-Path $root 'scripts/Measure-ArchitecturePerformance.ps1') -FocusedWindow -VisibleWindow -OutputDirectory (Join-Path $output 'focused-dry') -DryRun | ConvertFrom-Json
        if ($request.environment.PRISM_RENDER_PERFORMANCE_WINDOW_FOCUS -ne 'focused') { throw 'Focused option was not forwarded.' }
    }
    Check 'game-only-visible-cli' {
        $request = & (Join-Path $root 'scripts/Measure-ArchitecturePerformance.ps1') -ActiveViews game -VisibleWindow -OutputDirectory (Join-Path $output 'game-only-dry') -DryRun | ConvertFrom-Json
        if ($request.activeViewPolicy -ne 'game' -or $request.environment.PRISM_RENDER_PERFORMANCE_ACTIVE_VIEWS -ne 'game') {
            throw 'Game-only policy was not forwarded.'
        }
    }
    $focusReference = Read-ArchitecturePerformanceRun $fixture
    $focusCandidate = Read-ArchitecturePerformanceRun $fixture
    $focusCandidate.summary.windowFocusPolicy = 'unfocused'
    Check 'reject-focus-policy-comparison' { Assert-ArchitecturePerformanceCompatibility $focusReference $focusCandidate } -Reject
    $forgedRequest = Get-Content (Join-Path $fixture 'index.json') -Raw | ConvertFrom-Json -AsHashtable
    $forgedRequest.environment.PRISM_RENDER_PERFORMANCE_WINDOW_FOCUS = 'unfocused'
    $forgedRequest | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $fixture 'index.json')
    Check 'reject-request-raw-focus-policy-mismatch' { Read-ArchitecturePerformanceRun $fixture } -Reject
    $sealed | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $fixture 'index.json')
    foreach ($fault in @('occluded','suboptimal','wait-error','missing','stale','slot','display','refresh','position','focus','minimized','mode','negative','metric-missing','missing-position','missing-status','missing-scale','bad-visible')) {
        $rows=Make-PacingRows
        $sample=@($rows | Where-Object type -eq 'cpu')[3]
        switch ($fault) {
            'occluded' { $sample.resources.pacing.presentResult=142213121 }
            'suboptimal' { $sample.resources.pacing.acquireResult=1000001003 }
            'wait-error' { $sample.resources.pacing.waitResult=258 }
            'missing' { $sample.resources.Remove('pacing') }
            'stale' { $sample.resources.pacing.generation=1 }
            'slot' { $sample.resources.pacing.slot=9 }
            'display' { $sample.resources.window.displayAvailable=$false }
            'refresh' { $sample.resources.window.refreshHz=60 }
            'position' { $sample.resources.window.x=100 }
            'focus' { $sample.resources.window.focused=$true }
            'minimized' { $sample.resources.window.minimized=$true }
            'mode' { $sample.resources.pacing.syncInterval=0 }
            'negative' { $sample.resources.pacing.times.frameFenceMs=-1 }
            'metric-missing' { $sample.resources.pacing.times.Remove('reclaimMs') }
            'missing-position' { $sample.resources.window.Remove('x') }
            'missing-status' { $sample.resources.pacing.Remove('presentResult') }
            'missing-scale' { $sample.resources.window.Remove('scaleX') }
            'bad-visible' { $sample.resources.window.visible='false' }
        }
        $path=Write-Rows $rows "pacing-$fault"
        Check "reject-pacing-$fault" { Read-ArchitecturePerformanceSamples $path 2 4 4 1280 800 } -Reject
    }
    $candidate=Read-ArchitecturePerformanceRun $fixture
    $candidate.summary=Read-ArchitecturePerformanceSamples $pacingValid 2 4 4 1280 800
    Check 'reject-observation-protocol-mismatch' { Assert-ArchitecturePerformanceCompatibility $reference $candidate } -Reject
    $reference.summary=Read-ArchitecturePerformanceSamples $pacingValid 2 4 4 1280 800
    $candidate.summary.presentationConditions.refreshHz=60
    Check 'reject-actual-display-mismatch' { Assert-ArchitecturePerformanceCompatibility $reference $candidate } -Reject
    'corrupted' | Add-Content (Join-Path $fixture 'frames.jsonl')
    Check 'reject-artifact-corruption' { Read-ArchitecturePerformanceRun $fixture } -Reject
} finally { @{tests=$results;passed=@($results | Where-Object { -not $_.passed }).Count -eq 0} | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $output 'tests.json') -Encoding utf8 }
Write-Output "$($results.Count) performance tool tests passed: $output"
