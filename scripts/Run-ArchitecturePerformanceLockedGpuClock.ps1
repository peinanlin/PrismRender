[CmdletBinding()]
param(
    [ValidateSet('d3d12','vulkan')][string[]]$Backends = @('d3d12'),
    [string[]]$Scenes = @('preview'),
    [ValidateSet('native','serial')][string]$QueueMode = 'native',
    [ValidateSet('game','scene')][string]$View = 'game',
    [ValidateRange(60,10000)][int]$WarmupFrames = 180,
    [ValidateRange(180,10000)][int]$SampleFrames = 900,
    [switch]$VisibleWindow,
    [switch]$FocusedWindow,
    [switch]$UnfocusedWindow,
    [ValidateRange(-32768,32767)][int]$WindowX = 100,
    [ValidateRange(-32768,32767)][int]$WindowY = 100,
    [ValidateRange(0,31)][int]$GpuIndex = 0,
    [ValidateRange(180,4000)][int]$GraphicsClockMHz = 2400,
    [string]$BinaryPath = 'build-windows-ci/RelWithDebInfo/PrismRender.exe',
    [Parameter(Mandatory)][string]$OutputDirectory,
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'ArchitectureValidation.Common.ps1')
. (Join-Path $PSScriptRoot 'ArchitectureGpuClockControl.Common.ps1')
if ($FocusedWindow -and $UnfocusedWindow) { throw 'FocusedWindow and UnfocusedWindow are mutually exclusive.' }
if (($FocusedWindow -or $UnfocusedWindow) -and -not $VisibleWindow) { throw 'Explicit focus policy requires VisibleWindow.' }
$output = Resolve-ArchitectureOutputPath $root $OutputDirectory
if (Test-Path -LiteralPath $output) { throw "Refusing to overwrite locked-clock evidence: $output" }
$smi = Get-Command nvidia-smi -ErrorAction SilentlyContinue
if (-not $smi) { throw 'nvidia-smi is required for this opt-in NVIDIA clock-control run.' }
$plan = Get-ArchitectureGpuClockControlPlan $GpuIndex $GraphicsClockMHz
$benchmarkDirectory = Join-Path $output 'benchmark'
$record = [ordered]@{
    format = 'PrismArchitectureGpuClockControlledBaseline'; version = 1; status = 'planned'
    optInSystemStateChange = $true; resetRequired = $true
    plan = $plan; backends = $Backends; scenes = $Scenes; queueMode = $QueueMode; view = $View
    warmupFrames = $WarmupFrames; sampleFrames = $SampleFrames
    visibleWindow = $VisibleWindow.IsPresent; focusedWindow = $FocusedWindow.IsPresent; unfocusedWindow = $UnfocusedWindow.IsPresent
    binaryPath = [IO.Path]::GetFullPath($BinaryPath, $root)
    benchmarkDirectory = $benchmarkDirectory
    limitations = @(
        'The wrapper changes NVIDIA GPU clock state only for this explicit run and always attempts reset in finally.',
        'Power loss or forced termination outside PowerShell finally cannot be recovered automatically; use emergencyRecoveryCommand.',
        'Clock control removes one DVFS variable but does not waive V3 thresholds or validation requirements.')
}
if ($DryRun) { $record | ConvertTo-Json -Depth 8; return }

