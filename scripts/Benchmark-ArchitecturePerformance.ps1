[CmdletBinding()]
param(
    [ValidateSet('d3d12','vulkan')][string[]]$Backends = @('d3d12','vulkan'),
    [string[]]$Scenes = @('preview','shadows','hpwater-ocean','pbf'),
    [ValidateSet('native','serial')][string]$QueueMode = 'native',
    [ValidateSet('game','scene')][string]$View = 'game',
    [ValidateRange(60,10000)][int]$WarmupFrames = 60,
    [ValidateRange(180,10000)][int]$SampleFrames = 180,
    [switch]$VisibleWindow,
    [switch]$FocusedWindow,
    [switch]$UnfocusedWindow,
    [ValidateRange(-32768,32767)][int]$WindowX = 100,
    [ValidateRange(-32768,32767)][int]$WindowY = 100,
    [string]$BinaryPath = 'build-windows-ci/RelWithDebInfo/PrismRender.exe',
    [Parameter(Mandatory)][string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'ArchitectureValidation.Common.ps1')
. (Join-Path $PSScriptRoot 'ArchitecturePerformance.Common.ps1')
if ($FocusedWindow -and $UnfocusedWindow) { throw 'FocusedWindow and UnfocusedWindow are mutually exclusive.' }
if (($FocusedWindow -or $UnfocusedWindow) -and -not $VisibleWindow) { throw 'Explicit focus policy requires VisibleWindow.' }
if (-not $Backends.Count -or -not $Scenes.Count -or @($Backends | Select-Object -Unique).Count -ne $Backends.Count -or
    @($Scenes | Select-Object -Unique).Count -ne $Scenes.Count) { throw 'Benchmark cases must be nonempty and unique.' }
$catalog = (Get-Content (Join-Path $PSScriptRoot 'ArchitectureDemoCases.json') -Raw | ConvertFrom-Json).cases.scene
foreach ($scene in $Scenes) { if ($scene -notin $catalog) { throw "Unknown benchmark scene: $scene" } }
$output = Resolve-ArchitectureOutputPath $root $OutputDirectory
if (Test-Path $output) { throw "Refusing to overwrite benchmark: $output" }
New-Item -ItemType Directory -Path $output | Out-Null
$index = [ordered]@{ format = 'PrismArchitecturePerformanceBaseline'; version = 1; status = 'running'
    driverSha256 = (Get-FileHash $PSCommandPath).Hash
    backends = $Backends; scenes = $Scenes; warmupRunsPerBatch = 3; measurementRunsPerBatch = 5; batches = 2
    warmupFramesPerRun = $WarmupFrames; sampleFramesPerRun = $SampleFrames; drainFramesPerRun = 8
    runs = @(); comparisons = @(); limitations = @('Only listed cases; not complete P0/control/visual acceptance.',
        'Batch repeatability uses V3 5% median and 10% p95 limits in both directions; no automatic threshold relaxation.') }
$references = @{}; $batchSummaries = @{}
try {
    for ($batch = 1; $batch -le 2; ++$batch) {
        foreach ($backend in $Backends) { foreach ($scene in $Scenes) {
            $case = "$backend-$scene"
            $summaries = @()
            for ($run = 1; $run -le 8; ++$run) {
                $role = if ($run -le 3) { 'warmup' } else { 'measurement' }
                $path = Join-Path $output "$case-b$batch-r$run"
                Write-Output "Performance: $case batch $batch run $run/8 ($role)"
                & (Join-Path $PSScriptRoot 'Measure-ArchitecturePerformance.ps1') -Backend $backend -Scene $scene -QueueMode $QueueMode -View $View -BinaryPath $BinaryPath -WarmupFrames $WarmupFrames -SampleFrames $SampleFrames -VisibleWindow:$VisibleWindow -FocusedWindow:$FocusedWindow -UnfocusedWindow:$UnfocusedWindow -WindowX $WindowX -WindowY $WindowY -OutputDirectory $path
                $sample = Read-ArchitecturePerformanceRun $path
                if ($references.ContainsKey($case)) {
                    Assert-ArchitecturePerformanceCompatibility $references[$case] $sample
                    if ($sample.index.binarySha256 -ne $references[$case].index.binarySha256 -or $sample.sourceHash -ne $references[$case].sourceHash) {
                        throw 'Baseline source/binary changed between independent runs.'
                    }
                    if ($sample.index.driverSha256 -ne $references[$case].index.driverSha256 -or $sample.index.helperSha256 -ne $references[$case].index.helperSha256) {
                        throw 'Performance measurement/validation tools changed within a baseline.'
                    }
                } else { $references[$case] = $sample }
                $index.runs += @{ case = $case; batch = $batch; run = $run; role = $role; path = $path
                    indexSha256 = (Get-FileHash (Join-Path $path 'index.json')).Hash }
                if ($role -eq 'measurement') { $summaries += $sample.summary }
                $index | ConvertTo-Json -Depth 12 | Set-Content (Join-Path $output 'index.json') -Encoding utf8
            }
            $batchSummary = Get-ArchitecturePerformanceBatchSummary $summaries
            $batchSummary | ConvertTo-Json -Depth 12 | Set-Content (Join-Path $output "$case-b$batch-summary.json") -Encoding utf8
            if ($batch -eq 1) { $batchSummaries[$case] = $batchSummary }
            else {
                $forward = Compare-ArchitecturePerformanceDistributions $batchSummaries[$case] $batchSummary
                $reverse = Compare-ArchitecturePerformanceDistributions $batchSummary $batchSummaries[$case]
                $index.comparisons += @{ case = $case; passed = $forward.passed -and $reverse.passed; batch1To2 = $forward; batch2To1 = $reverse }
            }
        } }
    }
    if (@($index.comparisons | Where-Object { -not $_.passed }).Count) { throw 'Two-batch baseline repeatability exceeded V3 thresholds; retain samples and investigate.' }
    $index.status = 'baseline-repeatability-passed'
} catch { $index.status = 'failed'; $index['error'] = $_.Exception.Message; throw }
finally {
    $index['artifacts'] = @(Get-ChildItem $output -File | Where-Object Name -ne 'index.json' | ForEach-Object {
        @{ path = $_.Name; sha256 = (Get-FileHash $_.FullName).Hash }
    })
    $index | ConvertTo-Json -Depth 12 | Set-Content (Join-Path $output 'index.json') -Encoding utf8
}
Write-Output (Join-Path $output 'index.json')
