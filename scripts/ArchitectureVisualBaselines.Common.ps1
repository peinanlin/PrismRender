# Read-only baseline resolution. Import ArchitectureValidation.Common.ps1 first.
function Assert-ArchitectureFileHash([string]$Path, [string]$Sha256) {
    if ($Sha256 -notmatch '^[A-Fa-f0-9]{64}$' -or
        (Get-FileHash -LiteralPath $Path -ErrorAction Stop).Hash -ne $Sha256) {
        throw "Baseline evidence hash mismatch: $Path"
    }
}

function Get-ArchitectureDemoSample([string]$RunDirectory, [object]$Run, [object]$Command) {
    if ($Run.format -ne 'PrismArchitectureValidationRun' -or $Run.suite -ne 'demos' -or
        $Run.status -ne 'executed' -or $Command.name -notin $Run.completed) {
        throw 'Only completed Demo capture runs can be compared.'
    }
    if ($Command.name -notmatch '^([a-z0-9-]+)-(game|scene)-f([0-9]+)-r([0-9]+)$') {
        throw "Invalid capture identity: $($Command.name)"
    }
    $scene = $Matches[1]; $view = $Matches[2]; $frame = [int]$Matches[3]
    if (($Command.arguments -join '|') -ne "--api=$($Run.backend)|--scene=$scene") {
        throw 'Unrecognized Demo launch arguments.'
    }
    foreach ($suffix in @('bmp', 'gpu.json', 'identity.json')) {
        $artifactName = "$($Command.name).$suffix"
        $seal = @($Run.artifacts | Where-Object name -eq $artifactName)
        if ($seal.Count -ne 1) { throw "Missing capture artifact seal: $artifactName" }
        Assert-ArchitectureFileHash (Join-Path $RunDirectory $artifactName) $seal[0].sha256
    }
    # Do not silently accept an added quality/seed/control override as Demo defaults.
    $allowed = @('PRISM_RENDER_DETERMINISTIC', 'PRISM_RENDER_RDG_QUEUE_MODE', 'PRISM_RENDER_WIDTH',
        'PRISM_RENDER_HEIGHT', 'PRISM_RENDER_CAPTURE_VIEW', 'PRISM_RENDER_CAPTURE_DELAY_FRAMES',
        'PRISM_RENDER_EXIT_AFTER_CAPTURE', 'PRISM_RENDER_MAX_FRAMES', 'PRISM_RENDER_GPU_VALIDATION', 'VK_LAYER_PATH')
    foreach ($property in $Command.environment.PSObject.Properties) {
        if ($property.Name -notin $allowed -and $property.Name -notmatch '^PRISM_RENDER_(CAPTURE|FRAME_DIAGNOSTICS|CAPTURE_DIAGNOSTICS|GPU_TIMING_REPORT|RDG_REPORT|CPU_TRACE|PERFORMANCE_IDENTITY|QUEUE_COST_MODEL|LOG|CRASH_REPORT|MINIDUMP)_PATH$') {
            throw "Unrecognized capture setting: $($property.Name)"
        }
    }
    $envRecord = $Command.environment
    if ($envRecord.PSObject.Properties.Name -contains 'PRISM_RENDER_CAPTURE_DIAGNOSTICS_PATH') {
        $diagnosticName = "$($Command.name).capture.json"
        $diagnosticPath = Join-Path $RunDirectory $diagnosticName
        $seal = @($Run.artifacts | Where-Object name -eq $diagnosticName)
        if ($seal.Count -ne 1) { throw 'Missing capture diagnostics seal.' }
        Assert-ArchitectureFileHash $diagnosticPath $seal[0].sha256
        $diagnostic = Get-Content -LiteralPath $diagnosticPath -Raw | ConvertFrom-Json
        if ($diagnostic.format -ne 'PrismCaptureDiagnostics' -or $diagnostic.version -ne 1 -or
            -not $diagnostic.sourceKnown -or -not $diagnostic.sourceFresh -or
            $diagnostic.recordedFrameId -ne $frame -or $diagnostic.source.frameId -ne $frame -or
            $diagnostic.view -ne $view -or $diagnostic.source.view -ne $view -or
            [IO.Path]::GetFullPath($diagnostic.imagePath) -ne [IO.Path]::GetFullPath((Join-Path $RunDirectory "$($Command.name).bmp"))) {
            throw 'Capture diagnostics do not identify the requested image frame/view.'
        }
    }
    $capture = (Get-Content -LiteralPath (Join-Path $RunDirectory "$($Command.name).gpu.json") -Raw | ConvertFrom-Json).capture
    $identity = (Get-Content -LiteralPath (Join-Path $RunDirectory "$($Command.name).identity.json") -Raw | ConvertFrom-Json).identity
    if ($identity.graphicsApi -ne $Run.backend -or $envRecord.PRISM_RENDER_CAPTURE_VIEW -ne $view -or
        [int]$envRecord.PRISM_RENDER_CAPTURE_DELAY_FRAMES -ne $frame -or
        $capture.outputExtent[0] -ne [int]$envRecord.PRISM_RENDER_WIDTH -or
        $capture.outputExtent[1] -ne [int]$envRecord.PRISM_RENDER_HEIGHT -or
        $capture.deterministic -ne ($envRecord.PRISM_RENDER_DETERMINISTIC -eq '1') -or
        $capture.validationSequence -or $capture.cameraPreset -ne '' -or $capture.surfacePreset -ne '') {
        throw "Capture metadata disagrees with the requested default input: $($Command.name)"
    }
    # v1 runs did not record the build capability. Only the exact approved binary
    # can supply that legacy fact; new runs must carry it explicitly.
    $editor = if ($Run.PSObject.Properties.Name -contains 'editorEnabled') { $Run.editorEnabled }
        elseif ($Run.binarySha256 -eq 'C42D75E6E948AC02528615D64E815F428185F5097D9C977E27B648EF7E09DCF4') { $true }
        else { $null }
    $scope = [ordered]@{ scene = $scene; backend = $Run.backend; configuration = $identity.buildConfiguration
        editorEnabled = $editor; view = $view; activeViews = $capture.activeViews
        width = $capture.outputExtent[0]; height = $capture.outputExtent[1]; frame = $frame
        completedFrames = $capture.completedFrames; queueMode = $envRecord.PRISM_RENDER_RDG_QUEUE_MODE
        deterministic = $capture.deterministic }
    return [pscustomobject]@{ scope = [pscustomobject]$scope; image = (Join-Path $RunDirectory "$($Command.name).bmp") }
}

