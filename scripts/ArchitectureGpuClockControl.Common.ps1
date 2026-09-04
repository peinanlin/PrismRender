# CPU-only helpers for the opt-in NVIDIA clock-control benchmark wrapper.
function Get-ArchitectureSupportedGraphicsClocks {
    param([Parameter(Mandatory)][string]$NvidiaSmi, [ValidateRange(0,31)][int]$GpuIndex)
    $output = @(& $NvidiaSmi --id=$GpuIndex --query-supported-clocks=gr --format=csv,noheader,nounits 2>&1)
    if ($LASTEXITCODE -ne 0) { throw "Could not query supported NVIDIA graphics clocks: $($output -join ' ')" }
    $clocks = [Collections.Generic.List[int]]::new()
    foreach ($line in $output) {
        $value = 0
        if (-not [int]::TryParse(([string]$line).Trim(), [ref]$value) -or $value -le 0) {
            throw "Invalid supported NVIDIA graphics clock: $line"
        }
        $clocks.Add($value)
    }
    if (-not $clocks.Count) { throw 'NVIDIA reported no supported graphics clocks.' }
    return @($clocks | Sort-Object -Unique)
}

function Get-ArchitectureCurrentGraphicsClock {
    param([Parameter(Mandatory)][string]$NvidiaSmi, [ValidateRange(0,31)][int]$GpuIndex)
    $output = @(& $NvidiaSmi --id=$GpuIndex --query-gpu=clocks.current.graphics --format=csv,noheader,nounits 2>&1)
    if ($LASTEXITCODE -ne 0 -or $output.Count -ne 1) { throw "Could not query current NVIDIA graphics clock: $($output -join ' ')" }
    $value = 0
    if (-not [int]::TryParse(([string]$output[0]).Trim(), [ref]$value) -or $value -le 0) {
        throw "Invalid current NVIDIA graphics clock: $($output[0])"
    }
    return $value
}

function Test-ArchitectureGraphicsClockTarget {
    param(
        [ValidateRange(1,10000)][int]$CurrentMHz,
        [ValidateRange(1,10000)][int]$TargetMHz,
        [ValidateRange(0,200)][int]$ToleranceMHz = 20)
    return [Math]::Abs($CurrentMHz - $TargetMHz) -le $ToleranceMHz
}

function Get-ArchitectureGpuClockControlPlan {
    param([ValidateRange(0,31)][int]$GpuIndex, [ValidateRange(180,4000)][int]$GraphicsClockMHz)
    return [ordered]@{
        gpuIndex = $GpuIndex
        graphicsClockMHz = $GraphicsClockMHz
        lockArguments = @('-i', "$GpuIndex", '-lgc', "$GraphicsClockMHz,$GraphicsClockMHz")
        resetArguments = @('-i', "$GpuIndex", '-rgc')
        emergencyRecoveryCommand = "nvidia-smi -i $GpuIndex -rgc"
    }
}
