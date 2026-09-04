[CmdletBinding()]
param(
    [ValidateSet('d3d12','vulkan')][string]$Backend = 'd3d12',
    [string]$Scene = 'hpwater-ocean',
    [ValidateSet('native','serial')][string]$QueueMode = 'native',
    [ValidateSet('game','scene')][string]$View = 'game',
    [ValidateSet('default','game')][string]$ActiveViews = 'default',
    [ValidateSet('default','full-rebuild','versioned')]
    [string]$ScenePublicationMode = 'default',
    [ValidateRange(1,10000)][int]$WarmupFrames = 60,
    [ValidateRange(1,10000)][int]$SampleFrames = 180,
    [ValidateRange(4,100)][int]$DrainFrames = 8,
    [ValidateRange(64,8192)][int]$Width = 1280,
    [ValidateRange(64,8192)][int]$Height = 800,
    [switch]$VisibleWindow,
    [switch]$FocusedWindow,
    [switch]$UnfocusedWindow,
    [ValidateRange(-32768,32767)][int]$WindowX = 100,
    [ValidateRange(-32768,32767)][int]$WindowY = 100,
    [string]$BinaryPath = 'build-windows-ci/RelWithDebInfo/PrismRender.exe',
    [Parameter(Mandatory)][string]$OutputDirectory,
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'ArchitectureValidation.Common.ps1')
. (Join-Path $PSScriptRoot 'ArchitecturePerformance.Common.ps1')
if ($FocusedWindow -and $UnfocusedWindow) { throw 'FocusedWindow and UnfocusedWindow are mutually exclusive.' }
if (($FocusedWindow -or $UnfocusedWindow) -and -not $VisibleWindow) { throw 'Explicit focus policy requires VisibleWindow.' }
$catalog = (Get-Content (Join-Path $PSScriptRoot 'ArchitectureDemoCases.json') -Raw | ConvertFrom-Json).cases.scene
if ($Scene -notin $catalog) { throw "Unknown performance scene: $Scene" }
$output = Resolve-ArchitectureOutputPath $root $OutputDirectory
if (Test-Path $output) { throw "Refusing to overwrite performance evidence: $output" }
$binary = [IO.Path]::GetFullPath($BinaryPath, $root)
if (-not (Test-Path $binary -PathType Leaf)) { throw "Missing renderer: $binary" }
$total = $WarmupFrames + $SampleFrames + $DrainFrames
$environment = @{
    PRISM_RENDER_DETERMINISTIC = '1'; PRISM_RENDER_GPU_VALIDATION = '1'
    PRISM_RENDER_RDG_QUEUE_MODE = $QueueMode; PRISM_RENDER_WIDTH = "$Width"; PRISM_RENDER_HEIGHT = "$Height"
    PRISM_RENDER_CAPTURE_VIEW = $View; PRISM_RENDER_MAX_FRAMES = "$total"
    PRISM_RENDER_PERFORMANCE_WINDOW_X = "$WindowX"; PRISM_RENDER_PERFORMANCE_WINDOW_Y = "$WindowY"
    PRISM_RENDER_FRAME_PERFORMANCE_PATH = Join-Path $output 'frames.jsonl'
    PRISM_RENDER_PERFORMANCE_IDENTITY_PATH = Join-Path $output 'identity.json'
    PRISM_RENDER_QUEUE_COST_MODEL_PATH = Join-Path $output 'queue-cache.json'
    PRISM_RENDER_LOG_PATH = Join-Path $output 'application.log'
    PRISM_RENDER_CRASH_REPORT_PATH = Join-Path $output 'crash.json'
    PRISM_RENDER_MINIDUMP_PATH = Join-Path $output 'crash.dmp'
}
if (-not $VisibleWindow) { $environment.PRISM_RENDER_HEADLESS = '1' }
if ($FocusedWindow) { $environment.PRISM_RENDER_PERFORMANCE_WINDOW_FOCUS = 'focused' }
elseif ($UnfocusedWindow) { $environment.PRISM_RENDER_PERFORMANCE_WINDOW_FOCUS = 'unfocused' }
if ($ActiveViews -eq 'game') { $environment.PRISM_RENDER_PERFORMANCE_ACTIVE_VIEWS = 'game' }
if ($ScenePublicationMode -ne 'default') {
    $environment.PRISM_RENDER_SCENE_PUBLICATION_MODE = $ScenePublicationMode
}
if ($Backend -eq 'vulkan') {
    $layers = Join-Path $root 'artifacts/vulkan-validation-tools/sdk/Bin'
    if (Test-Path (Join-Path $layers 'VkLayer_khronos_validation.json')) { $environment.VK_LAYER_PATH = $layers }
}
$record = [ordered]@{ format = 'PrismArchitecturePerformanceRun'; version = 1; status = 'planned'
    backend = $Backend; scene = $Scene; view = $View; activeViewPolicy = $ActiveViews
    scenePublicationMode = $ScenePublicationMode
    queueMode = $QueueMode; width = $Width; height = $Height
    warmupFrames = $WarmupFrames; sampleFrames = $SampleFrames; drainFrames = $DrainFrames
    binary = $binary; binarySha256 = (Get-FileHash $binary).Hash; arguments = @("--api=$Backend", "--scene=$Scene")
    environment = $environment; outputDirectory = $output; cachePolicy = 'new-empty-process-local-cost-model; native-or-serial-only'
    limitations = @('Strict validation overhead is included; compare only equal validation/configuration.',
        'OS/driver shader caches are warmed, not cleared or asserted cold; runtime/application caches start empty each process.',
        'Single-run distributions are not the two-batch V3 acceptance gate.',
        'CPU memory is not GPU VRAM; PSO counts exclude external ImGui pipeline creation.') }
