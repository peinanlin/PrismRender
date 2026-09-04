[CmdletBinding()]
param([string]$BaselineId = '20260828-hpwater-complete')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$scripts = Join-Path $root 'scripts'
. (Join-Path $scripts 'ArchitectureValidation.Common.ps1')
$testRoot = Join-Path $root ('artifacts/architecture-refactor/tool-tests/' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot | Out-Null
$results = [Collections.Generic.List[object]]::new()
function Check([string]$Name, [scriptblock]$Action) {
    & $Action
    $results.Add(@{ name = $Name; passed = $true })
    Write-Host "PASS $Name"
}
function Expect-Failure([scriptblock]$Action) {
    $failed = $false
    try { & $Action | Out-Null } catch { $failed = $true }
    if (-not $failed) { throw 'Expected failure was accepted.' }
}
function Write-TestBitmap([string]$Path, [byte]$Value) {
    # Generated test fixture, 32x32 BGR24; rows are naturally four-byte aligned.
    $writer = [IO.BinaryWriter]::new([IO.File]::Open($Path, [IO.FileMode]::CreateNew))
    try {
        $writer.Write([byte]0x42); $writer.Write([byte]0x4d); $writer.Write([uint32](54 + 3072))
        $writer.Write([uint32]0); $writer.Write([uint32]54); $writer.Write([uint32]40)
        $writer.Write([int32]32); $writer.Write([int32]32); $writer.Write([uint16]1); $writer.Write([uint16]24)
        $writer.Write([uint32]0); $writer.Write([uint32]3072)
        for ($index = 0; $index -lt 4; ++$index) { $writer.Write([uint32]0) }
        for ($index = 0; $index -lt 3072; ++$index) { $writer.Write($Value) }
    } finally { $writer.Dispose() }
}
try {
    Check 'PowerShell syntax' {
        foreach ($file in @(Get-ChildItem -LiteralPath $scripts -Filter '*.ps1')) {
            $tokens = $null; $errors = $null
            [void][Management.Automation.Language.Parser]::ParseFile($file.FullName, [ref]$tokens, [ref]$errors)
            if ($errors.Count) { throw "$($file.Name): $errors" }
        }
    }
    Check 'Reject output escape and immutable snapshot' {
        Expect-Failure { Resolve-ArchitectureOutputPath $root '../outside' }
        Expect-Failure { Resolve-ArchitectureOutputPath $root "artifacts/architecture-refactor/$BaselineId/snapshot/output" }
    }
    Check 'All 20 demos planned without writing output' {
        $plan = & (Join-Path $scripts 'Validate-ArchitectureRefactor.ps1') -BaselineId $BaselineId -Repeats 1 -DryRun | ConvertFrom-Json
        if ($plan.commands.Count -ne 20 -or (Test-Path -LiteralPath $plan.outputDirectory)) { throw 'Incorrect dry-run coverage or side effect.' }
        if ($plan.commands[0].environment.PRISM_RENDER_DETERMINISTIC -ne '1') { throw 'Non deterministic plan.' }
    }
    Check 'Missing binary fails before output creation' {
        Expect-Failure { & (Join-Path $scripts 'Validate-ArchitectureRefactor.ps1') -BaselineId $BaselineId -BinaryPath 'missing.exe' -DryRun }
    }
    Check 'Frame diagnostics are opt-in and paired with every capture' {
        $plan = & (Join-Path $scripts 'Validate-ArchitectureRefactor.ps1') -BaselineId $BaselineId -Repeats 1 -FrameDiagnostics -DryRun | ConvertFrom-Json
        foreach ($command in $plan.commands) {
            if ($command.environment.PRISM_RENDER_CAPTURE_DIAGNOSTICS_PATH -notlike '*.capture.json' -or
                "$($command.name).frames.jsonl" -notin $command.expected -or
                "$($command.name).capture.json" -notin $command.expected) { throw 'Unpaired frame diagnostics.' }
        }
        Expect-Failure { & (Join-Path $scripts 'Validate-ArchitectureRefactor.ps1') -BaselineId $BaselineId -Suite gpu -FrameDiagnostics -DryRun }
    }
    Check 'Strict GPU suite forwards validation request' {
        $plan = & (Join-Path $scripts 'Validate-ArchitectureRefactor.ps1') -BaselineId $BaselineId -Suite gpu -Validation -DryRun | ConvertFrom-Json
        if ($plan.commands[0].environment.PRISM_RENDER_GPU_VALIDATION -ne '1') { throw 'GPU validation request was lost.' }
    }
    Check 'Strict capture requires enabled layer and rejects diagnostics after successful exit' {
        Assert-ArchitectureValidationLog 'Vulkan Khronos validation layer enabled.' vulkan
        Assert-ArchitectureValidationLog 'D3D12 debug layer enabled. [D3D12 validation 820] existing warning' d3d12
        Expect-Failure { Assert-ArchitectureValidationLog 'validation unavailable' vulkan }
        Expect-Failure { Assert-ArchitectureValidationLog 'Vulkan Khronos validation layer enabled. [Vulkan validation] error' vulkan }
        Expect-Failure { Assert-ArchitectureValidationLog 'D3D12 debug layer enabled. [D3D12 validation 999] error' d3d12 }
    }
    Check 'HPWater default arguments remain compatible' {
        $plan = & (Join-Path $scripts 'Capture-HpWater.ps1') -DryRun | ConvertFrom-Json
        if ($plan.outputBase -notlike '*artifacts*hpwater-validation*hpwater-ocean_d3d12_default_high_1280x800' -or
            $plan.environment.PRISM_RENDER_CAPTURE_DELAY_FRAMES -ne '30' -or $plan.environment.PRISM_RENDER_RDG_QUEUE_MODE -ne 'auto') {
            throw 'Legacy defaults changed.'
        }
    }
    Check 'HPWater isolated batch forwarding and no clobber' {
        $folder = Join-Path $testRoot 'water'
        $plans = @(& (Join-Path $scripts 'Validate-HpWater.ps1') -Mode benchmark -Apis vulkan -OutputDirectory $folder -NoClobber -DryRun)
        $joined = $plans -join "`n"
        if ($joined.Contains('hpwater-validation') -or -not $joined.Contains('bench_vulkan_extreme_2560x1417')) { throw 'Batch did not forward isolation parameters.' }
        if (Test-Path -LiteralPath $folder) { throw 'Dry-run wrote outputs.' }
        New-Item -ItemType Directory -Path $folder | Out-Null
        [IO.File]::Open((Join-Path $folder 'reserved.bmp'), [IO.FileMode]::CreateNew).Dispose()
        Expect-Failure { & (Join-Path $scripts 'Capture-HpWater.ps1') -OutputDirectory $folder -OutputName reserved -NoClobber -DryRun }
    }
    Check 'Child exit failure and scoped environment' {
        $previousValue = [Environment]::GetEnvironmentVariable('PRISM_RENDER_TEST_ONLY', 'Process')
        $env:PRISM_RENDER_TEST_ONLY = 'parent'
        try {
            Expect-Failure { Invoke-ArchitectureProcess -Binary (Get-Command pwsh).Source -Arguments @('-NoProfile', '-Command', 'exit 7') -WorkingDirectory $testRoot -OutputBase (Join-Path $testRoot 'exit7') }
            Invoke-ArchitectureProcess -Binary (Get-Command pwsh).Source -Arguments @('-NoProfile', '-Command', 'if ($env:PRISM_RENDER_TEST_ONLY) { exit 9 }') -WorkingDirectory $testRoot -OutputBase (Join-Path $testRoot 'environment')
            if ($env:PRISM_RENDER_TEST_ONLY -ne 'parent') { throw 'Parent environment was modified.' }
        } finally { [Environment]::SetEnvironmentVariable('PRISM_RENDER_TEST_ONLY', $previousValue, 'Process') }
    }
    Check 'Image thresholds positive and negative fixtures' {
        $reference = Join-Path $testRoot 'reference.bmp'
        $candidate = Join-Path $testRoot 'candidate.bmp'
        Write-TestBitmap $reference 128
        Write-TestBitmap $candidate 130
        $compare = Join-Path $scripts 'Compare-ArchitectureImages.ps1'
        & $compare -Reference $reference -Candidate $reference -ReferenceBackend d3d12 -CandidateBackend d3d12 -OutputDirectory (Join-Path $testRoot 'identical') | Out-Null
        Expect-Failure { & $compare -Reference $reference -Candidate $candidate -ReferenceBackend d3d12 -CandidateBackend d3d12 -OutputDirectory (Join-Path $testRoot 'same-fail') }
        & $compare -Reference $reference -Candidate $candidate -ReferenceBackend d3d12 -CandidateBackend vulkan -OutputDirectory (Join-Path $testRoot 'cross-pass') | Out-Null
        Expect-Failure { & $compare -Reference $reference -Candidate $reference -ReferenceBackend d3d12 -CandidateBackend d3d12 -OutputDirectory (Join-Path $testRoot 'identical') }
    }
    Check 'Driver child failure retained in evidence index' {
        $id = 'expected-child-failure-' + [guid]::NewGuid().ToString('N')
        Expect-Failure { & (Join-Path $scripts 'Validate-ArchitectureRefactor.ps1') -BaselineId $BaselineId -BinaryPath (Get-Command pwsh).Source -Scenes preview -Repeats 1 -RunId $id }
        $index = Get-Content (Join-Path $root "artifacts/architecture-refactor/$BaselineId/P0/d3d12/$id/index.json") -Raw | ConvertFrom-Json
        if ($index.status -ne 'failed' -or $index.completed.Count -ne 0 -or $index.error -notlike 'Child failed with exit *' -or
            -not (Test-Path -LiteralPath (Join-Path $index.outputDirectory 'preview-game-f30-r1.stderr.log'))) {
            throw 'Did not exercise the child-failure path (a preflight/lock failure is not sufficient).'
        }
    }
    Check 'HPWater summary schema and isolated comparator' {
        $folder = Join-Path $testRoot 'summary'
        New-Item -ItemType Directory -Path $folder | Out-Null
        foreach ($api in @('d3d12', 'vulkan')) {
            foreach ($quality in @('normal', 'high', 'extreme')) {
                foreach ($extent in @('1280x800', '2560x1417')) {
                    @{ passes = @(@{ name = 'WaterOptics.Refraction'; gpuMilliseconds = 1.0 },
                           @{ name = 'WaterOptics.Volume'; gpuMilliseconds = 0.5 })
                       capture = @{ waterMemoryMiB = 10; waterCoverage = @{ fraction = 0.5 }; activeViews = 1
                           refraction = @{ effectiveSamples = 4 }; volumetrics = @{ extent = @(640, 400) }
                           caustics = @{ mode = 1 } } } | ConvertTo-Json -Depth 6 |
                        Set-Content -LiteralPath (Join-Path $folder "bench_${api}_${quality}_$extent.json") -Encoding utf8
                }
            }
            foreach ($view in @('near', 'horizon', 'refraction', 'foam', 'caustics', 'underwater', 'waterline')) {
                Copy-Item -LiteralPath (Join-Path $testRoot 'reference.bmp') -Destination (Join-Path $folder "compare_hpwater-ocean_${api}_$view.bmp")
            }
        }
        $arguments = @{ OutputDirectory = $folder; CompareBinaryPath = (Join-Path $root 'build-windows-ci/RelWithDebInfo/PrismGoldenImageCompare.exe')
            NoClobber = $true; EnforceParity = $true }
        & (Join-Path $scripts 'Summarize-HpWater.ps1') @arguments | Out-Null
        $summary = Get-Content (Join-Path $folder 'acceptance-summary.json') -Raw | ConvertFrom-Json
        if ($summary.format -ne 'PrismWaterAcceptanceSummary' -or $summary.version -ne 1 -or
            $summary.benchmarks.Count -ne 12 -or $summary.backendParity.Count -ne 7 -or
            @($summary.backendParity | Where-Object { -not $_.passed }).Count) { throw 'Summary schema or isolated parity changed.' }
        Expect-Failure { & (Join-Path $scripts 'Summarize-HpWater.ps1') @arguments }
    }
}
finally {
    @{ tests = @($results.ToArray()); count = $results.Count } | ConvertTo-Json -Depth 4 |
        Set-Content -LiteralPath (Join-Path $testRoot 'tests.json') -Encoding utf8
}
Write-Output "Passed $($results.Count) tests. Evidence: $testRoot"
