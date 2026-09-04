[CmdletBinding()]
param(
    [ValidateSet('d3d12', 'vulkan')][string]$Backend = 'd3d12',
    [string]$BinaryPath =
        'build-windows-ci/RelWithDebInfo/PrismRender.exe',
    [Parameter(Mandatory)][string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. (Join-Path $root 'scripts/ArchitectureValidation.Common.ps1')

$output = Resolve-ArchitectureOutputPath $root $OutputDirectory
if (Test-Path -LiteralPath $output) {
    throw "Refusing to overwrite lifecycle evidence: $output"
}
[IO.Directory]::CreateDirectory($output) | Out-Null
$binary = [IO.Path]::GetFullPath($BinaryPath, $root)
if (-not (Test-Path -LiteralPath $binary -PathType Leaf)) {
    throw "Missing renderer: $binary"
}

function Invoke-LifecycleCase(
    [string]$Name,
    [string]$FailureStage,
    [bool]$ExpectedFailure,
    [bool]$ExpectedWaitForGpu) {
    $directory = Join-Path $output $Name
    [IO.Directory]::CreateDirectory($directory) | Out-Null
    $environment = @{
        PRISM_RENDER_DETERMINISTIC = '1'
        PRISM_RENDER_GPU_VALIDATION = '1'
        PRISM_RENDER_HEADLESS = '1'
        PRISM_RENDER_MAX_FRAMES = $(if ($ExpectedFailure) { '1' } else { '3' })
        PRISM_RENDER_LOG_PATH = Join-Path $directory 'application.log'
        PRISM_RENDER_CRASH_REPORT_PATH = Join-Path $directory 'crash.json'
        PRISM_RENDER_MINIDUMP_PATH = Join-Path $directory 'crash.dmp'
    }
    if ($FailureStage) {
        $environment.PRISM_RENDER_TEST_INITIALIZATION_FAILURE_STAGE =
            $FailureStage
    }
    if ($Backend -eq 'vulkan') {
        $layers = Join-Path $root 'artifacts/vulkan-validation-tools/sdk/Bin'
        if (Test-Path (Join-Path $layers 'VkLayer_khronos_validation.json')) {
            $environment.VK_LAYER_PATH = $layers
        }
    }

    $failed = $false
    try {
        Invoke-ArchitectureProcess -Binary $binary `
            -Arguments @("--api=$Backend", '--scene=preview') `
            -Environment $environment -WorkingDirectory $directory `
            -OutputBase (Join-Path $directory 'process') -TimeoutSeconds 120
    }
    catch {
        $failed = $true
    }
    if ($failed -ne $ExpectedFailure) {
        throw "Unexpected process result for lifecycle case: $Name"
    }

    $stderr = Get-Content (Join-Path $directory 'process.stderr.log') -Raw
    if ($ExpectedFailure -and
        $stderr -notmatch
            "Injected application initialization failure at stage: $FailureStage") {
        throw "The expected initialization failure was not reported: $Name"
    }
    if ($FailureStage -ne 'services') {
        Assert-ArchitectureValidationLog $stderr $Backend
    }

    $events = @(Get-Content $environment.PRISM_RENDER_LOG_PATH |
        ForEach-Object { $_ | ConvertFrom-Json })
    $begin = @($events | Where-Object event -eq 'renderer.shutdown.begin')
    $complete = @($events |
        Where-Object event -eq 'renderer.shutdown.completed')
    if ($begin.Count -ne 1 -or $complete.Count -ne 1 -or
        [bool]$begin[0].details.waitForGpu -ne $ExpectedWaitForGpu) {
        throw "Shutdown ownership or wait contract changed: $Name"
    }
    return [ordered]@{
        name = $Name
        failureStage = $FailureStage
        expectedFailure = $ExpectedFailure
        waitForGpu = [bool]$begin[0].details.waitForGpu
        shutdownCount = $begin.Count
        completionCount = $complete.Count
    }
}

$record = [ordered]@{
    format = 'PrismApplicationLifecycleValidation'
    version = 1
    status = 'running'
    backend = $Backend
    binary = $binary
    binarySha256 = (Get-FileHash -LiteralPath $binary).Hash
    cases = @()
}
try {
    $record.cases += Invoke-LifecycleCase 'normal' '' $false $false
    $record.cases += Invoke-LifecycleCase 'services' 'services' $true $false
    foreach ($stage in @('backend', 'content', 'renderers', 'editor')) {
        $record.cases += Invoke-LifecycleCase $stage $stage $true $true
    }
    $record.status = 'passed'
}
catch {
    $record.status = 'failed'
    $record['error'] = $_.Exception.Message
    throw
}
finally {
    $record['artifacts'] = @(Get-ChildItem $output -Recurse -File |
        ForEach-Object {
            @{
                path = [IO.Path]::GetRelativePath($output, $_.FullName)
                sha256 = (Get-FileHash $_.FullName).Hash
            }
        })
    $record | ConvertTo-Json -Depth 8 |
        Set-Content (Join-Path $output 'index.json') -Encoding utf8
}

Write-Output (Join-Path $output 'index.json')
