[CmdletBinding()]
param(
    [ValidateSet('d3d12', 'vulkan')][string]$Backend = 'd3d12',
    [string]$Scene = 'hpwater-ocean',
    [long[]]$Frames = @(30, 31, 32, 60, 61, 62, 90),
    [ValidateRange(0, 100000)][long]$EndFrame = 0,
    [ValidateSet('game', 'scene')][string]$View = 'game',
    [ValidateSet('native', 'serial', 'auto')][string]$QueueMode = 'native',
    [ValidateRange(64,8192)][int]$Width = 1280,
    [ValidateRange(64,8192)][int]$Height = 800,
    [ValidateSet('', 'near', 'horizon', 'refraction', 'foam', 'caustics', 'underwater', 'waterline')][string]$WaterCamera = '',
    [ValidateSet('', 'normal', 'high', 'extreme')][string]$WaterQuality = '',
    [ValidateSet('', 'single', 'rgb', 'off')][string]$WaterCaustics = '',
    [ValidateSet('', 'reference', 'calm', 'wake', 'strong-wind')][string]$WaterSurface = '',
    [switch]$WaterRayMarch,
    [switch]$WaterLifecycle,
    [string]$ActionsJson = '',
    [switch]$AssetStreaming,
    [switch]$Headless,
    [string]$BinaryPath = 'build-windows-ci/RelWithDebInfo/PrismRender.exe',
    [Parameter(Mandatory)][string]$OutputDirectory,
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'ArchitectureValidation.Common.ps1')
. (Join-Path $PSScriptRoot 'ArchitectureSequences.Common.ps1')
Assert-ArchitectureSequenceFrames $Frames
$actions = @()
if ($ActionsJson) {
    try { $actions = @($ActionsJson | ConvertFrom-Json -AsHashtable) }
    catch { throw 'ActionsJson must be a JSON array of capture sequence actions.' }
}
Assert-ArchitectureSequenceActions $actions $Frames
if ($EndFrame -eq 0) { $EndFrame = $Frames[-1] }
if ($EndFrame -lt $Frames[-1]) { throw 'EndFrame cannot precede the last sample.' }
if ($WaterLifecycle -and ($Scene -ne 'hpwater-ocean' -or $EndFrame -lt 330)) { throw 'WaterLifecycle requires hpwater-ocean and at least 330 frames.' }
$waterOverrides = $WaterCamera -or $WaterQuality -or $WaterCaustics -or $WaterSurface -or $WaterRayMarch
if ($waterOverrides -and $Scene -notin @('ocean','waveworks-ocean','hpwater-ocean')) { throw 'Water overrides require an Ocean scene.' }
if ($WaterLifecycle -and ($waterOverrides -or $Width -ne 1280 -or $Height -ne 800)) { throw 'WaterLifecycle has a fixed configuration; do not combine it with overrides.' }
$cases = (Get-Content (Join-Path $PSScriptRoot 'ArchitectureDemoCases.json') -Raw | ConvertFrom-Json).cases
$catalog = Get-Content (Join-Path $root 'src/Scene/DemoSceneCatalog.cpp') -Raw
$keys = @([regex]::Matches($catalog, '\{DemoSceneId::\w+,\s*"([^"]+)"') | ForEach-Object { $_.Groups[1].Value })
if ($keys.Count -eq 0 -or $keys.Count -ne $cases.Count -or @($cases.scene | Select-Object -Unique).Count -ne $cases.Count -or
    @(Compare-Object ($keys | Sort-Object) ($cases.scene | Sort-Object)).Count) { throw 'Demo catalog coverage mismatch.' }
