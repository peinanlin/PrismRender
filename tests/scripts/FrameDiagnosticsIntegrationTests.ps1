[CmdletBinding()]
param(
    [ValidateSet('d3d12', 'vulkan')][string[]]$Apis = @('d3d12', 'vulkan'),
    [ValidateSet('boundaries', 'lifecycle')][string]$Mode = 'boundaries',
    [string]$BinaryPath = 'build-windows-ci/RelWithDebInfo/PrismRender.exe',
    [Parameter(Mandatory)][string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. (Join-Path $root 'scripts/ArchitectureValidation.Common.ps1')
$output = Resolve-ArchitectureOutputPath $root $OutputDirectory
if (Test-Path -LiteralPath $output) { throw "Refusing to overwrite integration evidence: $output" }
$binary = [IO.Path]::GetFullPath($BinaryPath, $root)
New-Item -ItemType Directory -Path $output -ErrorAction Stop | Out-Null
$record = [ordered]@{ format = 'PrismFrameDiagnosticsIntegration'; version = 1; status = 'running'
    mode = $Mode; binary = $binary; binarySha256 = (Get-FileHash $binary).Hash; cases = @() }
$lock = $null
try {
    $lock = [IO.File]::Open((Join-Path $root 'artifacts/architecture-refactor/gpu-validation.lock'),
        [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    foreach ($api in $Apis) {
        $cases = if ($Mode -eq 'boundaries') { @('continue-after-capture', 'capture-write-failure') } else { @('lifecycle') }
        foreach ($case in $cases) {
            $folder = Join-Path $output "$api-$case"
            New-Item -ItemType Directory -Path $folder -ErrorAction Stop | Out-Null
            Copy-Item -LiteralPath (Join-Path $root 'artifacts/architecture-refactor/20260828-hpwater-complete/snapshot/imgui.ini') -Destination (Join-Path $folder 'imgui.ini')
            $plan = & (Join-Path $root 'scripts/Capture-HpWater.ps1') -Api $api -BinaryPath $binary -OutputDirectory $folder -OutputName capture -WorkingDirectory $folder -Validation -QueueMode native -NoClobber -DryRun | ConvertFrom-Json
            $environment = @{}
            foreach ($property in $plan.environment.PSObject.Properties) { $environment[$property.Name] = $property.Value }
            $environment.PRISM_RENDER_FRAME_DIAGNOSTICS_PATH = Join-Path $folder 'frames.jsonl'
            $environment.PRISM_RENDER_CAPTURE_DIAGNOSTICS_PATH = Join-Path $folder 'diagnostics.json'
            $environment.PRISM_RENDER_CAPTURE_DELAY_FRAMES = if ($Mode -eq 'lifecycle') { '330' } else { '3' }
            $environment.PRISM_RENDER_MAX_FRAMES = if ($Mode -eq 'lifecycle') { '340' } else { '7' }
            $environment.PRISM_RENDER_EXIT_AFTER_CAPTURE = if ($case -eq 'continue-after-capture') { '0' } else { '1' }
            $environment.PRISM_RENDER_WATER_VALIDATION_SEQUENCE = if ($Mode -eq 'lifecycle') { '1' } else { '0' }
            if ($Mode -eq 'lifecycle') { $environment.PRISM_RENDER_WATER_CAMERA = 'underwater' }
            $arguments = if ($Mode -eq 'lifecycle') { @("--api=$api", '--scene=hpwater-ocean') } else { @("--api=$api", '--scene=preview') }
            if ($case -eq 'capture-write-failure') {
                # A directory cannot be opened as a bitmap file. Do not make
                # a permission change or touch any existing output to inject failure.
                New-Item -ItemType Directory -Path $environment.PRISM_RENDER_CAPTURE_PATH | Out-Null
            }
            $environment | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $folder 'inputs.json') -Encoding utf8
            $failed = $false
            try {
                Invoke-ArchitectureProcess -Binary $binary -Arguments $arguments -Environment $environment -WorkingDirectory $folder -OutputBase (Join-Path $folder 'process') -TimeoutSeconds 300
            } catch {
                if ($case -ne 'capture-write-failure') { throw }
                $failed = $true
            }
            $stderr = Get-Content (Join-Path $folder 'process.stderr.log') -Raw
            Assert-ArchitectureValidationLog $stderr $api
            $frames = @(Get-Content (Join-Path $folder 'frames.jsonl') | ForEach-Object { $_ | ConvertFrom-Json })
            for ($index = 0; $index -lt $frames.Count; ++$index) {
                if ($frames[$index].frameId -ne $index + 1) { throw 'Frame trace has a gap or duplicate.' }
            }
            if ($case -eq 'capture-write-failure') {
                if (-not $failed -or $frames.Count -ne 3 -or (Test-Path (Join-Path $folder 'diagnostics.json')) -or $stderr -notmatch '(?i)capture') {
                    throw 'Failed bitmap write published success diagnostics or failed for an unrelated reason.'
                }
            } else {
                $capture = Get-Content (Join-Path $folder 'diagnostics.json') -Raw | ConvertFrom-Json
                $expectedCaptureFrame = if ($Mode -eq 'lifecycle') { 330 } else { 3 }
                $expectedFrames = if ($Mode -eq 'lifecycle') { 330 } else { 7 }
                if ($frames.Count -ne $expectedFrames -or $capture.recordedFrameId -ne $expectedCaptureFrame -or
                    $capture.source.frameId -ne $expectedCaptureFrame -or -not $capture.sourceFresh -or
                    -not (Test-Path -LiteralPath $environment.PRISM_RENDER_CAPTURE_PATH -PathType Leaf)) {
                    throw 'Completed capture lost its original frame or stopped the later frame trace.'
                }
                if ($Mode -eq 'lifecycle') {
                    if ($frames[60].views.game.history.waterOptics.key.explicitResetSerial -ne 1 -or
                        $frames[72].views.game.history.oceanResetRequests.local -ne 1 -or
                        $frames[84].views.game.history.oceanResetRequests.full -ne 1 -or
                        -not @($frames | Where-Object { $_.views.game.width -eq 960 }).Count -or
                        @($frames.scene | Select-Object -Unique).Count -lt 3) { throw 'Lifecycle history/resize/scene changes are missing.' }
                    foreach ($frame in $frames) {
                        foreach ($property in $frame.sharedSimulationPassCounts.PSObject.Properties) {
                            # Terrain's existing two-view pass can be a no-op
                            # after its revision was consumed. Preserve that
                            # baseline; this assertion is for ocean/PBF chains.
                            if ($property.Name -ne 'InteractiveTerrain.BrushAndErosion' -and $property.Value -ne 1) {
                                throw 'Ocean/PBF simulation pass was recorded more than once in a frame.'
                            }
                        }
                    }
                }
            }
            $record.cases += @{ api = $api; case = $case; passed = $true; expectedChildFailure = ($case -eq 'capture-write-failure'); frames = $frames.Count }
            Write-Host "PASS $api $case ($($frames.Count) frames)"
        }
    }
    $record.status = 'passed'
} catch {
    $record.status = 'failed'; $record['error'] = $_.Exception.Message
    throw
} finally {
    if ($lock) { $lock.Dispose() }
    $record['artifacts'] = @(Get-ChildItem $output -Recurse -File | ForEach-Object { @{ path = [IO.Path]::GetRelativePath($output, $_.FullName); sha256 = (Get-FileHash $_.FullName).Hash } })
    $record | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $output 'index.json') -Encoding utf8
}
Write-Output (Join-Path $output 'index.json')
