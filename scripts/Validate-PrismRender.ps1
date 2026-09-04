[CmdletBinding()]
param(
    [string]$BuildDirectory = "build-windows-ci",
    [ValidateSet("Debug", "Release", "RelWithDebInfo")]
    [string]$Configuration = "Debug",
    [switch]$IncludeGpu,
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot
$buildPath = Join-Path $projectRoot $BuildDirectory

if (-not (Test-Path $buildPath -PathType Container)) {
    throw "Build directory does not exist: $buildPath"
}

$cmake = Get-Command cmake -ErrorAction SilentlyContinue
$ctest = Get-Command ctest -ErrorAction SilentlyContinue
if ($null -eq $cmake -or $null -eq $ctest) {
    throw "Run this script from a Visual Studio Developer PowerShell with cmake and ctest on PATH."
}

if (-not $SkipBuild) {
    $buildCommand = @(
        "set NORMALIZED_PATH=!PATH!",
        'set "PATH="',
        'set "path="',
        'set "PATH=!NORMALIZED_PATH!"',
        "`"$($cmake.Source)`" --build `"$buildPath`" --config $Configuration --parallel 8"
    ) -join " & "
    & cmd.exe /d /v:on /c $buildCommand
    if ($LASTEXITCODE -ne 0) {
        throw "PrismRender build failed."
    }
}

$ctestArguments = @(
    "--test-dir", $buildPath,
    "-C", $Configuration,
    "--output-on-failure"
)
if (-not $IncludeGpu) {
    $ctestArguments += @("-LE", "gpu")
}
& $ctest.Source @ctestArguments
if ($LASTEXITCODE -ne 0) {
    throw "PrismRender tests failed."
}

$mcpServer = Join-Path $buildPath "$Configuration\PrismMcpServer.exe"
if (Test-Path $mcpServer -PathType Leaf) {
    $messages = @(
        '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"validation"}}',
        '{"jsonrpc":"2.0","id":2,"method":"tools/list"}',
        '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"prism.execute","arguments":{"requestId":"validation-status","command":"world.status","arguments":{}}}}'
    ) -join "`n"
    $responses = @(
        $messages |
            & $mcpServer --project-root $projectRoot |
            ForEach-Object { $_ | ConvertFrom-Json }
    )
    if ($responses.Count -ne 3 -or
        $responses[1].result.tools.Count -ne 2 -or
        -not $responses[2].result.structuredContent.success) {
        throw "PrismMcpServer validation failed."
    }
}

if ($IncludeGpu) {
    $harness = Join-Path $buildPath "$Configuration\PrismHarness.exe"
    if (Test-Path $harness -PathType Leaf) {
        $configurationTag = $Configuration.ToLowerInvariant()
        $streamingOutput = Join-Path $projectRoot (
            "automation\reports\validation-asset-streaming-$configurationTag.jsonl"
        )
        & $harness `
            --headless `
            --project-root $projectRoot `
            --commands "examples\harness\asset_streaming_runtime_validation.jsonl" `
            --output $streamingOutput
        if ($LASTEXITCODE -ne 0) {
            throw "Asset Streaming D3D12/Vulkan runtime validation process failed."
        }
        $streamingResult = Get-Content $streamingOutput -Raw | ConvertFrom-Json
        if (-not $streamingResult.success -or
            -not $streamingResult.data.passed -or
            -not $streamingResult.data.d3d12.validation.passed -or
            -not $streamingResult.data.vulkan.validation.passed -or
            -not $streamingResult.data.d3d12.buildIdentity.valid -or
            -not $streamingResult.data.vulkan.buildIdentity.valid -or
            -not $streamingResult.data.d3d12.buildIdentity.pdbMatchesExecutable -or
            -not $streamingResult.data.vulkan.buildIdentity.pdbMatchesExecutable -or
            $streamingResult.data.d3d12.performanceIdentity.identity.buildConfiguration -ne $Configuration -or
            $streamingResult.data.vulkan.performanceIdentity.identity.buildConfiguration -ne $Configuration -or
            -not $streamingResult.data.crossApiReportsMatched -or
            -not $streamingResult.data.comparison.passed) {
            throw "Asset Streaming D3D12/Vulkan runtime validation failed."
        }
        Write-Host (
            "Asset Streaming cross-API validation passed: " +
            "MAE={0:N8}, RMSE={1:N8}, resident={2}, configuration={3}" -f
            $streamingResult.data.comparison.metrics.meanAbsoluteError,
            $streamingResult.data.comparison.metrics.rootMeanSquareError,
            $streamingResult.data.d3d12.validation.residentCount,
            $Configuration
        )
    }
}

Write-Host "PrismRender validation passed: $BuildDirectory / $Configuration"
