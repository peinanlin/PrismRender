[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. (Join-Path $root 'scripts/ArchitectureValidation.Common.ps1')
. (Join-Path $root 'scripts/ArchitectureVisualBaselines.Common.ps1')
$output = Join-Path $root ('artifacts/architecture-refactor/tool-tests/baselines-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $output -ErrorAction Stop | Out-Null
$results = [Collections.Generic.List[string]]::new()
function Check([string]$Name, [scriptblock]$Action) { & $Action; $results.Add($Name); Write-Host "PASS $Name" }
function Expect-Failure([scriptblock]$Action) {
    $failed = $false
    try { & $Action | Out-Null } catch { $failed = $true }
    if (-not $failed) { throw 'Expected rejection did not occur.' }
}
try {
    $catalogPath = Join-Path $root 'scripts/ArchitectureVisualBaselineRevisions.json'
    $revisions = @(Read-ArchitectureVisualRevisions $root $catalogPath)
    Check 'Exactly three approved scopes, nine images and retained old failures verified' {
        if ($revisions.Count -ne 3 -or ($revisions.scope | Where-Object scene -eq 'pbf').Count -ne 2) { throw 'Wrong approval set.' }
    }
    Check 'Exact approved scope matches' {
        if (-not (Test-ArchitectureBaselineScope $revisions[0].scope $revisions[0].scope)) { throw 'Exact scope rejected.' }
    }
    Check 'Wrong API, scene, view, frame, extent, queue, active views or build cannot reuse approval' {
        foreach ($field in @('backend', 'scene', 'view', 'frame', 'completedFrames', 'width', 'height', 'queueMode', 'activeViews', 'configuration', 'editorEnabled', 'deterministic')) {
            $changed = $revisions[0].scope | ConvertTo-Json | ConvertFrom-Json
            $changed.$field = 'mismatch'
            if (Test-ArchitectureBaselineScope $changed $revisions[0].scope) { throw "Scope leak: $field" }
        }
    }
    Check 'Modified image hash rejected without touching baseline' {
        Expect-Failure { Assert-ArchitectureFileHash $revisions[0].image ('0' * 64) }
    }
    Check 'Modified catalog provenance rejected' {
        $invalid = Get-Content $catalogPath -Raw | ConvertFrom-Json
        $invalid.sourceSnapshotSha256 = '0' * 64
        $invalid | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $output 'invalid-catalog.json')
        Expect-Failure { Read-ArchitectureVisualRevisions $root (Join-Path $output 'invalid-catalog.json') }
    }
    Check 'Additional quality override rejected' {
        $directory = Split-Path -Parent $revisions[0].image
        $run = Get-Content (Join-Path $directory 'index.json') -Raw | ConvertFrom-Json
        $run.commands[0].environment | Add-Member -NotePropertyName PRISM_RENDER_WATER_QUALITY -NotePropertyValue low
        Expect-Failure { Get-ArchitectureDemoSample $directory $run $run.commands[0] }
    }
    Check 'Added diagnostics require their own sealed evidence' {
        $directory = Split-Path -Parent $revisions[0].image
        $run = Get-Content (Join-Path $directory 'index.json') -Raw | ConvertFrom-Json
        $run.commands[0].environment | Add-Member -NotePropertyName PRISM_RENDER_CAPTURE_DIAGNOSTICS_PATH -NotePropertyValue 'missing.capture.json'
        Expect-Failure { Get-ArchitectureDemoSample $directory $run $run.commands[0] }
    }
    Check 'Failed run cannot supply a reference' {
        $directory = Split-Path -Parent $revisions[0].image
        $run = Get-Content (Join-Path $directory 'index.json') -Raw | ConvertFrom-Json
        $run.status = 'failed'
        Expect-Failure { Get-ArchitectureDemoSample $directory $run $run.commands[0] }
    }
    Check 'Tampered capture metadata seal rejected' {
        $directory = Split-Path -Parent $revisions[0].image
        $run = Get-Content (Join-Path $directory 'index.json') -Raw | ConvertFrom-Json
        ($run.artifacts | Where-Object name -eq 'pbf-game-f30-r1.gpu.json').sha256 = '0' * 64
        Expect-Failure { Get-ArchitectureDemoSample $directory $run $run.commands[0] }
    }
    Check 'Duplicate approval rejected' {
        $invalid = Get-Content $catalogPath -Raw | ConvertFrom-Json
        $invalid.cases += $invalid.cases[0]
        $path = Join-Path $output 'duplicate-catalog.json'
        $invalid | ConvertTo-Json -Depth 8 | Set-Content $path
        Expect-Failure { Read-ArchitectureVisualRevisions $root $path }
    }
    Check 'Unknown legacy Editor capability does not match approval' {
        $directory = Split-Path -Parent $revisions[0].image
        $run = Get-Content (Join-Path $directory 'index.json') -Raw | ConvertFrom-Json
        $run.binarySha256 = '0' * 64
        $sample = Get-ArchitectureDemoSample $directory $run $run.commands[0]
        if ($null -ne $sample.scope.editorEnabled -or (Test-ArchitectureBaselineScope $sample.scope $revisions[0].scope)) {
            throw 'Unknown build capability reused approval.'
        }
    }
} finally {
    @{ count = $results.Count; passed = @($results.ToArray()) } | ConvertTo-Json -Depth 4 |
        Set-Content -LiteralPath (Join-Path $output 'tests.json') -Encoding utf8
}
Write-Output "Passed $($results.Count) tests. Evidence: $output"
