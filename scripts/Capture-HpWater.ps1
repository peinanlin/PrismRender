[CmdletBinding()]
param(
    [ValidateSet('d3d12', 'vulkan')][string]$Api = 'd3d12',
    [ValidateSet('hpwater-ocean', 'waveworks-ocean', 'ocean')]
    [string]$Scene = 'hpwater-ocean',
    [ValidateSet('default', 'near', 'horizon', 'refraction', 'foam', 'caustics', 'underwater', 'waterline')]
    [string]$Camera = 'default',
    [ValidateSet('normal', 'high', 'extreme')][string]$Quality = 'high',
    [ValidateSet('off', 'single', 'rgb')][string]$Caustics = 'single',
    [string]$DebugView = '',
    [ValidateRange(64, 8192)][int]$Width = 1280,
    [ValidateRange(64, 8192)][int]$Height = 800,
    [ValidateRange(1, 10000)][int]$Frames = 30,
    [ValidateSet('reference', 'calm', 'wake', 'strong-wind')][string]$SurfacePreset = 'reference',
    [switch]$RayMarch,
    [switch]$DisableVolumetrics,
    [switch]$Validation,
    [switch]$Sequence,
    [ValidateSet('auto', 'native', 'serial')][string]$QueueMode = 'auto',
    [ValidateSet('inline', 'pool')][string]$TaskExecutor = 'inline',
    [string]$OutputName = '',
    [string]$BinaryPath = '',
    [string]$OutputDirectory = '',
    [string]$WorkingDirectory = '',
    [switch]$NoClobber,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'
if ($Sequence -and -not $PSBoundParameters.ContainsKey('Frames')) { $Frames = 330 }
$projectRoot = Split-Path -Parent $PSScriptRoot
$executable = if ($BinaryPath) { [IO.Path]::GetFullPath($BinaryPath, $projectRoot) }
    else { Join-Path $projectRoot 'build-windows-ci/RelWithDebInfo/PrismRender.exe' }
$OutputDirectory = if ($OutputDirectory) { [IO.Path]::GetFullPath($OutputDirectory, $projectRoot) }
    else { Join-Path $projectRoot 'artifacts/hpwater-validation' }
$WorkingDirectory = if ($WorkingDirectory) { [IO.Path]::GetFullPath($WorkingDirectory, $projectRoot) } else { $projectRoot }
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) { throw "Missing renderer: $executable" }
if ([string]::IsNullOrEmpty($OutputName)) {
    $OutputName = "${Scene}_${Api}_${Camera}_${Quality}_${Width}x${Height}"
}
if ($OutputName -notmatch '^[a-zA-Z0-9_-]+$') {
    throw 'OutputName must be a simple file stem.'
}
$outputBase = Join-Path $outputDirectory $OutputName
if ($NoClobber -and (Get-ChildItem -LiteralPath $OutputDirectory -Filter "$OutputName.*" -ErrorAction SilentlyContinue)) {
    throw "Capture evidence already exists: $outputBase"
}
$environment = @{
    PRISM_RENDER_DETERMINISTIC = '1'
    PRISM_RENDER_RDG_QUEUE_MODE = $QueueMode
    PRISM_RENDER_TASK_EXECUTOR = $TaskExecutor
    PRISM_RENDER_OCEAN_PRESET = $SurfacePreset
    PRISM_RENDER_GPU_VALIDATION = $(if ($Validation) { '1' } else { '0' })
    PRISM_RENDER_WATER_VALIDATION_SEQUENCE = $(if ($Sequence) { '1' } else { '0' })
    PRISM_RENDER_WIDTH = "$Width"
    PRISM_RENDER_HEIGHT = "$Height"
    PRISM_RENDER_WATER_CAMERA = $Camera
    PRISM_RENDER_WATER_QUALITY = $Quality
    PRISM_RENDER_WATER_CAUSTICS = $Caustics
    PRISM_RENDER_WATER_DEBUG = $DebugView
    PRISM_RENDER_WATER_RAY_MARCH = $(if ($RayMarch) { '1' } else { '0' })
    PRISM_RENDER_WATER_VOLUMETRICS = $(if ($DisableVolumetrics) { 'off' } else { 'on' })
    PRISM_RENDER_CAPTURE_PATH = "$outputBase.bmp"
    PRISM_RENDER_CAPTURE_DELAY_FRAMES = "$Frames"
    PRISM_RENDER_CAPTURE_VIEW = 'game'
    PRISM_RENDER_EXIT_AFTER_CAPTURE = '1'
    PRISM_RENDER_MAX_FRAMES = "$($Frames + 10)"
    PRISM_RENDER_GPU_TIMING_REPORT_PATH = "$outputBase.json"
    PRISM_RENDER_LOG_PATH = "$outputBase.log"
    PRISM_RENDER_CRASH_REPORT_PATH = "$outputBase.crash.json"
    PRISM_RENDER_MINIDUMP_PATH = "$outputBase.dmp"
}
$previous = @{}
if ($NoClobber) {
    $environment['PRISM_RENDER_QUEUE_COST_MODEL_PATH'] = "$outputBase.queue-cache.json"
    $environment['PRISM_RENDER_CPU_TRACE_PATH'] = "$outputBase.cpu.json"
    $environment['PRISM_RENDER_PERFORMANCE_IDENTITY_PATH'] = "$outputBase.identity.json"
    $environment['PRISM_RENDER_RDG_REPORT_PATH'] = "$outputBase.graph.txt"
}
if ($Validation -and $Api -eq 'vulkan') {
    $localLayers = Join-Path $projectRoot 'artifacts/vulkan-validation-tools/sdk/Bin'
    if (Test-Path (Join-Path $localLayers 'VkLayer_khronos_validation.json')) {
        $environment['VK_LAYER_PATH'] = $localLayers
    }
}
if ($DryRun) {
    [ordered]@{ binary = $executable; arguments = @("--api=$Api", "--scene=$Scene")
        outputBase = $outputBase; environment = $environment } | ConvertTo-Json -Depth 5
    return
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
# A retained reservation prevents two isolated invocations from sharing a stem.
if ($NoClobber) {
    $reservation = [IO.File]::Open("$outputBase.reserved", [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    $reservation.Dispose()
}
[ordered]@{ binary = $executable; binarySha256 = (Get-FileHash -LiteralPath $executable).Hash
    arguments = @("--api=$Api", "--scene=$Scene"); environment = $environment
    createdUtc = [DateTime]::UtcNow.ToString('o') } |
    ConvertTo-Json -Depth 5 | Set-Content -LiteralPath "$outputBase.capture-input.json" -Encoding utf8
try {
    foreach ($name in $environment.Keys) {
        $previous[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
        [Environment]::SetEnvironmentVariable($name, $environment[$name], 'Process')
    }
    $captureStarted = [DateTime]::UtcNow
    $process = Start-Process -FilePath $executable -ArgumentList "--api=$Api", "--scene=$Scene" `
        -WorkingDirectory $WorkingDirectory -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput "$outputBase.stdout.log" -RedirectStandardError "$outputBase.stderr.log"
    $completed = $false
    for ($attempt = 0; $attempt -lt 4; ++$attempt) {
        if ($process.WaitForExit(30000)) { $completed = $true; break }
    }
    if (-not $completed) {
        Stop-Process -Id $process.Id
        throw "Capture timed out: $OutputName"
    }
    if ($process.ExitCode -ne 0 -or -not (Test-Path "$outputBase.bmp")) {
        $failure = (Get-Content "$outputBase.stderr.log" -Tail 8) -join "`n"
        throw "Capture failed (exit $($process.ExitCode)): $failure"
    }
    foreach ($artifact in @("$outputBase.bmp", "$outputBase.json")) {
        if (-not (Test-Path $artifact) -or (Get-Item $artifact).LastWriteTimeUtc -lt $captureStarted) {
            throw "Capture did not produce a fresh artifact: $artifact"
        }
    }
    Write-Output "Captured $OutputName"
    Write-Output "$outputBase.bmp"
}
finally {
    foreach ($name in $previous.Keys) {
        [Environment]::SetEnvironmentVariable($name, $previous[$name], 'Process')
    }
}