if ($DryRun) { $record | ConvertTo-Json -Depth 8; return }
New-Item -ItemType Directory -Path $output | Out-Null
$lock = $null
try {
    $lock = [IO.File]::Open((Join-Path $root 'artifacts/architecture-refactor/gpu-validation.lock'),
        [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    $sourceInputs = @(
        foreach ($folder in @('src','assets','cmake')) { Get-ChildItem (Join-Path $root $folder) -Recurse -File }
        Get-Item (Join-Path $root 'CMakeLists.txt'), (Join-Path $root 'CMakePresets.json')
    ) | Sort-Object FullName | ForEach-Object { @{ path = [IO.Path]::GetRelativePath($root, $_.FullName); sha256 = (Get-FileHash $_.FullName).Hash } }
    $sourceInputs | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $output 'source-inputs.json') -Encoding utf8
    $record['driverSha256'] = (Get-FileHash $PSCommandPath).Hash
    $record['helperSha256'] = (Get-FileHash (Join-Path $PSScriptRoot 'ArchitecturePerformance.Common.ps1')).Hash
    Copy-Item (Join-Path $root 'artifacts/architecture-refactor/20260828-hpwater-complete/snapshot/imgui.ini') (Join-Path $output 'imgui.ini')
    $record['initialImGuiSha256'] = (Get-FileHash (Join-Path $output 'imgui.ini')).Hash
    if (Test-Path $environment.PRISM_RENDER_QUEUE_COST_MODEL_PATH) { throw 'Cost model cache must begin absent.' }
    $record.status = 'running'
    $record | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $output 'index.json') -Encoding utf8
    Invoke-ArchitectureProcess -Binary $binary -Arguments $record.arguments -Environment $environment -WorkingDirectory $output -OutputBase (Join-Path $output 'process') -TimeoutSeconds 900
    Assert-ArchitectureValidationLog (Get-Content (Join-Path $output 'process.stderr.log') -Raw) $Backend
    $summary = Read-ArchitecturePerformanceSamples $environment.PRISM_RENDER_FRAME_PERFORMANCE_PATH $WarmupFrames $SampleFrames $DrainFrames $Width $Height
    if ($summary.pacingVersion -ne 1) { throw 'This measurement driver requires actual pacing/window diagnostics.' }
    $actualWindow = $summary.presentationConditions
    $requestedFocusPolicy = if ($FocusedWindow) { 'focused' } elseif ($UnfocusedWindow) { 'unfocused' } else { 'default' }
    if ($summary.windowFocusPolicy -ne $requestedFocusPolicy) {
        throw 'Actual window focus policy differs from the requested policy.'
    }
    if ($summary.activeViewPolicy -ne $ActiveViews -or
        ($ActiveViews -eq 'game' -and $summary.activeViews -ne 'game')) {
        throw 'Actual active views differ from the requested performance policy.'
    }
    if ($actualWindow.x -ne $WindowX -or $actualWindow.y -ne $WindowY -or $actualWindow.visible -ne $VisibleWindow.IsPresent -or
        $actualWindow.framebufferWidth -ne $Width -or $actualWindow.framebufferHeight -ne $Height) {
        throw 'Actual window placement/visibility/extent differs from the requested measurement conditions.'
    }
    $summary | ConvertTo-Json -Depth 12 | Set-Content (Join-Path $output 'summary.json') -Encoding utf8
    foreach ($entry in $sourceInputs) {
        if ((Get-FileHash (Join-Path $root $entry.path)).Hash -ne $entry.sha256) { throw "Source changed during performance run: $($entry.path)" }
    }
    if ((Get-FileHash $binary).Hash -ne $record.binarySha256) { throw 'Binary changed during performance run.' }
    $record.status = 'measured-and-validated'
} catch {
    $record.status = 'failed'; $record['error'] = $_.Exception.Message; throw
} finally {
    if ($lock) { $lock.Dispose() }
    $record['artifacts'] = @(Get-ChildItem $output -Recurse -File | Where-Object FullName -ne (Join-Path $output 'index.json') |
        ForEach-Object { @{ path = [IO.Path]::GetRelativePath($output, $_.FullName); sha256 = (Get-FileHash $_.FullName).Hash } })
    $record | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $output 'index.json') -Encoding utf8
}
Write-Output (Join-Path $output 'index.json')
