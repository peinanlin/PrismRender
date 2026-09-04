[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ReferenceRunDirectory,
    [Parameter(Mandatory)][string]$CandidateRunDirectory,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [switch]$UseApprovedRevisions,
    [string]$RevisionCatalogPath = 'scripts/ArchitectureVisualBaselineRevisions.json'
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$projectRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'ArchitectureValidation.Common.ps1')
. (Join-Path $PSScriptRoot 'ArchitectureVisualBaselines.Common.ps1')
$referenceDirectory = Resolve-ArchitectureOutputPath $projectRoot $ReferenceRunDirectory
$candidateDirectory = Resolve-ArchitectureOutputPath $projectRoot $CandidateRunDirectory
$output = Resolve-ArchitectureOutputPath $projectRoot $OutputDirectory
if (Test-Path -LiteralPath $output) { throw "Refusing to overwrite comparison: $output" }
$referenceRun = Get-Content -LiteralPath (Join-Path $referenceDirectory 'index.json') -Raw | ConvertFrom-Json
$candidateRun = Get-Content -LiteralPath (Join-Path $candidateDirectory 'index.json') -Raw | ConvertFrom-Json
if ($referenceRun.backend -ne $candidateRun.backend) { throw 'Demo regression comparison requires the same backend.' }
$revisions = @()
if ($UseApprovedRevisions) {
    $revisions = @(Read-ArchitectureVisualRevisions $projectRoot ([IO.Path]::GetFullPath($RevisionCatalogPath, $projectRoot)))
}
$results = [Collections.Generic.List[object]]::new()
$record = [ordered]@{ format = 'PrismArchitectureDemoComparison'; version = 1; status = 'running'
    referenceRun = $referenceDirectory; candidateRun = $candidateDirectory
    useApprovedRevisions = $UseApprovedRevisions.IsPresent; kind = 'same-backend'; results = @()
    limitations = @('Visual comparison only; strict validation, temporal controls and performance are separate gates.',
        'Legacy reference runs may lack Editor capability metadata; their actual view count and configuration are still checked.') }
New-Item -ItemType Directory -Path $output -ErrorAction Stop | Out-Null
try {
    foreach ($command in $candidateRun.commands) {
        $item = [ordered]@{ name = $command.name; status = 'failed'; revision = $null }
        try {
            $candidate = Get-ArchitectureDemoSample $candidateDirectory $candidateRun $command
            $selected = @($revisions | Where-Object { Test-ArchitectureBaselineScope $candidate.scope $_.scope })
            if ($selected.Count -gt 1) { throw 'Ambiguous baseline revision.' }
            if ($selected.Count -eq 1) {
                $referenceImage = $selected[0].image
                $item.revision = $selected[0].revision
                $item['previousImage'] = $selected[0].previousImage
            } else {
                $name = $command.name -replace '-r[0-9]+$', '-r1'
                $matches = @($referenceRun.commands | Where-Object name -eq $name)
                if ($matches.Count -ne 1) { throw "No unambiguous original reference for $name" }
                $reference = Get-ArchitectureDemoSample $referenceDirectory $referenceRun $matches[0]
                $expected = $reference.scope | Select-Object * -ExcludeProperty editorEnabled
                if (-not (Test-ArchitectureBaselineScope $candidate.scope $expected) -or
                    ($null -ne $reference.scope.editorEnabled -and $candidate.scope.editorEnabled -ne $reference.scope.editorEnabled)) {
                    throw "Reference input differs from candidate input: $name"
                }
                $referenceImage = $reference.image
                $hash = @($referenceRun.artifacts | Where-Object name -eq "$name.bmp")
                if ($hash.Count -ne 1) { throw 'Missing reference artifact seal.' }
                Assert-ArchitectureFileHash $referenceImage $hash[0].sha256
            }
            $hash = @($candidateRun.artifacts | Where-Object name -eq "$($command.name).bmp")
            if ($hash.Count -ne 1) { throw 'Missing candidate artifact seal.' }
            Assert-ArchitectureFileHash $candidate.image $hash[0].sha256
            $item['referenceImage'] = $referenceImage
            $item['candidateImage'] = $candidate.image
            & (Join-Path $PSScriptRoot 'Compare-ArchitectureImages.ps1') -Reference $referenceImage -Candidate $candidate.image `
                -ReferenceBackend $candidateRun.backend -CandidateBackend $candidateRun.backend `
                -OutputDirectory (Join-Path $output $command.name) | Out-Null
            $item.status = 'passed'
        } catch { $item['error'] = $_.Exception.Message }
        $results.Add($item)
        Write-Host "$($item.status): $($item.name)"
    }
    if ($results.Count -eq 0 -or @($results | Where-Object status -ne 'passed').Count) { throw 'One or more Demo comparisons failed.' }
    $record.status = 'passed'
} catch { $record.status = 'failed'; $record['error'] = $_.Exception.Message; throw }
finally {
    $record.results = @($results.ToArray())
    if ($UseApprovedRevisions) { $record['revisionCatalogSha256'] = (Get-FileHash -LiteralPath ([IO.Path]::GetFullPath($RevisionCatalogPath, $projectRoot))).Hash }
    $record | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $output 'index.json') -Encoding utf8
}
Write-Output (Join-Path $output 'index.json')
