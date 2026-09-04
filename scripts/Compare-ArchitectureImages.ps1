[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Reference,
    [Parameter(Mandatory)][string]$Candidate,
    [Parameter(Mandatory)][ValidateSet('d3d12', 'vulkan')][string]$ReferenceBackend,
    [Parameter(Mandatory)][ValidateSet('d3d12', 'vulkan')][string]$CandidateBackend,
    [string]$CompareBinaryPath = 'build-windows-ci/RelWithDebInfo/PrismGoldenImageCompare.exe',
    [Parameter(Mandatory)][string]$OutputDirectory,
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'ArchitectureValidation.Common.ps1')
$output = Resolve-ArchitectureOutputPath $projectRoot $OutputDirectory
if (Test-Path -LiteralPath $output) { throw "Refusing to overwrite comparison evidence: $output" }
$binary = [IO.Path]::GetFullPath($CompareBinaryPath, $projectRoot)
$referencePath = [IO.Path]::GetFullPath($Reference, $projectRoot)
$candidatePath = [IO.Path]::GetFullPath($Candidate, $projectRoot)
foreach ($file in @($binary, $referencePath, $candidatePath)) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing comparison input: $file" }
}
# A same-backend run cannot select the looser cross-backend thresholds.
$kind = if ($ReferenceBackend -eq $CandidateBackend) { 'same-backend' } else { 'cross-backend' }
$arguments = @($referencePath, $candidatePath, "--report=$(Join-Path $output 'metrics.txt')") +
    @(Get-ArchitectureImageThresholds $kind)
$record = [ordered]@{ format = 'PrismArchitectureImageComparison'; version = 1; kind = $kind
    reference = $referencePath; referenceBackend = $ReferenceBackend; candidate = $candidatePath
    candidateBackend = $CandidateBackend; binary = $binary; arguments = $arguments; status = 'planned' }
if ($DryRun) { $record | ConvertTo-Json -Depth 4; return }
New-Item -ItemType Directory -Path $output -ErrorAction Stop | Out-Null
try {
    Invoke-ArchitectureProcess -Binary $binary -Arguments $arguments -WorkingDirectory $output -OutputBase (Join-Path $output 'comparison')
    $record.status = 'passed'
}
catch { $record.status = 'failed'; $record['error'] = $_.Exception.Message; throw }
finally {
    $record['referenceSha256'] = (Get-FileHash -LiteralPath $referencePath).Hash
    $record['candidateSha256'] = (Get-FileHash -LiteralPath $candidatePath).Hash
    $record | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $output 'index.json') -Encoding utf8
}
Write-Output (Join-Path $output 'index.json')
