[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. (Join-Path $root 'scripts/ArchitectureFrameProfiler.Common.ps1')

$scenario = [pscustomobject]@{
    id = 'empty-editor-dual'
    workload = 'empty'
    binaryKind = 'editor'
    activeViews = 'game+scene'
    expectedEditorEnabled = $true
}
$arguments = @{
    Scenario = $scenario
    Warmup = 1
    Samples = 2
    ExpectedBackend = 'vulkan'
    ExpectedBuild = 'Release'
    ExpectedLevel = 'basic'
    ExpectedValidation = $true
    ExpectedVisible = $false
    ExpectedFocusPolicy = 'default'
    ExpectedWidth = 1280
    ExpectedHeight = 800
}

function New-TestReport {
    param([scriptblock]$Mutate)

    $header = [ordered]@{
        type = 'header'
        format = 'PrismFrameProfiler'
        version = 1
        metadata = [ordered]@{
            backend = 'Vulkan'
            width = 1280
            height = 800
            validationRequested = $true
            editorEnabled = $true
            headless = $true
            windowFocusPolicy = 'default'
            identity = [ordered]@{
                identity = [ordered]@{
                    buildConfiguration = 'Release'
                    adapterName = 'Test GPU'
                    driverVersion = '1'
                    deviceId = '2'
                    vendorId = '3'
                    executableHash = 'test-hash'
                }
            }
        }
    }
    $rows = [Collections.Generic.List[object]]::new()
    $rows.Add($header)
    foreach ($frameNumber in 1..3) {
        $rows.Add([ordered]@{
            type = 'frame'
            frameId = $frameNumber
            actualLevel = 'basic'
            activeViewMask = 3
            editorLoopMs = 2.0 + $frameNumber
            gpuFrameMs = 1.0
            profilerOverheadMs = 0.001
            pipeline = [ordered]@{
                inputToPresentMs = 4.0
                queueWaitMs = 0.1
                waitingDepthAtAcceptance = 1
                peakWaitingDepth = 1
            }
            framePacing = [ordered]@{
                available = $true
                profile = 'interactive-smooth'
                requestedPresentation = 'synchronized'
                effectivePresentation = 'synchronized'
                nativePresentMode = 'FIFO'
                admissionSource = 'frame-queue-fallback'
                fallbackReason = ''
                targetFps = $null
                configuredMaxQueuedFrames = 2
                effectiveMaxQueuedFrames = 2
                swapchainImageCount = 3
                frameResourceSlotCount = 2
                syncInterval = 1
                presentFlags = 0
                requestedGeneration = 1
                effectiveGeneration = 1
                tearingSupported = $true
                tearingEnabled = $false
                transitionPending = $false
            }
            lanes = [ordered]@{
                main = [ordered]@{ activeMs = 1.0; waitMs = 0.1 }
                render = [ordered]@{ activeMs = 0.8 }
                worker = [ordered]@{ activeMs = $null }
            }
            views = [ordered]@{
                game = [ordered]@{
                    cpuRenderMs = 0.5; gpuTotalMs = 0.4; rendered = $true
                }
                scene = [ordered]@{
                    cpuRenderMs = 0.3; gpuTotalMs = 0.2; rendered = $true
                }
            }
            waits = [ordered]@{
                'display-admission' = 0.01; 'frame-limiter' = 0.0
                'frame-fence' = 0.01; reclaim = 0.01; acquire = 0.01
                'image-fence' = 0.01; upload = 0.01; prepare = 0.01
                submit = 0.01; 'native-present' = 0.01; signal = 0.01
                'queue-backpressure' = $null
            }
        })
    }
    $rows.Add([ordered]@{
        type = 'footer'; status = 'complete'; completedFrames = 3
    })
    if ($null -ne $Mutate) { & $Mutate $rows }

    $path = Join-Path $TestDrive 'frames.jsonl'
    $lines = @($rows | ForEach-Object { $_ | ConvertTo-Json -Depth 12 -Compress })
    [IO.File]::WriteAllLines($path, $lines, [Text.UTF8Encoding]::new($false))
    return $path
}

function Assert-Rejected {
    param([string]$Name, [scriptblock]$Mutate)

    $path = New-TestReport $Mutate
    try {
        Get-ArchitectureScenarioRunSummary -Path $path @arguments | Out-Null
        throw "Negative case '$Name' was accepted."
    } catch {
        if ($_.Exception.Message -eq "Negative case '$Name' was accepted.") {
            throw
        }
    }
}

$TestDrive = Join-Path ([IO.Path]::GetTempPath()) (
    'PrismArchitectureFrameProfilerTests-' + [guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($TestDrive) | Out-Null
try {
    $valid = Get-ArchitectureScenarioRunSummary -Path (New-TestReport $null) @arguments
    if ($valid.actualLoopFps.median -le 0.0 -or
        $valid.actualLoopFps.average -le 0.0 -or
        $valid.views.sceneRenderedFrames -ne 2) {
        throw 'Valid report did not produce the expected summary.'
    }

    Assert-Rejected backend { param($r) $r[0].metadata.backend = 'Direct3D 12' }
    Assert-Rejected build { param($r) $r[0].metadata.identity.identity.buildConfiguration = 'Debug' }
    Assert-Rejected validation { param($r) $r[0].metadata.validationRequested = $false }
    Assert-Rejected width { param($r) $r[0].metadata.width = 1920 }
    Assert-Rejected height { param($r) $r[0].metadata.height = 1080 }
    Assert-Rejected level { param($r) $r[2].actualLevel = 'detailed' }
    Assert-Rejected editor { param($r) $r[0].metadata.editorEnabled = $false }
    Assert-Rejected views { param($r) $r[2].activeViewMask = 1 }
    Assert-Rejected pacing { param($r) $r[2].framePacing.profile = 'benchmark' }
    Assert-Rejected footer { param($r) $r[-1].completedFrames = 2 }
    Assert-Rejected ordering { param($r) $r[2].frameId = 1 }
    Write-Output 'Architecture frame-profiler report tests passed.'
} finally {
    Remove-Item -LiteralPath $TestDrive -Recurse -Force -ErrorAction SilentlyContinue
}
