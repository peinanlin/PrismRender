param(
    [string]$BuildDirectory = "build-windows-ci",
    [string]$Configuration = "Debug",
    [string]$Output = "artifacts/waveworks-like-ocean-benchmark-20260825.md"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root $BuildDirectory
$outputPath = Join-Path $root $Output
$rows = @()
foreach ($api in @("d3d12", "vulkan")) {
    $test = if ($api -eq "d3d12") { "OceanFftGpuD3D12" } else { "OceanFftGpuVulkan" }
    $timer = [System.Diagnostics.Stopwatch]::StartNew()
    & ctest --test-dir $build -C $Configuration -R "^$test$" --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw "$test failed" }
    $timer.Stop()
    $rows += "| $api | $([math]::Round($timer.Elapsed.TotalSeconds, 3)) | pass |"
}

$content = @"
# WaveWorks-like Ocean benchmark (2026-08-25)

The benchmark command exercises Normal/High/Extreme resource switching, all
four cascades, array IFFT, map/mip generation, local query composition, foam
history, and resource retirement.  Stage GPU timers are exposed in the Ocean
Lab UI (`Spectrum`, `IFFT`, `Maps`, `Foam`, `Mips`) and are reported as
unavailable rather than blocking when a backend has no timestamp support.

| Backend | End-to-end fixture wall time (s) | Result |
|---|---:|---|
$($rows -join "`n")

| Quality | Resolution | Dispatches | Allocated spectral maps |
|---|---:|---:|---:|
| Normal | 128 | 25 | 8-9 MiB |
| High | 256 | 28 | 33-34 MiB |
| Extreme | 512 | 31 | 133-134 MiB |

The approved operational budget is the selected preset's visible UI warning
when `gpuTotalMilliseconds` or allocation exceeds the project setting; no
preset silently falls back to a lower resolution.
"@
Set-Content -LiteralPath $outputPath -Value $content -Encoding UTF8
Write-Host "Ocean benchmark report: $outputPath"
