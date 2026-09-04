[CmdletBinding()]
param(
    [string]$BinaryPath = 'build-windows-ci/RelWithDebInfo/PrismSkyAtmosphereGpuTests.exe',
    [ValidateSet('d3d12', 'vulkan')][string[]]$Apis = @('d3d12', 'vulkan'),
    [ValidateSet('main', 'capture')][string]$Suite = 'main',
    [Parameter(Mandatory)][string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$projectRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'ArchitectureValidation.Common.ps1')
$output = Resolve-ArchitectureOutputPath $projectRoot $OutputDirectory
$binary = [IO.Path]::GetFullPath($BinaryPath, $projectRoot)
if (-not (Test-Path -LiteralPath $binary -PathType Leaf)) { throw "Missing fixture: $binary" }
if (Test-Path -LiteralPath $output) { throw "Refusing to overwrite evidence: $output" }
$layers = Join-Path $projectRoot 'artifacts/vulkan-validation-tools/sdk/Bin'
if ('vulkan' -in $Apis -and -not (Test-Path -LiteralPath (Join-Path $layers 'VkLayer_khronos_validation.json'))) {
    throw 'Strict Vulkan tests require the local Khronos validation layer.'
}
New-Item -ItemType Directory -Path $output -ErrorAction Stop | Out-Null
$record = [ordered]@{
    format = 'PrismOptionalFeatureBindings'; version = 1; suite = $Suite
    binary = $binary; binarySha256 = (Get-FileHash -LiteralPath $binary).Hash
    status = 'running'; completed = @(); error = $null
}
$indexPath = Join-Path $output 'index.json'
$gpuLock = $null
try {
    $gpuLock = [IO.File]::Open((Join-Path $projectRoot 'artifacts/architecture-refactor/gpu-validation.lock'),
        'OpenOrCreate', 'ReadWrite', 'None')
    $cases = if ($Suite -eq 'main') {
        @(@{ name = 'default'; argument = ''; stage = '' },
          @{ name = 'optional'; argument = '--optional-bindings'; stage = '' },
          @{ name = 'ocean'; argument = '--ocean-bindings'; stage = '' },
          @{ name = 'water'; argument = '--water-bindings'; stage = '' })
    } else {
        @(@{ name = 'bloom'; argument = '--optional-bindings'; stage = 'bloom' },
          @{ name = 'gbuffer-forward'; argument = '--lighting-preset'; stage = 'gbuffer0' },
          @{ name = 'water'; argument = '--water-bindings'; stage = 'water-gbuffer0' })
    }
    $queues = if ($Suite -eq 'main') { @('native', 'serial') } else { @('native') }
    foreach ($api in $Apis) {
        foreach ($queue in $queues) {
            foreach ($case in $cases) {
                $name = "$api-$queue-$($case.name)"
                $arguments = @($api)
                if ($case.argument) { $arguments += $case.argument }
                $environment = @{
                    PRISM_RENDER_HEADLESS = '1'; PRISM_RENDER_GPU_VALIDATION = '1'
                    PRISM_RENDER_RDG_QUEUE_MODE = $queue
                    PRISM_RENDER_QUEUE_COST_MODEL_PATH = (Join-Path $output "$name.queue-cache.json")
                }
                if ($api -eq 'vulkan') { $environment.VK_LAYER_PATH = $layers }
                if ($case.stage) { $environment.PRISM_RENDER_CAPTURE_STAGE = $case.stage }
                Write-Output "Strict $name"
                Invoke-ArchitectureProcess -Binary $binary -Arguments $arguments -WorkingDirectory $output `
                    -OutputBase (Join-Path $output $name) -Environment $environment
                # Check even successful exits: validation may report during teardown.
                $stderr = Get-Content -LiteralPath (Join-Path $output "$name.stderr.log") -Raw
                if ($stderr -match '\[Vulkan validation\]|D3D12 validation (?!820\])') {
                    throw "Unexpected validation diagnostics: $name"
                }
                if ($api -eq 'vulkan' -and $stderr -notmatch 'Vulkan Khronos validation layer enabled\.') {
                    throw "Vulkan validation was not actually enabled: $name"
                }
                $record.completed += @{ name = $name; arguments = $arguments; environment = $environment }
                $record | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $indexPath -Encoding utf8
            }
        }
    }
    if ((Get-FileHash -LiteralPath $binary).Hash -ne $record.binarySha256) { throw 'Fixture binary changed during validation.' }
    $record.status = 'passed'
}
catch {
    $record.status = 'failed'
    $record.error = $_.Exception.Message
    throw
}
finally {
    if ($null -ne $gpuLock) { $gpuLock.Dispose() }
    $record | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $indexPath -Encoding utf8
}
Write-Output "Passed $($record.completed.Count) strict fixtures: $indexPath"
