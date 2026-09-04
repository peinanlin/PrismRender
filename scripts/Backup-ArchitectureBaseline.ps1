[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidatePattern('^[a-zA-Z0-9][a-zA-Z0-9_-]{0,79}$')]
    [string]$BaselineId,
    [switch]$Verify
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$projectRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$baselineRoot = Join-Path $projectRoot "artifacts/architecture-refactor/$BaselineId"
$snapshotRoot = Join-Path $baselineRoot 'snapshot'
$manifestPath = Join-Path $baselineRoot 'snapshot-manifest.json'
$sealPath = Join-Path $baselineRoot 'snapshot-manifest.sha256'

function Assert-NoReparseAncestor([string]$Path) {
    $cursor = [IO.Path]::GetFullPath($Path)
    while ($cursor) {
        if (Test-Path -LiteralPath $cursor) {
            if ((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "Refusing linked snapshot path: $cursor"
            }
        }
        $cursor = [IO.Path]::GetDirectoryName($cursor)
    }
}

function Resolve-SnapshotFile([string]$RelativePath) {
    if ([IO.Path]::IsPathRooted($RelativePath) -or $RelativePath -match '(^|[\\/])\.\.([\\/]|$)') {
        throw "Unsafe manifest path: $RelativePath"
    }
    $resolved = [IO.Path]::GetFullPath((Join-Path $snapshotRoot $RelativePath))
    if (-not $resolved.StartsWith($snapshotRoot + [IO.Path]::DirectorySeparatorChar,
            [StringComparison]::OrdinalIgnoreCase)) { throw "Path escapes snapshot: $RelativePath" }
    Assert-NoReparseAncestor $resolved
    return $resolved
}

function Test-Snapshot {
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf) -or
        -not (Test-Path -LiteralPath $sealPath -PathType Leaf)) { throw 'Snapshot has no completed manifest/seal.' }
    if ((Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash -ne
        (Get-Content -LiteralPath $sealPath -Raw).Trim()) { throw 'Snapshot manifest hash mismatch.' }
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if ($manifest.format -ne 'PrismArchitectureSourceSnapshot' -or $manifest.version -ne 1 -or
        $manifest.baselineId -ne $BaselineId -or $manifest.files.Count -eq 0) { throw 'Invalid snapshot manifest.' }
    $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($entry in $manifest.files) {
        if (-not $seen.Add($entry.path)) { throw "Duplicate snapshot entry: $($entry.path)" }
        $copy = Resolve-SnapshotFile $entry.path
        if (-not (Test-Path -LiteralPath $copy -PathType Leaf) -or
            (Get-Item -LiteralPath $copy).Length -ne $entry.bytes -or
            (Get-FileHash -LiteralPath $copy -Algorithm SHA256).Hash -ne $entry.sha256) {
            throw "Snapshot verification failed: $($entry.path)"
        }
    }
    $copiedFiles = @(Get-ChildItem -LiteralPath $snapshotRoot -Recurse -File -Force)
    if ($copiedFiles.Count -ne $seen.Count) { throw 'Snapshot contains unlisted files.' }
    Write-Output "Verified $($seen.Count) files: $manifestPath"
}

Assert-NoReparseAncestor $baselineRoot
if ($Verify) { Test-Snapshot; return }
if (Test-Path -LiteralPath $baselineRoot) { throw "Baseline already exists; refusing overwrite: $baselineRoot" }

# Only project inputs are copied. Build/cache/capture outputs and Git metadata are never touched.
$inputRoots = @('src', 'assets', 'cmake', 'scripts', 'tests', 'tools', 'examples', 'docs',
    'openspec', '.github', 'third_party', 'automation/assets', 'automation/shaders',
    'automation/tests', 'automation/snapshots')
$rootFiles = @('AGENTS.md', '.gitignore', 'CMakeLists.txt', 'CMakePresets.json', 'imgui.ini')
function Get-InputFiles {
    foreach ($inputRoot in $inputRoots) {
        $path = Join-Path $projectRoot $inputRoot
        if (-not (Test-Path -LiteralPath $path -PathType Container)) { throw "Missing input root: $path" }
        Assert-NoReparseAncestor $path
        foreach ($item in Get-ChildItem -LiteralPath $path -Recurse -Force) {
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Linked input: $($item.FullName)" }
            if (-not $item.PSIsContainer) { $item }
        }
    }
    foreach ($rootFile in $rootFiles) {
        $path = Join-Path $projectRoot $rootFile
        if (Test-Path -LiteralPath $path -PathType Leaf) {
            Assert-NoReparseAncestor $path
            Get-Item -LiteralPath $path -Force
        }
    }
}
$inputs = @(Get-InputFiles | Sort-Object FullName)
$entries = [Collections.Generic.List[object]]::new()
New-Item -ItemType Directory -Path $snapshotRoot | Out-Null
foreach ($inputFile in $inputs) {
    $relative = [IO.Path]::GetRelativePath($projectRoot, $inputFile.FullName).Replace('\', '/')
    $target = Resolve-SnapshotFile $relative
    $hash = (Get-FileHash -LiteralPath $inputFile.FullName -Algorithm SHA256).Hash
    New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($target)) -Force | Out-Null
    Copy-Item -LiteralPath $inputFile.FullName -Destination $target -ErrorAction Stop
    if ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne $hash) {
        throw "Input changed during backup: $relative"
    }
    $entries.Add([ordered]@{ path = $relative; bytes = $inputFile.Length; sha256 = $hash })
}
# Detect concurrent writes/deletions/additions before certifying a coherent source baseline.
$after = @(Get-InputFiles | Sort-Object FullName)
if (@(Compare-Object @($inputs.FullName) @($after.FullName)).Count -ne 0) { throw 'Input file set changed during backup.' }
foreach ($entry in $entries) {
    if ((Get-FileHash -LiteralPath (Join-Path $projectRoot $entry.path) -Algorithm SHA256).Hash -ne $entry.sha256) {
        throw "Source changed before baseline was sealed: $($entry.path)"
    }
}
[ordered]@{
    format = 'PrismArchitectureSourceSnapshot'; version = 1; baselineId = $BaselineId
    createdUtc = [DateTime]::UtcNow.ToString('o'); sourceRoot = $projectRoot
    inputRoots = $inputRoots; rootFiles = $rootFiles
    excluded = @('.git', '.codex', '.agents', '.vs', 'build-*', 'artifacts',
        'automation/cache', 'automation/captures', 'automation/performance', 'automation/reports', 'automation/*.jsonl', 'towork', 'skills')
    files = @($entries.ToArray())
} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $manifestPath -Encoding utf8
(Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash | Set-Content -LiteralPath $sealPath -Encoding ascii
Test-Snapshot