New-Item -ItemType Directory -Path $output | Out-Null
$indexPath = Join-Path $output 'index.json'
$record.driverSha256 = (Get-FileHash $PSCommandPath).Hash
$record.commonSha256 = (Get-FileHash (Join-Path $PSScriptRoot 'ArchitectureGpuClockControl.Common.ps1')).Hash
$record.status = 'querying-supported-clocks'
$record | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $indexPath -Encoding utf8
$primaryError = $null
$resetError = $null
$lockAttempted = $false
$lockApplied = $false
try {
    $supported = @(Get-ArchitectureSupportedGraphicsClocks $smi.Source $GpuIndex)
    $record['supportedGraphicsClocksMHz'] = $supported
    if ($GraphicsClockMHz -notin $supported) { throw "Requested graphics clock is not supported: $GraphicsClockMHz MHz" }
    $record['graphicsClockBeforeMHz'] = Get-ArchitectureCurrentGraphicsClock $smi.Source $GpuIndex
    $record.status = 'locking-clock'
    $record | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $indexPath -Encoding utf8
    $lockAttempted = $true
    $lockOutput = @(& $smi.Source @($plan.lockArguments) 2>&1 | ForEach-Object { [string]$_ })
    $lockExitCode = $LASTEXITCODE
    $record['lock'] = @{ exitCode = $lockExitCode; output = $lockOutput }
    if ($lockExitCode -ne 0) { throw "nvidia-smi GPU clock lock failed: $($lockOutput -join ' ')" }
    $lockApplied = $true
    $clockToleranceMHz = 20
    $clockObservations = @()
    $clockDeadline = [Diagnostics.Stopwatch]::StartNew()
    do {
        $currentClock = Get-ArchitectureCurrentGraphicsClock $smi.Source $GpuIndex
        $clockObservations += $currentClock
        if (Test-ArchitectureGraphicsClockTarget $currentClock $GraphicsClockMHz $clockToleranceMHz) { break }
        Start-Sleep -Milliseconds 250
    } while ($clockDeadline.Elapsed.TotalSeconds -lt 5)
    $record['graphicsClockLockToleranceMHz'] = $clockToleranceMHz
    $record['graphicsClockLockObservationsMHz'] = $clockObservations
    $record['graphicsClockAfterLockMHz'] = $clockObservations[-1]
    if (-not (Test-ArchitectureGraphicsClockTarget $clockObservations[-1] $GraphicsClockMHz $clockToleranceMHz)) {
        throw "NVIDIA graphics clock did not converge to the requested lock: requested $GraphicsClockMHz MHz; observed $($clockObservations -join ', ') MHz."
    }
    $record.status = 'benchmark-running'
    $record | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $indexPath -Encoding utf8
    & (Join-Path $PSScriptRoot 'Benchmark-ArchitecturePerformance.ps1') -Backends $Backends -Scenes $Scenes `
        -QueueMode $QueueMode -View $View -WarmupFrames $WarmupFrames -SampleFrames $SampleFrames `
        -VisibleWindow:$VisibleWindow -FocusedWindow:$FocusedWindow -UnfocusedWindow:$UnfocusedWindow -WindowX $WindowX -WindowY $WindowY `
        -BinaryPath $record.binaryPath -OutputDirectory $benchmarkDirectory
    if ($LASTEXITCODE -ne 0) { throw "Locked-clock benchmark exited with code $LASTEXITCODE." }
    $benchmarkIndex = Join-Path $benchmarkDirectory 'index.json'
    if (-not (Test-Path -LiteralPath $benchmarkIndex -PathType Leaf)) { throw 'Locked-clock benchmark did not produce an index.' }
    $record['benchmarkIndexSha256'] = (Get-FileHash -LiteralPath $benchmarkIndex).Hash
} catch {
    $primaryError = $_.Exception.Message
} finally {
    if ($lockAttempted) {
        $attempts = @()
        for ($attempt = 1; $attempt -le 3; ++$attempt) {
            $resetOutput = @(& $smi.Source @($plan.resetArguments) 2>&1 | ForEach-Object { [string]$_ })
            $resetExitCode = $LASTEXITCODE
            $attempts += @{ attempt = $attempt; exitCode = $resetExitCode; output = $resetOutput }
            if ($resetExitCode -eq 0) { break }
            Start-Sleep -Milliseconds 250
        }
        $record['resetAttempts'] = $attempts
        if ($attempts[-1].exitCode -ne 0 -and $lockApplied) {
            $resetError = "GPU clock reset failed after three attempts. Run: $($plan.emergencyRecoveryCommand)"
        } elseif ($attempts[-1].exitCode -ne 0) {
            $record['resetWarning'] = 'Reset was rejected after the lock command was also rejected; no successful clock mutation was reported.'
        } else {
            try { $record['graphicsClockAfterResetMHz'] = Get-ArchitectureCurrentGraphicsClock $smi.Source $GpuIndex }
            catch { $record['graphicsClockAfterResetObservationError'] = $_.Exception.Message }
        }
    }
    if ($primaryError) { $record['error'] = $primaryError }
    if ($resetError) { $record['resetError'] = $resetError }
    $record.status = if ($resetError) { 'reset-failed' } elseif ($primaryError -and $record.Contains('resetWarning')) {
        'lock-rejected-reset-unavailable'
    } elseif ($primaryError) { 'failed-reset-completed' } else { 'baseline-complete-reset-completed' }
    $record['finishedUtc'] = [DateTime]::UtcNow.ToString('o')
    $record | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $indexPath -Encoding utf8
}
if ($resetError) { throw $resetError }
if ($primaryError) { throw $primaryError }
Write-Output $indexPath
