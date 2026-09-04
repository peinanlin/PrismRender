[CmdletBinding()]
param(
    [ValidateSet('d3d12','vulkan')][string[]]$Apis = @('d3d12','vulkan'),
    [string]$BinaryPath = 'build-windows-ci/RelWithDebInfo/PrismRender.exe',
    [Parameter(Mandatory)][string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. (Join-Path $root 'scripts/ArchitectureValidation.Common.ps1')
$output = Resolve-ArchitectureOutputPath $root $OutputDirectory
if (Test-Path $output) { throw 'Refusing to overwrite integration evidence.' }
New-Item -ItemType Directory -Path $output | Out-Null
$record = [ordered]@{ format='PrismCaptureSequenceIntegration'; version=1; status='running'; cases=@() }
$lock = $null
try {
    $lock = [IO.File]::Open((Join-Path $root 'artifacts/architecture-refactor/gpu-validation.lock'),
        [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    foreach ($api in $Apis) {
        $folder = Join-Path $output "$api-early-exit"
        $plan = & (Join-Path $root 'scripts/Capture-ArchitectureSequence.ps1') -Backend $api -Scene preview -Frames 3,4,5 -EndFrame 7 -BinaryPath $BinaryPath -OutputDirectory $folder -DryRun | ConvertFrom-Json
        New-Item -ItemType Directory -Path $folder | Out-Null
        Copy-Item (Join-Path $root 'artifacts/architecture-refactor/20260828-hpwater-complete/snapshot/imgui.ini') (Join-Path $folder 'imgui.ini')
        @{ version=1; frames=@(3,4,5) } | ConvertTo-Json | Set-Content (Join-Path $folder 'sequence-input.json') -Encoding utf8NoBOM
        $environment = @{}
        foreach ($property in $plan.environment.PSObject.Properties) { $environment[$property.Name]=$property.Value }
        # Bypass the driver's input check to exercise the real application's
        # incomplete-sequence exit, including two already-successful captures.
        $environment.PRISM_RENDER_MAX_FRAMES = '4'
        $environment | ConvertTo-Json | Set-Content (Join-Path $folder 'inputs.json')
        $failed = $false
        try {
            Invoke-ArchitectureProcess -Binary $plan.binary -Arguments $plan.arguments -Environment $environment -WorkingDirectory $folder -OutputBase (Join-Path $folder 'process') -TimeoutSeconds 120
        } catch { $failed = $true }
        $stderr = Get-Content (Join-Path $folder 'process.stderr.log') -Raw
        Assert-ArchitectureValidationLog $stderr $api
        if (-not $failed -or $stderr -notmatch 'before all capture sequence samples completed' -or
            @(Get-Content (Join-Path $folder 'captures/frames.jsonl')).Count -ne 4 -or
            @(Get-ChildItem (Join-Path $folder 'captures') -Filter '*.bmp').Count -ne 2 -or
            @(Get-ChildItem (Join-Path $folder 'captures') -Filter '*.capture.json').Count -ne 2 -or
            (Test-Path (Join-Path $folder 'captures/frame-5.capture.json'))) {
            throw 'Early exit did not reject the incomplete sequence correctly.'
        }
        $record.cases += @{ api=$api; binary=$plan.binary; binarySha256=$plan.binarySha256; passed=$true; expectedChildFailure=$true }
        Write-Host "PASS $api incomplete sequence rejected"
    }
    $record.status='passed'
} catch { $record.status='failed'; $record['error']=$_.Exception.Message; throw }
finally {
    if ($lock) { $lock.Dispose() }
    $record['artifacts']=@(Get-ChildItem $output -Recurse -File | ForEach-Object { @{ path=[IO.Path]::GetRelativePath($output,$_.FullName); sha256=(Get-FileHash $_.FullName).Hash } })
    $record | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $output 'index.json') -Encoding utf8
}
Write-Output (Join-Path $output 'index.json')
