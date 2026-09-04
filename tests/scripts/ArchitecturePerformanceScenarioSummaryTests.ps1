[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$script = Join-Path $root 'scripts/Summarize-ArchitecturePerformanceScenarios.ps1'
$testRoot = Join-Path $root (
    'artifacts/architecture-refactor/tool-tests/performance-scenario-summary-' +
    [guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($testRoot) | Out-Null

function Save-Json([object]$Value, [string]$Path) {
    [IO.Directory]::CreateDirectory((Split-Path -Parent $Path)) | Out-Null
    [IO.File]::WriteAllText($Path, ($Value | ConvertTo-Json -Depth 16),
        [Text.UTF8Encoding]::new($false))
}

function New-Result([string]$Id, [string]$Workload,
    [string]$BinaryKind, [string]$Views, [double]$Fps) {
    $distribution = { param($value) @{ minimum=$value;average=$value;median=$value;p95=$value;p99=$value;maximum=$value } }
    return @{
        id=$Id;workload=$Workload;binaryKind=$BinaryKind;activeViews=$Views
        executableHash="$BinaryKind-hash";adapter=@{vendorId=1;deviceId=2;driverVersion='3'}
        actualLoopFps=& $distribution $Fps;editorLoopMs=& $distribution (1000.0/$Fps)
        lanes=@{mainActiveMs=& $distribution 1.0;mainWaitMs=& $distribution 0.2
            renderActiveMs=& $distribution 0.8;workerActiveMs=$null}
        views=@{gameCpuMs=& $distribution 0.5
            sceneCpuMs=if($Views -eq 'game+scene'){& $distribution 0.3}else{$null}}
        gpuCriticalHintMs=& $distribution 0.4;profilerOverheadMs=& $distribution 0.0002
        waits=@{'frame-fence'=& $distribution 0.01;'image-fence'=& $distribution 0.02
            submit=& $distribution 0.03;'native-present'=& $distribution 0.04}
    }
}

try {
    $scenarioDefinition = Join-Path $testRoot 'scenarios.json'
    Save-Json @{format='PrismArchitecturePerformanceScenarios';version=1} $scenarioDefinition
    $runDirectories = @()
    foreach ($repeat in 1..3) {
        $directory = Join-Path $testRoot "run-$repeat"
        $runDirectories += $directory
        $results = @()
        foreach ($workload in @('empty','preview')) {
            $results += New-Result "$workload-editor-dual-view" $workload editor 'game+scene' (300+$repeat)
            $results += New-Result "$workload-editor-game-only" $workload editor game (500+$repeat)
            $results += New-Result "$workload-standalone-game-only" $workload standalone game (800+$repeat)
        }
        Save-Json @{format='PrismArchitecturePerformanceBenchmark';version=1
            status='measured-and-validated';backend='vulkan';buildConfiguration='Release'
            validation=$true;profilingLevel='basic';queueMode='serial';width=1280;height=800
            warmupFrames=30;sampleFrames=120;drainFrames=8
            scenarioDefinition=$scenarioDefinition;results=$results} (Join-Path $directory 'summary.json')
    }
    $levelPath = Join-Path $testRoot 'levels.json'
    Save-Json @{format='PrismFrameProfilerLevelSummary';version=1
        metadata=@{profilingSequence='off:60,basic:60,detailed:60,capture:60'}
        levels=@{off=@{};basic=@{};detailed=@{};capture=@{}}} $levelPath
    $detailedPath = Join-Path $testRoot 'detailed.json'
    Save-Json @{format='PrismPerformanceSummary';version=1
        distributions=@{'memory.workingSetBytes'=@{median=1};'memory.peakWorkingSetBytes'=@{median=2}
            'memory.privateCommitBytes'=@{median=3};'retirement.pending'=@{median=0}}
        pso=@{graphicsCreatedDuringMeasurement=0;computeCreatedDuringMeasurement=0}} $detailedPath

    $output = Join-Path $testRoot 'valid-output'
    & $script -RunDirectories $runDirectories -ProfilerLevelSummary $levelPath `
        -DetailedPerformanceSummary $detailedPath -OutputDirectory $output | Out-Null
    $report = Get-Content (Join-Path $output 'report.json') -Raw | ConvertFrom-Json
    if ($report.status -ne 'passed' -or $report.independentMatrixCount -ne 3 -or
        $report.scenarios.'preview-editor-game-only'.actualLoopFps.median -ne 502) {
        throw 'Valid scenario aggregate produced the wrong result.'
    }

    try {
        & $script -RunDirectories $runDirectories[0..1] -ProfilerLevelSummary $levelPath `
            -DetailedPerformanceSummary $detailedPath -OutputDirectory (Join-Path $testRoot 'too-few') | Out-Null
        throw 'Two-run aggregate was accepted.'
    } catch {
        if ($_.Exception.Message -eq 'Two-run aggregate was accepted.') { throw }
    }
    $mismatchPath = Join-Path $runDirectories[2] 'summary.json'
    $mismatch = Get-Content $mismatchPath -Raw | ConvertFrom-Json
    $mismatch.width = 1920
    Save-Json $mismatch $mismatchPath
    try {
        & $script -RunDirectories $runDirectories -ProfilerLevelSummary $levelPath `
            -DetailedPerformanceSummary $detailedPath -OutputDirectory (Join-Path $testRoot 'mismatch') | Out-Null
        throw 'Mismatched scenario aggregate was accepted.'
    } catch {
        if ($_.Exception.Message -eq 'Mismatched scenario aggregate was accepted.') { throw }
    }
    Write-Output 'Architecture performance scenario aggregate tests passed.'
} finally {
    Remove-Item -LiteralPath $testRoot -Recurse -Force -ErrorAction SilentlyContinue
}
