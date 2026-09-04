[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Reference,
    [Parameter(Mandatory)][string]$Candidate,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [string]$CompareBinaryPath = 'build-windows-ci/RelWithDebInfo/PrismFrameDiagnosticsCompare.exe'
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'ArchitectureValidation.Common.ps1')
$output = Resolve-ArchitectureOutputPath $projectRoot $OutputDirectory
if (Test-Path -LiteralPath $output) { throw "Refusing to overwrite diagnostics evidence: $output" }
$binary = [IO.Path]::GetFullPath($CompareBinaryPath, $projectRoot)
$referencePath = [IO.Path]::GetFullPath($Reference, $projectRoot)
$candidatePath = [IO.Path]::GetFullPath($Candidate, $projectRoot)
foreach ($path in @($binary, $referencePath, $candidatePath)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing diagnostics comparison input: $path" }
}
New-Item -ItemType Directory -Path $output -ErrorAction Stop | Out-Null
$record = [ordered]@{ format = 'PrismArchitectureFrameComparison'; version = 1; status = 'running'
    reference = $referencePath; referenceSha256 = (Get-FileHash -LiteralPath $referencePath).Hash
    candidate = $candidatePath; candidateSha256 = (Get-FileHash -LiteralPath $candidatePath).Hash
    binarySha256 = (Get-FileHash -LiteralPath $binary).Hash
    limitation = 'Structural diagnostics only; image thresholds and driver validation remain independent gates.' }
try {
    Invoke-ArchitectureProcess -Binary $binary -Arguments @($referencePath, $candidatePath, (Join-Path $output 'comparison.json')) -WorkingDirectory $output -OutputBase (Join-Path $output 'comparison')
    $record.status = 'passed'
} catch {
    $record.status = 'failed'
    $record['error'] = $_.Exception.Message
    throw
} finally {
    $record | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $output 'index.json') -Encoding utf8
}
Write-Output (Join-Path $output 'index.json')
