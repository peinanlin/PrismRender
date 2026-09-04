[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$InputDirectory,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [switch]$Enforce
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'ArchitectureValidation.Common.ps1')
. (Join-Path $PSScriptRoot 'ArchitecturePerformance.Common.ps1')
. (Join-Path $PSScriptRoot 'ArchitecturePerformanceReport.Common.ps1')
$inputPath = [IO.Path]::GetFullPath($InputDirectory, $root)
$output = Resolve-ArchitectureOutputPath $root $OutputDirectory
if (Test-Path -LiteralPath $output) { throw 'Refusing to overwrite an audit report.' }
if ($output.StartsWith($inputPath + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Write the audit outside the immutable baseline directory.'
}
$report = Read-ArchitecturePerformanceBaseline $inputPath
$report['reportDriverSha256'] = (Get-FileHash $PSCommandPath).Hash
$report['reportHelperSha256'] = (Get-FileHash (Join-Path $PSScriptRoot 'ArchitecturePerformanceReport.Common.ps1')).Hash
$report['validationHelperSha256'] = (Get-FileHash (Join-Path $PSScriptRoot 'ArchitecturePerformance.Common.ps1')).Hash
New-Item -ItemType Directory -Path $output | Out-Null
$report | ConvertTo-Json -Depth 20 | Set-Content (Join-Path $output 'report.json') -Encoding utf8
Write-Output (Join-Path $output 'report.json')
if ($Enforce -and -not $report.passed) { throw 'Baseline is incomplete or exceeds V3; the diagnostic report is retained.' }
