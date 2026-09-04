[CmdletBinding()]
param([ValidateSet('d3d12','vulkan')][string]$Backend = 'd3d12',
    [string]$BinaryPath = 'build-windows-ci/RelWithDebInfo/PrismRender.exe',
    [Parameter(Mandatory)][string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. (Join-Path $root 'scripts/ArchitectureValidation.Common.ps1')
$output = Resolve-ArchitectureOutputPath $root $OutputDirectory
if (Test-Path $output) { throw 'Integration evidence must be new.' }
New-Item -ItemType Directory -Path $output | Out-Null
$binary = [IO.Path]::GetFullPath($BinaryPath, $root)
$results = @()
$lock = [IO.File]::Open((Join-Path $root 'artifacts/architecture-refactor/gpu-validation.lock'),
    [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
try {
    foreach ($case in @('timing-conflict','trace-conflict','rdg-conflict','requires-deterministic','position-pair','position-malformed','position-range',
            'focus-invalid','focus-requires-sampling','focus-hidden','active-views-invalid','active-views-requires-sampling','active-views-scene-capture')) {
        $path = Join-Path $output $case
        New-Item -ItemType Directory -Path $path | Out-Null
        $samplePath = Join-Path $path 'frames.jsonl'
        $environment = @{ PRISM_RENDER_DETERMINISTIC='1';PRISM_RENDER_GPU_VALIDATION='1';PRISM_RENDER_HEADLESS='1'
            PRISM_RENDER_MAX_FRAMES='12';PRISM_RENDER_FRAME_PERFORMANCE_PATH=$samplePath
            PRISM_RENDER_QUEUE_COST_MODEL_PATH=(Join-Path $path 'queue-cache.json') }
        if ($Backend -eq 'vulkan') { $environment.VK_LAYER_PATH=Join-Path $root 'artifacts/vulkan-validation-tools/sdk/Bin' }
        $expected = 'Performance sampling conflicts with'
        switch ($case) {
            'timing-conflict' { $environment.PRISM_RENDER_GPU_TIMING_REPORT_PATH=Join-Path $path 'timing.json' }
            'trace-conflict' { $environment.PRISM_RENDER_CPU_TRACE_PATH=Join-Path $path 'trace.json' }
            'rdg-conflict' { $environment.PRISM_RENDER_RDG_REPORT_PATH=Join-Path $path 'graph.json' }
            'requires-deterministic' { $environment.PRISM_RENDER_DETERMINISTIC='0'; $expected='Performance sampling requires deterministic' }
            'position-pair' { $environment.PRISM_RENDER_PERFORMANCE_WINDOW_X='100'; $expected='Performance window position requires' }
            'position-malformed' { $environment.PRISM_RENDER_PERFORMANCE_WINDOW_X='100x'; $environment.PRISM_RENDER_PERFORMANCE_WINDOW_Y='100'; $expected='Performance window position requires' }
            'position-range' { $environment.PRISM_RENDER_PERFORMANCE_WINDOW_X='32768'; $environment.PRISM_RENDER_PERFORMANCE_WINDOW_Y='100'; $expected='Performance window position requires' }
            'focus-invalid' { $environment.PRISM_RENDER_PERFORMANCE_WINDOW_FOCUS='false'; $expected='Performance window focus must be' }
            'focus-requires-sampling' { $environment.PRISM_RENDER_PERFORMANCE_WINDOW_FOCUS='unfocused'; $environment.Remove('PRISM_RENDER_FRAME_PERFORMANCE_PATH'); $expected='Performance window focus requires performance sampling' }
            'focus-hidden' { $environment.PRISM_RENDER_PERFORMANCE_WINDOW_FOCUS='unfocused'; $expected='Explicit performance window focus requires a visible window' }
            'active-views-invalid' { $environment.PRISM_RENDER_PERFORMANCE_ACTIVE_VIEWS='scene'; $expected='Performance active views must be game' }
            'active-views-requires-sampling' { $environment.PRISM_RENDER_PERFORMANCE_ACTIVE_VIEWS='game'; $environment.Remove('PRISM_RENDER_FRAME_PERFORMANCE_PATH'); $expected='Performance active views require performance sampling' }
            'active-views-scene-capture' { $environment.PRISM_RENDER_PERFORMANCE_ACTIVE_VIEWS='game'; $environment.PRISM_RENDER_CAPTURE_VIEW='scene'; $expected='Game-only performance sampling cannot capture the Scene view' }
        }
        $failed = $false
        try { Invoke-ArchitectureProcess -Binary $binary -Arguments @("--api=$Backend",'--scene=preview') -WorkingDirectory $path -OutputBase (Join-Path $path 'process') -Environment $environment -TimeoutSeconds 180 }
        catch { if ($_.Exception.Message -notmatch 'Child failed with exit') { throw }; $failed=$true }
        $stderr=Get-Content (Join-Path $path 'process.stderr.log') -Raw
        # Focus input validation intentionally precedes window/device creation;
        # these startup negatives must not claim that a GPU layer was exercised.
        if ($case -notlike 'focus-*' -and $case -notin @('active-views-invalid','active-views-requires-sampling')) { Assert-ArchitectureValidationLog $stderr $Backend }
        if (-not $failed -or $stderr -notmatch $expected -or (Test-Path $samplePath)) {
            throw "Performance negative integration did not fail at the intended boundary: $case"
        }
        $results += @{case=$case;passed=$true;binarySha256=(Get-FileHash $binary).Hash
            boundary=$(if ($case -like 'focus-*' -or $case -in @('active-views-invalid','active-views-requires-sampling')) { 'before-window-and-device' } else { 'performance-startup-with-validation' })}
    }
} finally {
    $lock.Dispose()
    @{results=$results;completed=$results.Count;backend=$Backend} | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $output 'index.json') -Encoding utf8
}
Write-Output "$($results.Count) negative performance integration checks passed."
