[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ReferenceMatrix,
    [Parameter(Mandatory)][string]$CandidateMatrix,
    [Parameter(Mandatory)][string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'ArchitectureValidation.Common.ps1')

function Read-Matrix([string]$Path) {
    $directory = [IO.Path]::GetFullPath($Path, $root)
    $indexPath = Join-Path $directory 'index.json'
    if (-not (Test-Path -LiteralPath $indexPath -PathType Leaf)) {
        throw "Missing Demo sequence matrix: $indexPath"
    }
    $index = Get-Content -LiteralPath $indexPath -Raw | ConvertFrom-Json
    if ($index.format -ne 'PrismArchitectureDemoSequenceMatrix' -or
        $index.version -ne 1 -or $index.status -ne 'passed' -or
        @($index.runs).Count -ne 40 -or
        @($index.comparisons).Count -ne 20) {
        throw "Demo sequence matrix is incomplete: $indexPath"
    }
    return [pscustomobject]@{
        directory = $directory
        indexPath = $indexPath
        index = $index
    }
}

$reference = Read-Matrix $ReferenceMatrix
$candidate = Read-Matrix $CandidateMatrix
if ($reference.index.backend -ne $candidate.index.backend) {
    throw 'Frozen/default matrix comparison must use the same backend.'
}
$referenceRuns = @($reference.index.runs | Where-Object repeat -eq 1)
$candidateRuns = @($candidate.index.runs | Where-Object repeat -eq 1)
$referenceScenes = @($referenceRuns.scene | Sort-Object)
$candidateScenes = @($candidateRuns.scene | Sort-Object)
$sceneDifferences = @(Compare-Object -ReferenceObject $referenceScenes -DifferenceObject $candidateScenes)
if ($referenceRuns.Count -ne 20 -or $candidateRuns.Count -ne 20 -or
    $sceneDifferences.Count -ne 0) {
    throw 'Frozen/default matrices do not cover the same 20 Demo scenes.'
}

$output = Resolve-ArchitectureOutputPath $root $OutputDirectory
if (Test-Path -LiteralPath $output) {
    throw "Refusing to overwrite matrix comparison: $output"
}
[IO.Directory]::CreateDirectory($output) | Out-Null
$summary = [ordered]@{
    format = 'PrismArchitectureDemoSequenceMatrixComparison'
    version = 1
    status = 'running'
    backend = $reference.index.backend
    reference = $reference.directory
    referenceIndexSha256 = (Get-FileHash $reference.indexPath).Hash
    candidate = $candidate.directory
    candidateIndexSha256 = (Get-FileHash $candidate.indexPath).Hash
    comparisons = @()
}
try {
    foreach ($referenceRun in $referenceRuns) {
        $candidateRun = @($candidateRuns | Where-Object {
                $_.scene -eq $referenceRun.scene
            })
        if ($candidateRun.Count -ne 1) {
            throw "Candidate run is ambiguous for $($referenceRun.scene)."
        }
        $comparison = Join-Path $output $referenceRun.scene
        $comparisonArguments = @{
            ReferenceDirectory = $referenceRun.directory
            CandidateDirectory = $candidateRun[0].directory
            OutputDirectory = $comparison
        }
        $comparisonScript = Join-Path $PSScriptRoot 'Compare-ArchitectureSequences.ps1'
        & $comparisonScript @comparisonArguments | Out-Null
        $comparisonIndex = Join-Path $comparison 'index.json'
        $summary.comparisons += [ordered]@{
            scene = $referenceRun.scene
            directory = $comparison
            indexSha256 = (Get-FileHash $comparisonIndex).Hash
        }
    }
    $summary.status = 'passed'
} catch {
    $summary.status = 'failed'
    $summary['error'] = $_.Exception.Message
    throw
} finally {
    [IO.File]::WriteAllText(
        (Join-Path $output 'index.json'),
        ($summary | ConvertTo-Json -Depth 8),
        [Text.UTF8Encoding]::new($false))
}
Write-Output (Join-Path $output 'index.json')
