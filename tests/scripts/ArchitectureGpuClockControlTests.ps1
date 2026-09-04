$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. (Join-Path $root 'scripts/ArchitectureGpuClockControl.Common.ps1')
$plan = Get-ArchitectureGpuClockControlPlan 2 2400
if (($plan.lockArguments -join ' ') -ne '-i 2 -lgc 2400,2400' -or
    ($plan.resetArguments -join ' ') -ne '-i 2 -rgc' -or
    $plan.emergencyRecoveryCommand -ne 'nvidia-smi -i 2 -rgc') { throw 'GPU clock-control plan differs.' }
if (-not (Test-ArchitectureGraphicsClockTarget 2392 2400) -or
    -not (Test-ArchitectureGraphicsClockTarget 2420 2400) -or
    (Test-ArchitectureGraphicsClockTarget 2421 2400)) { throw 'GPU clock target tolerance differs.' }
$dry = & (Join-Path $root 'scripts/Run-ArchitecturePerformanceLockedGpuClock.ps1') `
    -OutputDirectory 'artifacts/architecture-refactor/clock-control-dry-run' -DryRun | ConvertFrom-Json
if (-not $dry.optInSystemStateChange -or -not $dry.resetRequired -or $dry.plan.graphicsClockMHz -ne 2400 -or
    $dry.status -ne 'planned' -or $dry.limitations.Count -ne 3) { throw 'GPU clock-control dry-run contract differs.' }
Write-Output 'Architecture GPU clock control tests passed: 3'
