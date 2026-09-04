$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. (Join-Path $root 'scripts/ArchitectureGpuClockDiagnostic.Common.ps1')
$temporary = Join-Path ([IO.Path]::GetTempPath()) ("prism-gpu-clock-tests-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temporary | Out-Null
try {
    $csv = Join-Path $temporary 'telemetry.csv'
    @(
        '2026/08/29 11:00:00.000, 0, NVIDIA Test, P3, 1575, 1575, 7001, 7, 23.38, 145.00, 43',
        '2026/08/29 11:00:00.100, 0, NVIDIA Test, P0, 3000, 3000, 7001, 80, 100.00, 145.00, 55',
        '2026/08/29 11:00:00.200, 0, NVIDIA Test, P0, 2985, 2985, 7001, 70, 90.00, 145.00, 53') |
        Set-Content -LiteralPath $csv -Encoding ascii
    $rows = @(ConvertFrom-ArchitectureNvidiaSmiCsv $csv)
    $summary = Get-ArchitectureNvidiaSmiSummary $rows
    if ($summary.samples -ne 3 -or $summary.pstates.P0 -ne 2 -or $summary.pstates.P3 -ne 1 -or
        $summary.graphicsClockMHz.median -ne 2985 -or $summary.gpuUtilizationPercent.maximum -ne 80) {
        throw 'Valid NVIDIA telemetry summary differs.'
    }
    Add-Content -LiteralPath $csv -Value '2026/08/29 11:00:00.300, 0,' -Encoding ascii
    if (@(ConvertFrom-ArchitectureNvidiaSmiCsv $csv).Count -ne 3) { throw 'Truncated final telemetry row was not isolated.' }
    @('2026/08/29 11:00:00.000, 0,',
        '2026/08/29 11:00:00.100, 0, NVIDIA Test, P0, 3000, 3000, 7001, 80, 100.00, 145.00, 55') |
        Set-Content -LiteralPath $csv -Encoding ascii
    $failed = $false
    try { ConvertFrom-ArchitectureNvidiaSmiCsv $csv | Out-Null } catch { $failed = $true }
    if (-not $failed) { throw 'Truncated non-final telemetry row was accepted.' }
    'bad, 0, NVIDIA Test, unknown, 1, 1, 1, 1, 1, 1, 1' | Set-Content $csv
    $failed = $false
    try { ConvertFrom-ArchitectureNvidiaSmiCsv $csv | Out-Null } catch { $failed = $true }
    if (-not $failed) { throw 'Invalid pstate was accepted.' }
    $utc = ConvertTo-ArchitectureNvidiaTimestampUtc '2026/08/29 11:00:00.000'
    if ($utc.ToString('o') -notlike '2026-08-29T03:00:00.0000000Z') { throw 'NVIDIA local timestamp was not converted to UTC.' }
    $correlationRows = @([pscustomobject]@{ x = 1.0; y = 3.0 }, [pscustomobject]@{ x = 2.0; y = 2.0 }, [pscustomobject]@{ x = 3.0; y = 1.0 })
    if ([Math]::Abs((Get-ArchitecturePearsonCorrelation $correlationRows x y) + 1.0) -gt 1e-12) { throw 'Pearson correlation differs.' }
    $dryRun = & (Join-Path $root 'scripts/Measure-ArchitectureGpuClockDiagnostic.ps1') `
        -OutputDirectory artifacts/architecture-refactor/gpu-clock-dry-run -DryRun | ConvertFrom-Json
    if (-not $dryRun.diagnosticOnly -or $dryRun.status -ne 'planned' -or
        'This tool never changes clocks, power policy, rendering settings, synchronization, or thresholds.' -notin $dryRun.limitations) {
        throw 'Diagnostic dry-run contract differs.'
    }
    if ($dryRun.pollingMilliseconds -ne 100 -or $dryRun.runsRequested -ne 8) { throw 'Diagnostic dry-run defaults differ.' }
    Write-Output 'Architecture GPU clock diagnostic tests passed: 8'
} finally {
    if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Recurse -Force }
}