if ($Scene -notin $keys) { throw "Unknown scene: $Scene" }
$output = Resolve-ArchitectureOutputPath $root $OutputDirectory
if (Test-Path $output) { throw "Refusing to overwrite sequence evidence: $output" }
$binary = [IO.Path]::GetFullPath($BinaryPath, $root)
$toolsDirectory = Split-Path -Parent $binary
$structuralTool = Join-Path $toolsDirectory 'PrismFrameDiagnosticsCompare.exe'
$visibilityTool = Join-Path $toolsDirectory 'PrismImageVisibilityCheck.exe'
foreach ($file in @($binary, $structuralTool, $visibilityTool)) { if (-not (Test-Path $file -PathType Leaf)) { throw "Missing sequence binary: $file" } }
$environment = @{
    PRISM_RENDER_DETERMINISTIC = '1'; PRISM_RENDER_GPU_VALIDATION = '1'
    PRISM_RENDER_RDG_QUEUE_MODE = $QueueMode; PRISM_RENDER_WIDTH = "$Width"; PRISM_RENDER_HEIGHT = "$Height"
    PRISM_RENDER_CAPTURE_SEQUENCE_PATH = Join-Path $output 'sequence-input.json'
    PRISM_RENDER_CAPTURE_SEQUENCE_OUTPUT = Join-Path $output 'captures'
    PRISM_RENDER_CAPTURE_VIEW = $View; PRISM_RENDER_MAX_FRAMES = "$EndFrame"
    PRISM_RENDER_EXIT_AFTER_CAPTURE = $(if ($EndFrame -eq $Frames[-1]) { '1' } else { '0' })
    PRISM_RENDER_PERFORMANCE_IDENTITY_PATH = Join-Path $output 'identity.json'
    PRISM_RENDER_QUEUE_COST_MODEL_PATH = Join-Path $output 'queue-cache.json'
    PRISM_RENDER_LOG_PATH = Join-Path $output 'application.log'
    PRISM_RENDER_CRASH_REPORT_PATH = Join-Path $output 'crash.json'
    PRISM_RENDER_MINIDUMP_PATH = Join-Path $output 'crash.dmp'
}
if ($WaterCamera) { $environment.PRISM_RENDER_WATER_CAMERA = $WaterCamera }
if ($WaterQuality) { $environment.PRISM_RENDER_WATER_QUALITY = $WaterQuality }
if ($WaterCaustics) { $environment.PRISM_RENDER_WATER_CAUSTICS = $WaterCaustics }
if ($WaterSurface) { $environment.PRISM_RENDER_OCEAN_PRESET = $WaterSurface }
if ($WaterRayMarch) { $environment.PRISM_RENDER_WATER_RAY_MARCH = '1' }
if ($Headless) { $environment.PRISM_RENDER_HEADLESS = '1' }
if ($AssetStreaming) {
    $environment.PRISM_RENDER_ASSET_STREAMING = '1'
    $environment.PRISM_RENDER_ASSET_STREAMING_REPORT_PATH = Join-Path $output 'asset-streaming.json'
    $environment.PRISM_RENDER_ASSET_STREAMING_SCENE_REPORT_PATH = Join-Path $output 'asset-streaming-scene.json'
}
if ($WaterLifecycle) {
    # Match the existing HPWater lifecycle driver's input configuration exactly.
    $environment.PRISM_RENDER_WATER_VALIDATION_SEQUENCE = '1'
    $environment.PRISM_RENDER_WATER_CAMERA = 'underwater'
    $environment.PRISM_RENDER_OCEAN_PRESET = 'reference'
    $environment.PRISM_RENDER_WATER_QUALITY = 'high'
    $environment.PRISM_RENDER_WATER_CAUSTICS = 'single'
    $environment.PRISM_RENDER_WATER_RAY_MARCH = '0'
    $environment.PRISM_RENDER_WATER_VOLUMETRICS = 'on'
}
if ($Backend -eq 'vulkan') {
    $layers = Join-Path $root 'artifacts/vulkan-validation-tools/sdk/Bin'
    if (Test-Path (Join-Path $layers 'VkLayer_khronos_validation.json')) { $environment.VK_LAYER_PATH = $layers }
}
$record = [ordered]@{ format = 'PrismArchitectureCaptureSequence'; version = 1; status = 'planned'
    backend = $Backend; scene = $Scene; view = $View; queueMode = $QueueMode; waterLifecycle = [bool]$WaterLifecycle
    frames = $Frames; endFrame = $EndFrame; actions = $actions; assetStreaming = [bool]$AssetStreaming
    binary = $binary; binarySha256 = (Get-FileHash $binary).Hash
    environment = $environment; arguments = @("--api=$Backend", "--scene=$Scene")
    outputDirectory = $output; limitations = @('Readback waits affect timing; not performance evidence.',
        'Sampling is continuous in one process but only the specified frames have images.',
        'Capture/structure/visibility checks do not replace baseline image comparison or complete control coverage.') }
