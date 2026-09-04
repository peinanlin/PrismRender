[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. (Join-Path $root 'scripts/ArchitectureValidation.Common.ps1')
. (Join-Path $root 'scripts/ArchitectureSequences.Common.ps1')
. (Join-Path $root 'scripts/ArchitectureDemoSequences.Common.ps1')
$output = Join-Path $root ('artifacts/architecture-refactor/tool-tests/sequences-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $output | Out-Null
$results = [Collections.Generic.List[object]]::new()
function Check([string]$Name, [scriptblock]$Action) {
    & $Action
    $results.Add(@{ name = $Name; passed = $true })
    Write-Host "PASS $Name"
}
function Reject([scriptblock]$Action) {
    $failed = $false
    try { & $Action | Out-Null } catch { $failed = $true }
    if (-not $failed) { throw 'Expected sequence validation failure.' }
}
function Clone($Value) { $Value | ConvertTo-Json -Depth 20 | ConvertFrom-Json }
try {
    Check 'Strict ordered bounded sample input' {
        Assert-ArchitectureSequenceFrames @(1,2,5)
        foreach ($invalid in @(@(), @(0), @(-1), @(2,1), @(1,1), @(4294967296))) {
            Reject { Assert-ArchitectureSequenceFrames $invalid }
        }
        Reject { Assert-ArchitectureSequenceFrames @(1..4097) }
    }
    Check 'Dry-run uses one process and never legacy delayed capture' {
        $plan = & (Join-Path $root 'scripts/Capture-ArchitectureSequence.ps1') -Scene preview -Frames 3,4,5 -EndFrame 7 -OutputDirectory (Join-Path $output 'dry-run') -DryRun | ConvertFrom-Json
        if (Test-Path $plan.outputDirectory) { throw 'Dry-run wrote evidence.' }
        if ($plan.environment.PRISM_RENDER_CAPTURE_SEQUENCE_PATH -notlike '*sequence-input.json' -or
            $plan.environment.PRISM_RENDER_EXIT_AFTER_CAPTURE -ne '0' -or
            'PRISM_RENDER_CAPTURE_PATH' -in $plan.environment.PSObject.Properties.Name) { throw 'Invalid continuous capture plan.' }
    }
    Check 'Version 2 controls are strict and forwarded only by sequence capture' {
        $actions = @(@{frame=2;type='set-taa';enabled=$false},@{frame=3;type='set-taa';enabled=$true})
        Assert-ArchitectureSequenceActions $actions @(1,2,3)
        foreach ($invalid in @(
            @(@{frame=0;type='set-taa';enabled=$false}),
            @(@{frame=2;type='set-taa'}),
            @(@{frame=2;type='refresh-scene';enabled=$true}),
            @(@{frame=2;type='unknown'}),
            @(@{frame=3;type='set-taa';enabled=$false},@{frame=2;type='set-taa';enabled=$true}))) {
            Reject { Assert-ArchitectureSequenceActions $invalid @(1,2,3) }
        }
        $json = $actions | ConvertTo-Json -Compress
        $plan = & (Join-Path $root 'scripts/Capture-ArchitectureSequence.ps1') -Scene streaming -Frames 1,2,3 -ActionsJson $json `
            -AssetStreaming -OutputDirectory (Join-Path $output 'actions-dry') -DryRun | ConvertFrom-Json
        if ($plan.actions.Count -ne 2 -or $plan.environment.PRISM_RENDER_ASSET_STREAMING -ne '1' -or
            $plan.environment.PRISM_RENDER_ASSET_STREAMING_SCENE_REPORT_PATH -notlike '*asset-streaming-scene.json') {
            throw 'Controlled sequence plan was not forwarded.'
        }
    }
    Check 'Demo sequence catalog covers all 20 keys and required controls' {
        $catalogPath = Join-Path $root 'scripts/ArchitectureDemoSequences.json'
        $catalog = Read-ArchitectureDemoSequenceCatalog $root $catalogPath
        if ($catalog.cases.Count -ne 20 -or @($catalog.cases | Where-Object { $_.tags -contains 'ocean' }).Count -ne 3 -or
            @($catalog.cases | Where-Object { $_.tags -contains 'fluid' }).Count -ne 4) { throw 'Required sequence coverage changed.' }
        $plan = & (Join-Path $root 'scripts/Validate-ArchitectureDemoSequences.ps1') -Backend vulkan `
            -Scenes preview,post-process -OutputDirectory (Join-Path $output 'catalog-dry') -DryRun | ConvertFrom-Json
        if ($plan.plans.Count -ne 2 -or (Test-Path (Join-Path $output 'catalog-dry'))) { throw 'Demo matrix dry-run wrote output or lost cases.' }
        $bad = $catalog | ConvertTo-Json -Depth 12 | ConvertFrom-Json -AsHashtable
        $bad.cases = @($bad.cases | Where-Object scene -ne 'preview')
        $badPath = Join-Path $output 'bad-catalog.json'
        $bad | ConvertTo-Json -Depth 12 | Set-Content $badPath -Encoding utf8
        Reject { Read-ArchitectureDemoSequenceCatalog $root $badPath }
    }
    Check 'Reject incomplete lifecycle and invalid end frame' {
        Reject { & (Join-Path $root 'scripts/Capture-ArchitectureSequence.ps1') -Frames 3,4,5 -EndFrame 4 -OutputDirectory (Join-Path $output 'bad') -DryRun }
        Reject { & (Join-Path $root 'scripts/Capture-ArchitectureSequence.ps1') -WaterLifecycle -Frames 30 -OutputDirectory (Join-Path $output 'bad') -DryRun }
        Reject { & (Join-Path $root 'scripts/Capture-ArchitectureSequence.ps1') -Scene unknown -OutputDirectory (Join-Path $output 'bad') -DryRun }
        Reject { & (Join-Path $root 'scripts/Capture-ArchitectureSequence.ps1') -Scene preview -WaterCamera near -OutputDirectory (Join-Path $output 'bad') -DryRun }
        Reject { & (Join-Path $root 'scripts/Capture-ArchitectureSequence.ps1') -WaterLifecycle -Frames 330 -WaterQuality high -OutputDirectory (Join-Path $output 'bad') -DryRun }
    }
    Check 'Water matrix covers seven cameras and three qualities at two extents per API' {
        $plan = & (Join-Path $root 'scripts/Validate-ArchitectureWaterBaselines.ps1') -OutputDirectory (Join-Path $output 'matrix-dry') -DryRun | ConvertFrom-Json
        if ($plan.cases.Count -ne 26 -or $plan.repeats -ne 3 -or (Test-Path (Join-Path $output 'matrix-dry'))) { throw 'Incorrect water matrix.' }
        foreach ($api in @('d3d12','vulkan')) {
            if (@($plan.cases | Where-Object { $_.backend -eq $api -and $_.name -like '*-view-*' }).Count -ne 7 -or
                @($plan.cases | Where-Object { $_.backend -eq $api -and $_.name -like '*-quality-*' }).Count -ne 6) { throw 'Incomplete API matrix.' }
        }
        Reject { & (Join-Path $root 'scripts/Validate-ArchitectureWaterBaselines.ps1') -Apis @() -OutputDirectory (Join-Path $output 'bad') -DryRun }
        Reject { & (Join-Path $root 'scripts/Validate-ArchitectureWaterBaselines.ps1') -Modes views,views -OutputDirectory (Join-Path $output 'bad') -DryRun }
    }
    Check 'Reject desktop-adjusted extent, preserve explicit lifecycle resize, headless is opt-in' {
        Assert-ArchitectureSequenceExtents @(@{frame=1;width=2560;height=1417}) 2560 1417
        Reject { Assert-ArchitectureSequenceExtents @(@{frame=1;width=2556;height=1406}) 2560 1417 }
        Assert-ArchitectureSequenceExtents @(@{frame=170;width=960;height=600}) 1280 800 -WaterLifecycle
        Reject { Assert-ArchitectureSequenceExtents @(@{frame=170;width=960;height=590}) 1280 800 -WaterLifecycle }
        $plan = & (Join-Path $root 'scripts/Capture-ArchitectureSequence.ps1') -Headless -OutputDirectory (Join-Path $output 'hidden-dry') -DryRun | ConvertFrom-Json
        if ($plan.environment.PRISM_RENDER_HEADLESS -ne '1') { throw 'Headless test configuration was lost.' }
    }
    $identity = [pscustomobject]@{ backend='d3d12'; scene='preview'; view='game'; queueMode='native'; waterLifecycle=$false
        endFrame=3; frames=@(1,2,3); arguments=@('--api=d3d12','--scene=preview')
        environment=[pscustomobject]@{ PRISM_RENDER_DETERMINISTIC='1'; PRISM_RENDER_WIDTH='1280'; PRISM_RENDER_CAPTURE_SEQUENCE_OUTPUT='a' } }
    Check 'Same backend and all visual inputs must match' {
        Assert-ArchitectureSequenceCompatibility $identity $identity
        $other = Clone $identity; $other.environment.PRISM_RENDER_CAPTURE_SEQUENCE_OUTPUT = 'b'
        Assert-ArchitectureSequenceCompatibility $identity $other
        foreach ($property in @('backend','scene','view','queueMode')) {
            $other = Clone $identity; $other.$property = 'different'
            Reject { Assert-ArchitectureSequenceCompatibility $identity $other }
        }
        $other = Clone $identity; $other.frames = @(1,2,4)
        Reject { Assert-ArchitectureSequenceCompatibility $identity $other }
        $other = Clone $identity; $other.environment.PRISM_RENDER_WIDTH = '640'
        Reject { Assert-ArchitectureSequenceCompatibility $identity $other }
        $other = Clone $identity; $other.environment | Add-Member NoteProperty PRISM_RENDER_NEW_QUALITY 'low'
        Reject { Assert-ArchitectureSequenceCompatibility $identity $other }
    }
    $fixture = Join-Path $output 'fixture'
    New-Item -ItemType Directory -Path $fixture | Out-Null
    $image = Join-Path $fixture 'frame-1.bmp'
    $writer = [IO.BinaryWriter]::new([IO.File]::Open($image, [IO.FileMode]::CreateNew))
    try {
        $writer.Write([uint16]0x4d42); $writer.Write([uint32]70); $writer.Write([uint32]0)
        $writer.Write([uint32]54); $writer.Write([uint32]40); $writer.Write([int32]2); $writer.Write([int32]2)
        $writer.Write([uint16]1); $writer.Write([uint16]32); $writer.Write([uint32]0); $writer.Write([uint32]16)
        foreach ($n in 1..4) { $writer.Write([uint32]0) }
        foreach ($n in 1..4) { $writer.Write([uint32]4286611584) }
    } finally { $writer.Dispose() }
    $view = @{ frameId=1; scene='Preview'; sceneGeneration=1; view='game'; width=2; height=2; simulationTimeSeconds=0 }
    $trace = @{ format='PrismFrameDiagnostics'; frameId=1; scene='Preview'; sceneGeneration=1; simulationTimeSeconds=0; views=@{ game=$view } }
    $capture = Clone @{ format='PrismCaptureDiagnostics'; version=1; recordedFrameId=1; imagePath=$image; view='game'
        sourceKnown=$true; sourceFresh=$true; source=$view; frame=$trace }
    Check 'Capture must match actual frame, view, scene, time, and BMP extent' {
        Assert-ArchitectureSequenceCapture $capture 1 game $image ([pscustomobject]$trace)
        foreach ($property in @('sourceKnown','sourceFresh')) {
            $bad = Clone $capture; $bad.$property = $false
            Reject { Assert-ArchitectureSequenceCapture $bad 1 game $image ([pscustomobject]$trace) }
        }
        foreach ($property in @('frameId','sceneGeneration','width','height','simulationTimeSeconds')) {
            $bad = Clone $capture; $bad.source.$property = 999
            Reject { Assert-ArchitectureSequenceCapture $bad 1 game $image ([pscustomobject]$trace) }
        }
        $bad = Clone $capture; $bad.source.scene = 'Wrong scene'
        Reject { Assert-ArchitectureSequenceCapture $bad 1 game $image ([pscustomobject]$trace) }
        Reject { Assert-ArchitectureSequenceCapture $capture 1 scene $image ([pscustomobject]$trace) }
    }
    $capture | ConvertTo-Json -Depth 10 | Set-Content (Join-Path $fixture 'frame-1.capture.json') -Encoding utf8
    $trace | ConvertTo-Json -Depth 10 -Compress | Set-Content (Join-Path $fixture 'frames.jsonl') -Encoding utf8
    Check 'Trace rejects missing, duplicated, and extra samples' {
        [void](Assert-ArchitectureSequenceTrace $fixture @(1) 1 game)
        Reject { Assert-ArchitectureSequenceTrace $fixture @(1) 2 game }
        Reject { Assert-ArchitectureSequenceTrace $fixture @(1,2) 1 game }
        $trace | ConvertTo-Json -Depth 10 -Compress | Add-Content (Join-Path $fixture 'frames.jsonl') -Encoding utf8
        Reject { Assert-ArchitectureSequenceTrace $fixture @(1) 2 game }
    }
    Check 'Failed run cannot be promoted to a reference' {
        @{ format='PrismArchitectureCaptureSequence'; version=1; status='failed' } | ConvertTo-Json | Set-Content (Join-Path $fixture 'index.json')
        Reject { Read-ArchitectureSequenceRun $root $fixture }
    }
    Check 'Sealed artifacts reject corruption and path traversal' {
        $fake = @{ format='PrismArchitectureCaptureSequence'; version=1; status='captured-and-validated'; frames=@(1)
            artifacts=@(@{ path='frame-1.bmp'; sha256=('0' * 64) }) }
        $fake | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $fixture 'index.json')
        Reject { Read-ArchitectureSequenceRun $root $fixture }
        $fake.artifacts[0].path = '../outside.bmp'
        $fake | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $fixture 'index.json')
        Reject { Read-ArchitectureSequenceRun $root $fixture }
    }
} finally {
    $results.ToArray() | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $output 'results.json') -Encoding utf8
}
Write-Output (Join-Path $output 'results.json')
