# Shared by the architecture-only driver and its CPU tests. Does not change process state on import.
function Resolve-ArchitectureOutputPath([string]$ProjectRoot, [string]$Path) {
    $allowedRoot = [IO.Path]::GetFullPath((Join-Path $ProjectRoot 'artifacts/architecture-refactor'))
    $resolved = [IO.Path]::GetFullPath($Path, $ProjectRoot)
    if ($resolved -ne $allowedRoot -and -not $resolved.StartsWith(
            $allowedRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Architecture evidence must remain under $allowedRoot"
    }
    if ([IO.Path]::GetRelativePath($allowedRoot, $resolved) -match '(^|[\\/])snapshot([\\/]|$)') {
        throw 'Evidence output cannot be inside an immutable source snapshot.'
    }
    $cursor = $resolved
    while ($cursor) {
        if ((Test-Path -LiteralPath $cursor) -and
            ((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "Linked evidence path is not allowed: $cursor"
        }
        $cursor = [IO.Path]::GetDirectoryName($cursor)
    }
    return $resolved
}

function Get-ArchitectureImageThresholds([ValidateSet('same-backend', 'cross-backend')][string]$Comparison) {
    if ($Comparison -eq 'same-backend') {
        return @('--mean=0.00392156862745098', '--rmse=0.00784313725490196',
            '--changed=0.001', '--ssim=0.999', '--pixel-tolerance=8', '--enforce')
    }
    return @('--mean=0.03', '--rmse=0.10', '--changed=0.25', '--ssim=0.90', '--pixel-tolerance=8', '--enforce')
}

function Assert-ArchitectureValidationLog([string]$Text, [ValidateSet('d3d12', 'vulkan')][string]$Backend) {
    if ($Text -match '\[Vulkan validation\]|D3D12 validation (?!820\])') { throw 'Unexpected GPU validation diagnostic.' }
    $enabled = if ($Backend -eq 'vulkan') { 'Vulkan Khronos validation layer enabled\.' } else { 'D3D12 debug layer enabled\.' }
    if ($Text -notmatch $enabled) { throw "Requested $Backend validation was not actually enabled." }
}

function Invoke-ArchitectureProcess {
    param(
        [Parameter(Mandatory)][string]$Binary,
        [string[]]$Arguments = @(),
        [Parameter(Mandatory)][string]$WorkingDirectory,
        [Parameter(Mandatory)][string]$OutputBase,
        [hashtable]$Environment = @{},
        [ValidateRange(1, 3600)][int]$TimeoutSeconds = 180
    )
    if (-not (Test-Path -LiteralPath $Binary -PathType Leaf)) { throw "Missing binary: $Binary" }
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $Binary
    $info.WorkingDirectory = $WorkingDirectory
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    foreach ($argument in $Arguments) { $info.ArgumentList.Add($argument) }
    # Preserve PATH/SDK/driver environment, but never inherit an unrelated capture or quality override.
    foreach ($key in @($info.Environment.Keys)) {
        if ($key -like 'PRISM_RENDER_*') { [void]$info.Environment.Remove($key) }
    }
    foreach ($key in $Environment.Keys) { $info.Environment[$key] = [string]$Environment[$key] }
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $info
    $started = $false
    try {
        $started = $process.Start()
        if (-not $started) { throw "Could not start $Binary" }
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            $process.Kill($true)
            $process.WaitForExit()
            throw "Child timed out after ${TimeoutSeconds}s: $Binary"
        }
        if ($process.ExitCode -ne 0) { throw "Child failed with exit $($process.ExitCode): $Binary" }
    }
    finally {
        if ($started) {
            $stdout.GetAwaiter().GetResult() | Set-Content -LiteralPath "$OutputBase.stdout.log" -Encoding utf8
            $stderr.GetAwaiter().GetResult() | Set-Content -LiteralPath "$OutputBase.stderr.log" -Encoding utf8
        }
        $process.Dispose()
    }
}