function Test-ArchitectureBaselineScope([object]$Scope, [object]$Expected) {
    foreach ($property in $Expected.PSObject.Properties) {
        if ($property.Name -eq 'settings') { continue }
        if ($property.Name -notin $Scope.PSObject.Properties.Name -or $Scope.($property.Name) -ne $property.Value) { return $false }
    }
    return $true
}

function Read-ArchitectureVisualRevisions([string]$ProjectRoot, [string]$CatalogPath) {
    $catalog = Get-Content -LiteralPath $CatalogPath -Raw | ConvertFrom-Json
    if ($catalog.format -ne 'PrismArchitectureVisualBaselineRevisions' -or $catalog.version -ne 1 -or
        [string]::IsNullOrWhiteSpace($catalog.approval)) { throw 'Invalid baseline revision catalog.' }
    Assert-ArchitectureFileHash (Join-Path $ProjectRoot $catalog.sourceSnapshot) $catalog.sourceSnapshotSha256
    $sourceFiles = @{}
    $snapshot = Get-Content -LiteralPath (Join-Path $ProjectRoot $catalog.sourceSnapshot) -Raw | ConvertFrom-Json
    foreach ($file in $snapshot.files) {
        if ($file.path -match '^(src|assets|cmake)/' -or $file.path -in @('CMakeLists.txt', 'CMakePresets.json')) {
            $sourceFiles[$file.path] = $file.sha256
        }
    }
    $root = Resolve-ArchitectureOutputPath $ProjectRoot $catalog.evidenceRoot
    $result = [Collections.Generic.List[object]]::new()
    $keys = [Collections.Generic.HashSet[string]]::new()
    foreach ($case in $catalog.cases) {
        if (-not $keys.Add("$($case.backend)/$($case.scene)")) { throw 'Duplicate approved baseline scope.' }
        $directory = Resolve-ArchitectureOutputPath $ProjectRoot (Join-Path $root $case.repeatRun)
        $run = Get-Content -LiteralPath (Join-Path $directory 'index.json') -Raw | ConvertFrom-Json
        if ($run.binarySha256 -ne $catalog.binarySha256 -or $run.completed.Count -ne 3 -or $run.commands.Count -ne 3) {
            throw 'Approved baseline must retain its exact binary and three independent captures.'
        }
        Assert-ArchitectureFileHash (Join-Path $directory 'source-inputs.json') $run.sourceInputsSha256
        $inputs = @(Get-Content -LiteralPath (Join-Path $directory 'source-inputs.json') -Raw | ConvertFrom-Json)
        if ($inputs.Count -ne $sourceFiles.Count -or @($inputs.path | Select-Object -Unique).Count -ne $inputs.Count) {
            throw 'Captured source set differs from the approved checkpoint.'
        }
        foreach ($inputFile in $inputs) {
            if ($sourceFiles[$inputFile.path] -ne $inputFile.sha256) { throw "Captured source differs: $($inputFile.path)" }
        }
        $first = $null
        for ($repeat = 1; $repeat -le 3; ++$repeat) {
            $name = "$($case.scene)-$($catalog.scope.view)-f$($catalog.scope.frame)-r$repeat"
            $commands = @($run.commands | Where-Object name -eq $name)
            if ($commands.Count -ne 1 -or $commands[0].environment.PRISM_RENDER_GPU_VALIDATION -ne '1') { throw 'Missing strict repeat evidence.' }
            $logName = "$name.stderr.log"
            $logSeal = @($run.artifacts | Where-Object name -eq $logName)
            if ($logSeal.Count -ne 1) { throw 'Missing strict diagnostic seal.' }
            $logPath = Join-Path $directory $logName
            Assert-ArchitectureFileHash $logPath $logSeal[0].sha256
            Assert-ArchitectureValidationLog (Get-Content -LiteralPath $logPath -Raw) $case.backend
            $sample = Get-ArchitectureDemoSample $directory $run $commands[0]
            if ($sample.scope.backend -ne $case.backend -or -not (Test-ArchitectureBaselineScope $sample.scope $catalog.scope) -or
                $sample.scope.completedFrames -ne $catalog.scope.frame) { throw 'Approved sample scope mismatch.' }
            Assert-ArchitectureFileHash $sample.image $case.sha256
            if ($repeat -eq 1) { $first = $sample }
        }
        $previous = Resolve-ArchitectureOutputPath $ProjectRoot (Join-Path $root $case.previousImage)
        Assert-ArchitectureFileHash $previous $case.previousSha256
        $comparison = Get-Content -LiteralPath (Join-Path $root $case.previousComparison) -Raw | ConvertFrom-Json
        if ($comparison.status -ne 'failed' -or $comparison.kind -ne 'same-backend' -or
            $comparison.referenceSha256 -ne $case.previousSha256 -or $comparison.candidateSha256 -ne $case.sha256) {
            throw 'Historical failed comparison was replaced or does not match the retained images.'
        }
        $result.Add([pscustomobject]@{ revision = $catalog.revision; scope = $first.scope
            image = $first.image; sha256 = $case.sha256; previousImage = $previous; previousSha256 = $case.previousSha256 })
    }
    return $result.ToArray()
}