if ($DryRun) { $record | ConvertTo-Json -Depth 6; return }
New-Item -ItemType Directory -Path $output | Out-Null
$lock = $null
try {
    $lock = [IO.File]::Open((Join-Path $root 'artifacts/architecture-refactor/gpu-validation.lock'),
        [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    $sourceInputs = @(
        foreach ($folder in @('src', 'assets', 'cmake')) { Get-ChildItem (Join-Path $root $folder) -Recurse -File }
        Get-Item (Join-Path $root 'CMakeLists.txt'), (Join-Path $root 'CMakePresets.json')
    ) | Sort-Object FullName | ForEach-Object { @{ path = [IO.Path]::GetRelativePath($root, $_.FullName); sha256 = (Get-FileHash $_.FullName).Hash } }
    $sourceInputs | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $output 'source-inputs.json') -Encoding utf8
    $record['driverSha256'] = (Get-FileHash $PSCommandPath).Hash
    $record['helperSha256'] = (Get-FileHash (Join-Path $PSScriptRoot 'ArchitectureSequences.Common.ps1')).Hash
    $record['tools'] = @($structuralTool, $visibilityTool) | ForEach-Object { @{ path = $_; sha256 = (Get-FileHash $_).Hash } }
    $sequenceInput = if ($actions.Count) { @{ version = 2; frames = $Frames; actions = $actions } }
        else { @{ version = 1; frames = $Frames } }
    $sequenceInput | ConvertTo-Json -Depth 6 | Set-Content $environment.PRISM_RENDER_CAPTURE_SEQUENCE_PATH -Encoding utf8NoBOM
    Copy-Item (Join-Path $root 'artifacts/architecture-refactor/20260828-hpwater-complete/snapshot/imgui.ini') (Join-Path $output 'imgui.ini')
    $record.status = 'running'
    $record | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $output 'index.json') -Encoding utf8
    Invoke-ArchitectureProcess -Binary $binary -Arguments $record.arguments -Environment $environment -WorkingDirectory $output -OutputBase (Join-Path $output 'process') -TimeoutSeconds 600
    Assert-ArchitectureValidationLog (Get-Content (Join-Path $output 'process.stderr.log') -Raw) $Backend
    $trace = Assert-ArchitectureSequenceTrace $environment.PRISM_RENDER_CAPTURE_SEQUENCE_OUTPUT $Frames $EndFrame $View -Actions $actions -WaterLifecycle:$WaterLifecycle
    Assert-ArchitectureSequenceExtents $trace.frames $Width $Height -WaterLifecycle:$WaterLifecycle
    $trace | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $output 'trace-summary.json') -Encoding utf8
    foreach ($frame in $Frames) {
        $base = Join-Path $environment.PRISM_RENDER_CAPTURE_SEQUENCE_OUTPUT "frame-$frame"
        Invoke-ArchitectureProcess -Binary $structuralTool -Arguments @("$base.capture.json", "$base.capture.json", "$base.structure.json") -WorkingDirectory $output -OutputBase "$base.structure"
        Invoke-ArchitectureProcess -Binary $visibilityTool -Arguments @("$base.bmp", "--report=$base.visibility.txt", '--enforce') -WorkingDirectory $output -OutputBase "$base.visibility"
    }
    foreach ($entry in $sourceInputs) {
        if ((Get-FileHash (Join-Path $root $entry.path)).Hash -ne $entry.sha256) { throw "Source changed during capture: $($entry.path)" }
    }
    if ((Get-FileHash $binary).Hash -ne $record.binarySha256) { throw 'Renderer binary changed during capture.' }
    $record.status = 'captured-and-validated'
} catch {
    $record.status = 'failed'; $record['error'] = $_.Exception.Message; throw
} finally {
    if ($lock) { $lock.Dispose() }
    $record['artifacts'] = @(Get-ChildItem $output -Recurse -File | Where-Object FullName -ne (Join-Path $output 'index.json') |
        ForEach-Object { @{ path = [IO.Path]::GetRelativePath($output, $_.FullName); sha256 = (Get-FileHash $_.FullName).Hash } })
    $record | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $output 'index.json') -Encoding utf8
}
Write-Output (Join-Path $output 'index.json')
