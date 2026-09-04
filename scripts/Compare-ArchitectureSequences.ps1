[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ReferenceDirectory,
    [Parameter(Mandatory)][string]$CandidateDirectory,
    [Parameter(Mandatory)][string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'ArchitectureValidation.Common.ps1')
. (Join-Path $PSScriptRoot 'ArchitectureSequences.Common.ps1')
$reference = Read-ArchitectureSequenceRun $root $ReferenceDirectory
$candidate = Read-ArchitectureSequenceRun $root $CandidateDirectory
Assert-ArchitectureSequenceCompatibility $reference.run $candidate.run
$output = Resolve-ArchitectureOutputPath $root $OutputDirectory
if (Test-Path $output) { throw 'Refusing to overwrite sequence comparison.' }
New-Item -ItemType Directory -Path $output | Out-Null
$record = [ordered]@{ format = 'PrismArchitectureSequenceComparison'; version = 1; status = 'running'
    reference = $reference.directory; candidate = $candidate.directory
    referenceIndexSha256 = (Get-FileHash (Join-Path $reference.directory 'index.json')).Hash
    candidateIndexSha256 = (Get-FileHash (Join-Path $candidate.directory 'index.json')).Hash
    backend = $reference.run.backend; samples = @() }
try {
    foreach ($frame in $reference.run.frames) {
        $referenceBase = Join-Path $reference.directory "captures/frame-$frame"
        $candidateBase = Join-Path $candidate.directory "captures/frame-$frame"
        $sample = @{ frame = $frame; passed = $false }
        try {
            & (Join-Path $PSScriptRoot 'Compare-ArchitectureImages.ps1') -Reference "$referenceBase.bmp" -Candidate "$candidateBase.bmp" -ReferenceBackend $reference.run.backend -CandidateBackend $candidate.run.backend -OutputDirectory (Join-Path $output "frame-$frame-image") | Out-Null
            & (Join-Path $PSScriptRoot 'Compare-ArchitectureFrameDiagnostics.ps1') -Reference "$referenceBase.capture.json" -Candidate "$candidateBase.capture.json" -OutputDirectory (Join-Path $output "frame-$frame-structure") | Out-Null
            $sample.passed = $true
        } catch { $sample['error'] = $_.Exception.Message }
        $record.samples += $sample
    }
    if (@($record.samples | Where-Object passed -eq $false).Count) { throw 'One or more sequence samples failed the unchanged same-backend image/structure gates.' }
    $record.status = 'passed'
} catch { $record.status = 'failed'; $record['error'] = $_.Exception.Message; throw }
finally { $record | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $output 'index.json') -Encoding utf8 }
Write-Output (Join-Path $output 'index.json')
