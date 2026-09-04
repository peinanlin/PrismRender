# Read-only validation helpers; import ArchitectureValidation.Common.ps1 first.
function Read-ArchitectureSequenceRun([string]$ProjectRoot, [string]$Directory) {
    $directoryPath = Resolve-ArchitectureOutputPath $ProjectRoot $Directory
    $run = Get-Content (Join-Path $directoryPath 'index.json') -Raw | ConvertFrom-Json
    if ($run.format -ne 'PrismArchitectureCaptureSequence' -or $run.version -ne 1 -or
        $run.status -ne 'captured-and-validated') { throw 'Only complete, validated capture sequences can be compared.' }
    Assert-ArchitectureSequenceFrames $run.frames
    $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($entry in $run.artifacts) {
        if ([IO.Path]::IsPathRooted($entry.path) -or $entry.path -match '(^|[\\/])\.\.([\\/]|$)' -or
            -not $seen.Add($entry.path.Replace('\', '/'))) { throw 'Invalid or duplicate sequence artifact path.' }
        $path = Resolve-ArchitectureOutputPath $ProjectRoot (Join-Path $directoryPath $entry.path)
        if (-not $path.StartsWith($directoryPath + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) -or
            $entry.sha256 -notmatch '^[a-fA-F0-9]{64}$' -or (Get-FileHash $path).Hash -ne $entry.sha256) {
            throw 'Sequence artifact hash mismatch.'
        }
    }
    $required = @('sequence-input.json', 'source-inputs.json', 'identity.json', 'captures/frames.jsonl', 'process.stderr.log')
    foreach ($frame in $run.frames) { $required += @("captures/frame-$frame.bmp", "captures/frame-$frame.capture.json") }
    foreach ($path in $required) { if (-not $seen.Contains($path)) { throw "Missing sequence artifact seal: $path" } }
    $sequenceInput = Get-Content (Join-Path $directoryPath 'sequence-input.json') -Raw | ConvertFrom-Json
    [object[]]$actions = @()
    if ($run.PSObject.Properties.Name -contains 'actions') {
        $actions = @($run.actions | ForEach-Object { $_ | ConvertTo-Json -Depth 6 | ConvertFrom-Json -AsHashtable })
    }
    $inputHasActions = $sequenceInput.PSObject.Properties.Name -contains 'actions'
    if ($sequenceInput.version -notin @(1,2) -or ($sequenceInput.frames -join ',') -ne ($run.frames -join ',') -or
        ($inputHasActions -ne ($actions.Count -gt 0)) -or ($actions.Count -and
            (($sequenceInput.actions | ConvertTo-Json -Depth 6 -Compress) -ne ($actions | ConvertTo-Json -Depth 6 -Compress)))) {
        throw 'Sequence input differs from recorded sample plan.'
    }
    Assert-ArchitectureValidationLog (Get-Content (Join-Path $directoryPath 'process.stderr.log') -Raw) $run.backend
    $trace = Assert-ArchitectureSequenceTrace (Join-Path $directoryPath 'captures') $run.frames $run.endFrame $run.view -Actions $actions -WaterLifecycle:$run.waterLifecycle
    Assert-ArchitectureSequenceExtents $trace.frames ([int]$run.environment.PRISM_RENDER_WIDTH) ([int]$run.environment.PRISM_RENDER_HEIGHT) -WaterLifecycle:$run.waterLifecycle
    return @{ directory = $directoryPath; run = $run }
}

function Assert-ArchitectureSequenceExtents([object[]]$Trace, [int]$Width, [int]$Height, [switch]$WaterLifecycle) {
    foreach ($frame in $Trace) {
        $expected = ($frame.width -eq $Width -and $frame.height -eq $Height)
        if ($WaterLifecycle) {
            $expected = ($frame.width -eq 1280 -and $frame.height -eq 800) -or
                ($frame.width -eq 960 -and $frame.height -eq 600)
        }
        if (-not $expected) {
            throw "Actual frame $($frame.frame) extent $($frame.width)x$($frame.height) differs from the requested capture configuration."
        }
    }
}

function Assert-ArchitectureSequenceCompatibility([object]$Reference, [object]$Candidate) {
    foreach ($property in @('backend', 'scene', 'view', 'queueMode', 'waterLifecycle', 'endFrame')) {
        if ($Reference.$property -ne $Candidate.$property) { throw "Sequence configuration mismatch: $property" }
    }
    $referenceStreaming = if ($Reference.PSObject.Properties.Name -contains 'assetStreaming') { $Reference.assetStreaming } else { $false }
    $candidateStreaming = if ($Candidate.PSObject.Properties.Name -contains 'assetStreaming') { $Candidate.assetStreaming } else { $false }
    $referenceActions = if ($Reference.PSObject.Properties.Name -contains 'actions') { @($Reference.actions) } else { @() }
    $candidateActions = if ($Candidate.PSObject.Properties.Name -contains 'actions') { @($Candidate.actions) } else { @() }
    if ($referenceStreaming -ne $candidateStreaming) { throw 'Sequence configuration mismatch: assetStreaming' }
    if (($Reference.frames -join ',') -ne ($Candidate.frames -join ',') -or
        ($Reference.arguments -join '|') -ne ($Candidate.arguments -join '|') -or
        (($referenceActions | ConvertTo-Json -Depth 6 -Compress) -ne ($candidateActions | ConvertTo-Json -Depth 6 -Compress))) {
        throw 'Sequence frames/actions/launch arguments differ.'
    }
    # Only output locations and validation-layer search paths are non-visual.
    # Every other input (including future switches) must match, not be ignored.
    $outputs = @('PRISM_RENDER_CAPTURE_SEQUENCE_PATH', 'PRISM_RENDER_CAPTURE_SEQUENCE_OUTPUT',
        'PRISM_RENDER_PERFORMANCE_IDENTITY_PATH', 'PRISM_RENDER_QUEUE_COST_MODEL_PATH',
        'PRISM_RENDER_LOG_PATH', 'PRISM_RENDER_CRASH_REPORT_PATH', 'PRISM_RENDER_MINIDUMP_PATH',
        'PRISM_RENDER_ASSET_STREAMING_REPORT_PATH', 'PRISM_RENDER_ASSET_STREAMING_SCENE_REPORT_PATH',
        'VK_LAYER_PATH')
    $keys = @(@($Reference.environment.PSObject.Properties.Name) + @($Candidate.environment.PSObject.Properties.Name) | Sort-Object -Unique)
    foreach ($key in $keys) {
        if ($key -in $outputs) { continue }
        if ($key -notin $Reference.environment.PSObject.Properties.Name -or
            $key -notin $Candidate.environment.PSObject.Properties.Name -or
            $Reference.environment.$key -ne $Candidate.environment.$key) { throw "Sequence input mismatch: $key" }
    }
}

function Assert-ArchitectureSequenceFrames([long[]]$Frames) {
    if ($Frames.Count -eq 0 -or $Frames.Count -gt 4096) { throw 'A sequence needs 1..4096 samples.' }
    [long]$previous = 0
    foreach ($frame in $Frames) {
        if ($frame -le $previous -or $frame -gt [uint32]::MaxValue) { throw 'Sample frames must strictly increase within uint32 range.' }
        $previous = $frame
    }
}

function Assert-ArchitectureSequenceActions([object[]]$Actions, [long[]]$Frames) {
    if ($Actions.Count -gt 4096) { throw 'A capture sequence supports at most 4096 actions.' }
    [long]$previousFrame = 0
    [string]$previousType = ''
    foreach ($action in $Actions) {
        if ($action -isnot [Collections.IDictionary] -or -not $action.Contains('frame') -or -not $action.Contains('type') -or
            $action.frame -is [bool] -or $action.frame -is [string] -or [Math]::Truncate([double]$action.frame) -ne [double]$action.frame -or
            $action.frame -lt 1 -or $action.frame -gt $Frames[-1] -or $action.type -isnot [string] -or
            $action.type -cnotin @('activate-demo-scene','activate-streaming','refresh-scene','set-shadows','set-taa')) {
            throw 'Invalid capture sequence action.'
        }
        $setting = $action.type -in @('set-shadows','set-taa')
        $demoScene = $action.type -eq 'activate-demo-scene'
        if (($setting -and ($action.Count -ne 3 -or -not $action.Contains('enabled') -or $action.enabled -isnot [bool])) -or
            ($demoScene -and ($action.Count -ne 3 -or -not $action.Contains('scene') -or $action.scene -isnot [string])) -or
            (-not $setting -and -not $demoScene -and $action.Count -ne 2) -or $action.frame -lt $previousFrame -or
            ($action.frame -eq $previousFrame -and [string]::CompareOrdinal($action.type, $previousType) -le 0)) {
            throw 'Capture sequence actions must be ordered, unique, in range, and type-correct.'
        }
        $previousFrame = [long]$action.frame
        $previousType = $action.type
    }
}

function Assert-ArchitectureSequenceCapture([object]$Capture, [long]$Frame, [string]$View,
    [string]$ImagePath, [object]$Trace) {
    if ($Capture.format -ne 'PrismCaptureDiagnostics' -or $Capture.version -ne 1 -or
        -not $Capture.sourceKnown -or -not $Capture.sourceFresh -or
        $Capture.recordedFrameId -ne $Frame -or $Capture.source.frameId -ne $Frame -or
        $Capture.frame.frameId -ne $Frame -or $Capture.view -ne $View -or $Capture.source.view -ne $View -or
        [IO.Path]::GetFullPath($Capture.imagePath) -ne [IO.Path]::GetFullPath($ImagePath) -or
        $Capture.source.scene -ne $Trace.scene -or $Capture.source.sceneGeneration -ne $Trace.sceneGeneration -or
        $Capture.source.simulationTimeSeconds -ne $Trace.simulationTimeSeconds -or
        $Capture.source.width -ne $Trace.views.$View.width -or $Capture.source.height -ne $Trace.views.$View.height) {
        throw "Sequence sample $Frame does not match its actual frame/view/scene/extent."
    }
    # Header identity is checked independently of the JSON. The existing image
    # tools subsequently validate the complete BMP and compute visibility.
    $stream = [IO.File]::OpenRead($ImagePath)
    $reader = [IO.BinaryReader]::new($stream)
    try {
        if ($stream.Length -lt 54 -or $reader.ReadUInt16() -ne 0x4d42) { throw 'Invalid sequence bitmap.' }
        $stream.Position = 18
        if ($reader.ReadInt32() -ne $Capture.source.width -or
            [Math]::Abs($reader.ReadInt32()) -ne $Capture.source.height) { throw 'Sequence bitmap extent differs from recorded source.' }
    } finally { $reader.Dispose() }
}

function Assert-ArchitectureSequenceTrace([string]$Directory, [long[]]$Frames, [long]$EndFrame,
    [string]$View, [object[]]$Actions = @(), [switch]$WaterLifecycle) {
    Assert-ArchitectureSequenceFrames $Frames
    Assert-ArchitectureSequenceActions $Actions $Frames
    $samples = [Collections.Generic.List[object]]::new()
    $summaries = [Collections.Generic.List[object]]::new()
    [long]$expected = 1
    [int]$nextAction = 0
    $taaExpected = $null
    $shadowsExpected = $null
    $streamingRequested = $false
    $streamingObserved = $false
    $selected = [Collections.Generic.HashSet[long]]::new($Frames)
    foreach ($line in [IO.File]::ReadLines((Join-Path $Directory 'frames.jsonl'))) {
        $trace = $line | ConvertFrom-Json
        if ($trace.format -ne 'PrismFrameDiagnostics' -or $trace.frameId -ne $expected -or
            [Math]::Abs($trace.simulationTimeSeconds - ($expected - 1) / 60.0) -gt 1e-10) {
            throw 'Sequence trace has a gap, duplicate, or incorrect fixed time.'
        }
        while ($nextAction -lt $Actions.Count -and $Actions[$nextAction].frame -eq $expected) {
            $action = $Actions[$nextAction]
            switch ($action.type) {
                'set-taa' { $taaExpected = [bool]$action.enabled }
                'set-shadows' { $shadowsExpected = [bool]$action.enabled }
                'refresh-scene' {
                    if ($trace.views.PSObject.Properties.Name -notcontains 'scene') {
                        throw "Scene refresh action did not render the Scene view at frame $expected."
                    }
                }
                'activate-streaming' { $streamingRequested = $true }
            }
            ++$nextAction
        }
        if ($null -ne $taaExpected -and $trace.views.game.settings.taaEnabled -ne $taaExpected) {
            throw "TAA control was not applied at frame $expected."
        }
        if ($null -ne $shadowsExpected -and $trace.views.game.settings.shadowsEnabled -ne $shadowsExpected) {
            throw "Shadow control was not applied at frame $expected."
        }
        if ($streamingRequested -and $trace.scene -like 'Streamed Asset Scene:*') { $streamingObserved = $true }
        if ($selected.Contains($expected)) {
            $image = Join-Path $Directory "frame-$expected.bmp"
            $capture = Get-Content (Join-Path $Directory "frame-$expected.capture.json") -Raw | ConvertFrom-Json
            Assert-ArchitectureSequenceCapture $capture $expected $View $image $trace
            $samples.Add(@{ frame = $expected; view = $View; scene = $trace.scene; width = $capture.source.width
                height = $capture.source.height; imageSha256 = (Get-FileHash $image).Hash })
        }
        if ($WaterLifecycle) {
            foreach ($count in $trace.sharedSimulationPassCounts.PSObject.Properties) {
                if ($count.Name -ne 'InteractiveTerrain.BrushAndErosion' -and $count.Value -ne 1) {
                    throw 'Ocean/PBF pass-recording chain was duplicated.'
                }
            }
            if ($expected -eq 61 -and $trace.views.game.history.waterOptics.key.explicitResetSerial -ne 1) { throw 'Missing optics reset.' }
            if ($expected -eq 73 -and $trace.views.game.history.oceanResetRequests.local -ne 1) { throw 'Missing local reset.' }
            if ($expected -eq 85 -and $trace.views.game.history.oceanResetRequests.full -ne 1) { throw 'Missing full reset.' }
        }
        $summaries.Add(@{ frame = $expected; scene = $trace.scene; width = $trace.views.game.width; height = $trace.views.game.height })
        ++$expected
    }
    if ($expected - 1 -ne $EndFrame -or $samples.Count -ne $Frames.Count -or
        @(Get-ChildItem $Directory -Filter '*.bmp').Count -ne $Frames.Count -or
        @(Get-ChildItem $Directory -Filter '*.capture.json').Count -ne $Frames.Count) { throw 'Sequence is incomplete or has extra captures.' }
    if ($nextAction -ne $Actions.Count -or ($streamingRequested -and -not $streamingObserved)) {
        throw 'Capture sequence controls did not complete.'
    }
    if ($WaterLifecycle -and ($EndFrame -lt 330 -or
        @($summaries.scene | Select-Object -Unique).Count -ne 3 -or
        -not @($summaries | Where-Object width -eq 960).Count)) { throw 'Incomplete water lifecycle/resize/scene-switch trace.' }
    return @{ completedFrames = $EndFrame; samples = @($samples.ToArray()); frames = @($summaries.ToArray()) }
}
