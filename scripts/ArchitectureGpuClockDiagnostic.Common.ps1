Set-StrictMode -Version Latest

function ConvertFrom-ArchitectureNvidiaSmiCsv {
    param([Parameter(Mandatory)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Missing NVIDIA telemetry: $Path" }
    $headers = @('timestamp','gpuIndex','name','pstate','graphicsClockMHz','smClockMHz','memoryClockMHz',
        'gpuUtilizationPercent','powerWatts','powerLimitWatts','temperatureCelsius')
    $lines = @(Get-Content -LiteralPath $Path | Where-Object { $_.Trim() })
    $rows = @($lines | ConvertFrom-Csv -Header $headers)
    if (-not $rows.Count) { throw 'NVIDIA telemetry contains no samples.' }
    $validRows = [Collections.Generic.List[object]]::new()
    for ($rowIndex = 0; $rowIndex -lt $rows.Count; ++$rowIndex) {
        $row = $rows[$rowIndex]
        $incomplete = @($headers | Where-Object { $null -eq $row.$_ -or -not $row.$_.Trim() }).Count -ne 0
        if ($incomplete -and $rowIndex -eq $rows.Count - 1) { continue }
        if ($incomplete) { throw "Incomplete NVIDIA telemetry row $($rowIndex + 1)." }
        if ($row.pstate -notmatch '^P[0-9]+$') { throw "Invalid NVIDIA pstate: $($row.pstate)" }
        foreach ($field in @('gpuIndex','graphicsClockMHz','smClockMHz','memoryClockMHz','gpuUtilizationPercent',
                'powerWatts','powerLimitWatts','temperatureCelsius')) {
            $value = 0.0
            if (-not [double]::TryParse($row.$field, [Globalization.NumberStyles]::Float,
                    [Globalization.CultureInfo]::InvariantCulture, [ref]$value) -or -not [double]::IsFinite($value)) {
                throw "Invalid NVIDIA telemetry field $field."
            }
            $row.$field = $value
        }
        $validRows.Add($row)
    }
    if (-not $validRows.Count) { throw 'NVIDIA telemetry contains no complete samples.' }
    return $validRows.ToArray()
}

function Get-ArchitectureNumericDistribution {
    param([Parameter(Mandatory)][object[]]$Values)
    $sorted = @($Values | ForEach-Object { [double]$_ } | Sort-Object)
    if (-not $sorted.Count) { throw 'Cannot summarize an empty numeric distribution.' }
    function Get-NearestRank([double]$quantile) {
        $index = [Math]::Ceiling($quantile * $sorted.Count) - 1
        return $sorted[[Math]::Max(0, [Math]::Min($sorted.Count - 1, $index))]
    }
    return [ordered]@{ count = $sorted.Count; minimum = $sorted[0]; median = Get-NearestRank 0.5
        p95 = Get-NearestRank 0.95; maximum = $sorted[-1] }
}

function Get-ArchitectureNvidiaSmiSummary {
    param([Parameter(Mandatory)][object[]]$Rows)
    if (-not $Rows.Count) { throw 'Cannot summarize empty NVIDIA telemetry.' }
    $pstates = [ordered]@{}
    foreach ($group in @($Rows | Group-Object pstate | Sort-Object Name)) { $pstates[$group.Name] = $group.Count }
    return [ordered]@{
        samples = $Rows.Count
        gpuIndex = [int]$Rows[0].gpuIndex
        adapterName = $Rows[0].name.Trim()
        pstates = $pstates
        graphicsClockMHz = Get-ArchitectureNumericDistribution @($Rows.graphicsClockMHz)
        smClockMHz = Get-ArchitectureNumericDistribution @($Rows.smClockMHz)
        memoryClockMHz = Get-ArchitectureNumericDistribution @($Rows.memoryClockMHz)
        gpuUtilizationPercent = Get-ArchitectureNumericDistribution @($Rows.gpuUtilizationPercent)
        powerWatts = Get-ArchitectureNumericDistribution @($Rows.powerWatts)
        temperatureCelsius = Get-ArchitectureNumericDistribution @($Rows.temperatureCelsius)
    }
}

function ConvertTo-ArchitectureNvidiaTimestampUtc {
    param([Parameter(Mandatory)][string]$Value)
    $local = [DateTime]::ParseExact($Value.Trim(), 'yyyy/MM/dd HH:mm:ss.fff',
        [Globalization.CultureInfo]::InvariantCulture, [Globalization.DateTimeStyles]::None)
    return [TimeZoneInfo]::ConvertTimeToUtc($local)
}

function Get-ArchitecturePearsonCorrelation {
    param([Parameter(Mandatory)][object[]]$Rows, [Parameter(Mandatory)][string]$X,
        [Parameter(Mandatory)][string]$Y)
    if ($Rows.Count -lt 2) { return $null }
    $meanX = 0.0; $meanY = 0.0
    foreach ($row in $Rows) { $meanX += [double]$row.$X; $meanY += [double]$row.$Y }
    $meanX /= $Rows.Count; $meanY /= $Rows.Count
    $numerator = 0.0; $squareX = 0.0; $squareY = 0.0
    foreach ($row in $Rows) {
        $deltaX = [double]$row.$X - $meanX; $deltaY = [double]$row.$Y - $meanY
        $numerator += $deltaX * $deltaY; $squareX += $deltaX * $deltaX; $squareY += $deltaY * $deltaY
    }
    if ($squareX -eq 0 -or $squareY -eq 0) { return $null }
    return $numerator / [Math]::Sqrt($squareX * $squareY)
}
