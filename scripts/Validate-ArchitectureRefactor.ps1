[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidatePattern('^[a-zA-Z0-9][a-zA-Z0-9_-]{0,79}$')][string]$BaselineId,
    [ValidateSet('P0', 'P1', 'P2', 'P3', 'P4', 'P5', 'P6', 'P7', 'P8')][string]$Phase = 'P0',
    [ValidateSet('d3d12', 'vulkan')][string]$Backend = 'd3d12',
    [ValidateSet('demos', 'cpu', 'gpu', 'hpwater')][string]$Suite = 'demos',
    [string]$BinaryPath = 'build-windows-ci/RelWithDebInfo/PrismRender.exe',
    [string]$BuildDirectory = 'build-windows-ci',
    [string]$OutputRoot = 'artifacts/architecture-refactor',
    [ValidatePattern('^[a-zA-Z0-9][a-zA-Z0-9_-]{0,79}$')][string]$RunId = ([DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfff') + '-' + [guid]::NewGuid().ToString('N').Substring(0, 8)),
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')][string]$Configuration = 'RelWithDebInfo',
    [string[]]$Scenes = @(),
    [ValidateSet('game', 'scene')][string]$View = 'game',
    [ValidateRange(64, 8192)][int]$Width = 1280,
    [ValidateRange(64, 8192)][int]$Height = 800,
    [ValidateRange(1, 10)][int]$Repeats = 3,
    [switch]$TemporalSamples,
    [ValidateSet('auto', 'native', 'serial')][string]$QueueMode = 'native',
    [ValidateSet('lifecycle', 'views', 'benchmark', 'details', 'all')][string]$WaterMode = 'views',
    [switch]$Validation,
    [switch]$FrameDiagnostics,
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$projectRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'ArchitectureValidation.Common.ps1')
if ($FrameDiagnostics -and $Suite -ne 'demos') { throw '-FrameDiagnostics currently requires -Suite demos.' }
$root = Resolve-ArchitectureOutputPath $projectRoot $OutputRoot
$runDirectory = Resolve-ArchitectureOutputPath $projectRoot (Join-Path $root "$BaselineId/$Phase/$Backend/$RunId")
if (Test-Path -LiteralPath $runDirectory) { throw "Refusing to overwrite evidence: $runDirectory" }
$binary = [IO.Path]::GetFullPath($BinaryPath, $projectRoot)
$build = [IO.Path]::GetFullPath($BuildDirectory, $projectRoot)
if (-not (Test-Path -LiteralPath $binary -PathType Leaf)) { throw "Missing renderer: $binary" }
$manifestPath = Join-Path $projectRoot "artifacts/architecture-refactor/$BaselineId/snapshot-manifest.json"
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) { throw "Missing source baseline manifest: $manifestPath" }
if ((Get-FileHash -LiteralPath $manifestPath).Hash -ne (Get-Content "$($manifestPath -replace '\.json$', '.sha256')" -Raw).Trim()) {
    throw 'Source baseline manifest seal mismatch.'
}
$commands = [Collections.Generic.List[object]]::new()
if ($Suite -in @('cpu', 'gpu')) {
    if (-not (Test-Path -LiteralPath (Join-Path $build 'CTestTestfile.cmake'))) { throw "Not a configured test build: $build" }
    $ctest = (Get-Command ctest -ErrorAction Stop).Source
    $filter = if ($Suite -eq 'gpu') { '-L' } else { '-LE' }
    $testEnvironment = @{}
    if ($Suite -eq 'gpu' -and $Validation) {
        $testEnvironment.PRISM_RENDER_GPU_VALIDATION = '1'
        $layers = Join-Path $projectRoot 'artifacts/vulkan-validation-tools/sdk/Bin'
        if (Test-Path -LiteralPath (Join-Path $layers 'VkLayer_khronos_validation.json')) { $testEnvironment.VK_LAYER_PATH = $layers }
    }
    $commands.Add(@{ binary = $ctest; arguments = @('--test-dir', $build, '-C', $Configuration,
        '--output-on-failure', '--no-tests=error', $filter, 'gpu', '-j1', '--output-junit', (Join-Path $runDirectory 'ctest.xml'))
        environment = $testEnvironment; name = 'ctest'; expected = @('ctest.xml') })
}
elseif ($Suite -eq 'hpwater') {
    $commands.Add(@{ binary = (Get-Command pwsh -ErrorAction Stop).Source
        arguments = @('-NoProfile', '-File', (Join-Path $PSScriptRoot 'Validate-HpWater.ps1'),
            '-Mode', $WaterMode, '-Apis', $Backend, '-BinaryPath', $binary, '-OutputDirectory', $runDirectory,
            '-WorkingDirectory', $runDirectory, '-NoClobber')
        environment = @{}; name = 'hpwater'; expected = @() })
}
else {
    $cases = (Get-Content (Join-Path $PSScriptRoot 'ArchitectureDemoCases.json') -Raw | ConvertFrom-Json).cases
    $catalogText = Get-Content (Join-Path $projectRoot 'src/Scene/DemoSceneCatalog.cpp') -Raw
    $keys = @([regex]::Matches($catalogText, '\{DemoSceneId::\w+,\s*"([^"]+)"') | ForEach-Object { $_.Groups[1].Value })
    if ($keys.Count -eq 0 -or $cases.Count -ne $keys.Count -or
        @($cases.scene | Select-Object -Unique).Count -ne $cases.Count -or
        @(Compare-Object ($keys | Sort-Object) ($cases.scene | Sort-Object)).Count) { throw 'Demo catalog coverage mismatch.' }
    if ($Scenes.Count) {
        foreach ($scene in $Scenes) { if ($scene -notin $keys) { throw "Unknown scene: $scene" } }
        $cases = @($cases | Where-Object { $_.scene -in $Scenes })
    }
    foreach ($case in $cases) {
        $samples = if ($TemporalSamples) { $case.frames } else { @($case.frames[0]) }
        foreach ($frame in $samples) {
            for ($repeat = 1; $repeat -le $Repeats; ++$repeat) {
                $name = "$($case.scene)-$View-f$frame-r$repeat"
                $base = Join-Path $runDirectory $name
                $environment = @{
                    PRISM_RENDER_DETERMINISTIC = '1'; PRISM_RENDER_RDG_QUEUE_MODE = $QueueMode
                    PRISM_RENDER_WIDTH = "$Width"; PRISM_RENDER_HEIGHT = "$Height"
                    PRISM_RENDER_CAPTURE_PATH = "$base.bmp"; PRISM_RENDER_CAPTURE_VIEW = $View
                    PRISM_RENDER_CAPTURE_DELAY_FRAMES = "$frame"; PRISM_RENDER_EXIT_AFTER_CAPTURE = '1'
                    PRISM_RENDER_MAX_FRAMES = "$($frame + 30)"; PRISM_RENDER_GPU_TIMING_REPORT_PATH = "$base.gpu.json"
                    PRISM_RENDER_RDG_REPORT_PATH = "$base.graph.txt"; PRISM_RENDER_CPU_TRACE_PATH = "$base.cpu.json"
                    PRISM_RENDER_PERFORMANCE_IDENTITY_PATH = "$base.identity.json"
                    PRISM_RENDER_QUEUE_COST_MODEL_PATH = "$base.queue-cache.json"
                    PRISM_RENDER_LOG_PATH = "$base.log"; PRISM_RENDER_CRASH_REPORT_PATH = "$base.crash.json"
                    PRISM_RENDER_MINIDUMP_PATH = "$base.dmp"; PRISM_RENDER_GPU_VALIDATION = $(if ($Validation) { '1' } else { '0' })
                }
                if ($Validation -and $Backend -eq 'vulkan') {
                    $layers = Join-Path $projectRoot 'artifacts/vulkan-validation-tools/sdk/Bin'
                    if (Test-Path -LiteralPath (Join-Path $layers 'VkLayer_khronos_validation.json')) { $environment.VK_LAYER_PATH = $layers }
                }
                $expected = @("$name.bmp", "$name.gpu.json", "$name.graph.txt", "$name.cpu.json", "$name.identity.json")
                if ($FrameDiagnostics) {
                    $environment.PRISM_RENDER_FRAME_DIAGNOSTICS_PATH = "$base.frames.jsonl"
                    $environment.PRISM_RENDER_CAPTURE_DIAGNOSTICS_PATH = "$base.capture.json"
                    $expected += @("$name.frames.jsonl", "$name.capture.json")
                }
                $commands.Add(@{ binary = $binary; arguments = @("--api=$Backend", "--scene=$($case.scene)")
                    environment = $environment; name = $name
                    expected = $expected })
            }
        }
    }
}
$record = [ordered]@{
    format = 'PrismArchitectureValidationRun'; version = 1; baselineId = $BaselineId
    phase = $Phase; backend = $Backend; suite = $Suite; runId = $RunId; status = 'planned'
    createdUtc = [DateTime]::UtcNow.ToString('o'); sourceManifest = $manifestPath
    manifestSha256 = (Get-FileHash -LiteralPath $manifestPath).Hash
    binary = $binary; binarySha256 = (Get-FileHash -LiteralPath $binary).Hash
    outputDirectory = $runDirectory; commands = @($commands.ToArray()); completed = @(); error = $null
    limitations = @('Sample endpoints are separate deterministic runs, not lifecycle sequences.',
        'The legacy graph report is emitted at the first reporting point, not necessarily the captured image frame.',
        'Capture success alone is not image, validation-layer or performance acceptance.')
}
$cachePath = Join-Path $build 'CMakeCache.txt'
if ((Test-Path -LiteralPath $cachePath) -and $binary -eq (Join-Path $build "$Configuration/PrismRender.exe")) {
    $editorSetting = @(Select-String -LiteralPath $cachePath -Pattern '^PRISM_RENDER_BUILD_EDITOR:BOOL=(ON|OFF)$')
    if ($editorSetting.Count -eq 1) { $record['editorEnabled'] = $editorSetting[0].Matches[0].Groups[1].Value -eq 'ON' }
}
if ($DryRun) { $record | ConvertTo-Json -Depth 10; return }
New-Item -ItemType Directory -Path $runDirectory -ErrorAction Stop | Out-Null
$indexPath = Join-Path $runDirectory 'index.json'
$lock = $null
try {
    # Cross-process lock is shared across roots/batches; no GPU work runs concurrently through this driver.
    $lockPath = Join-Path $projectRoot 'artifacts/architecture-refactor/gpu-validation.lock'
    $lock = [IO.File]::Open($lockPath, [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    $sourceInputs = @(
        foreach ($sourceRoot in @('src', 'assets', 'cmake')) {
            Get-ChildItem -LiteralPath (Join-Path $projectRoot $sourceRoot) -Recurse -File
        }
        Get-Item -LiteralPath (Join-Path $projectRoot 'CMakeLists.txt'), (Join-Path $projectRoot 'CMakePresets.json')
    ) | Sort-Object FullName | ForEach-Object {
        @{ path = [IO.Path]::GetRelativePath($projectRoot, $_.FullName).Replace('\', '/')
           sha256 = (Get-FileHash -LiteralPath $_.FullName).Hash; bytes = $_.Length }
    }
    @($sourceInputs) | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $runDirectory 'source-inputs.json') -Encoding utf8
    $record['sourceInputsSha256'] = (Get-FileHash -LiteralPath (Join-Path $runDirectory 'source-inputs.json')).Hash
    $record['driverSha256'] = (Get-FileHash -LiteralPath $PSCommandPath).Hash
    $record.status = 'running'
    $record | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $indexPath -Encoding utf8
    # ImGui uses the working-directory ini. Seed an isolated copy; never let captures write the user's layout.
    $layout = Join-Path $projectRoot "artifacts/architecture-refactor/$BaselineId/snapshot/imgui.ini"
    if (Test-Path -LiteralPath $layout) { Copy-Item -LiteralPath $layout -Destination (Join-Path $runDirectory 'imgui.ini') }
    foreach ($command in $commands) {
        Write-Host "[$Phase/$Backend] $($command.name)"
        $working = $runDirectory
        if ($Suite -eq 'demos') {
            $working = Join-Path $runDirectory "$($command.name)-working"
            New-Item -ItemType Directory -Path $working -ErrorAction Stop | Out-Null
            if (Test-Path -LiteralPath $layout) { Copy-Item -LiteralPath $layout -Destination (Join-Path $working 'imgui.ini') }
        }
        Invoke-ArchitectureProcess -Binary $command.binary -Arguments $command.arguments -Environment $command.environment `
            -WorkingDirectory $working -OutputBase (Join-Path $runDirectory $command.name) -TimeoutSeconds 1800
        if ($Suite -eq 'demos' -and $Validation) {
            Assert-ArchitectureValidationLog (Get-Content -LiteralPath (Join-Path $runDirectory "$($command.name).stderr.log") -Raw) $Backend
        }
        foreach ($artifact in $command.expected) {
            if (-not (Test-Path -LiteralPath (Join-Path $runDirectory $artifact) -PathType Leaf)) { throw "Missing expected artifact: $artifact" }
        }
        $record.completed += $command.name
        $record | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $indexPath -Encoding utf8
    }
    foreach ($inputFile in $sourceInputs) {
        if ((Get-FileHash -LiteralPath (Join-Path $projectRoot $inputFile.path)).Hash -ne $inputFile.sha256) {
            throw "Rendering input changed during validation: $($inputFile.path)"
        }
    }
    $record.status = 'executed' # Comparison and acceptance are separate, never infer 'passed' from process exit alone.
}
catch {
    $record.status = 'failed'
    $record.error = $_.Exception.Message
    throw
}
finally {
    if ($lock) { $lock.Dispose() }
    $record['finishedUtc'] = [DateTime]::UtcNow.ToString('o')
    $record['artifacts'] = @(Get-ChildItem -LiteralPath $runDirectory -File | Where-Object { $_.Name -ne 'index.json' } |
        ForEach-Object { @{ name = $_.Name; bytes = $_.Length; sha256 = (Get-FileHash -LiteralPath $_.FullName).Hash } })
    $record | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $indexPath -Encoding utf8
}
Write-Output $indexPath
