# Strict catalog validation; import ArchitectureSequences.Common.ps1 first.
function Read-ArchitectureDemoSequenceCatalog([string]$ProjectRoot, [string]$Path) {
    $catalog = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json -AsHashtable
    if ($catalog.Count -ne 5 -or $catalog.format -cne 'PrismArchitectureDemoSequences' -or
        $catalog.version -ne 1 -or -not $catalog.policy -or $catalog.cases.Count -ne 20) {
        throw 'Invalid Demo sequence catalog envelope.'
    }
    $source = Get-Content (Join-Path $ProjectRoot 'src/Scene/DemoSceneCatalog.cpp') -Raw
    $keys = @([regex]::Matches($source, '\{DemoSceneId::\w+,\s*"([^"]+)"') | ForEach-Object { $_.Groups[1].Value })
    $scenes = @($catalog.cases.scene)
    if ($keys.Count -ne 20 -or @($scenes | Select-Object -Unique).Count -ne 20 -or
        @(Compare-Object ($keys | Sort-Object) ($scenes | Sort-Object)).Count) {
        throw 'Demo sequence catalog does not cover the exact DemoSceneCatalog.'
    }
    $allowedTags = @('fixed-input','temporal','ocean','fluid','taa-control','shadow-control',
        'streaming-control','dual-view-control','water-lifecycle')
    foreach ($case in $catalog.cases) {
        if ($case.Count -ne 9 -or $case.view -cnotin @('game','scene') -or $case.assetStreaming -isnot [bool] -or
            $case.waterLifecycle -isnot [bool] -or -not $case.applicability -or $case.endFrame -ne $case.frames[-1] -or
            @($case.tags | Select-Object -Unique).Count -ne $case.tags.Count -or @($case.tags | Where-Object { $_ -cnotin $allowedTags }).Count) {
            throw "Invalid Demo sequence case: $($case.scene)"
        }
        Assert-ArchitectureSequenceFrames $case.frames
        Assert-ArchitectureSequenceActions @($case.actions) $case.frames
    }
    $expectedCoverage = @{
        ocean = @('hpwater-ocean','ocean','waveworks-ocean')
        fluid = @('fluid-caustics','fluid-render','fluid-toon','pbf')
        'taa-control' = @('post-process')
        'shadow-control' = @('shadows')
        'streaming-control' = @('streaming')
        'dual-view-control' = @('waveworks-ocean')
    }
    foreach ($name in $expectedCoverage.Keys) {
        $declared = @($catalog.coverage[$name] | Sort-Object)
        $tagged = @($catalog.cases | Where-Object { $_.tags -contains $name } | ForEach-Object scene | Sort-Object)
        if (($declared -join ',') -cne (($expectedCoverage[$name] | Sort-Object) -join ',') -or
            ($tagged -join ',') -cne ($declared -join ',')) { throw "Invalid required sequence coverage: $name" }
    }
    $taa = @($catalog.cases | Where-Object { $_.scene -eq 'post-process' })[0]
    $shadows = @($catalog.cases | Where-Object { $_.scene -eq 'shadows' })[0]
    $streaming = @($catalog.cases | Where-Object { $_.scene -eq 'streaming' })[0]
    $dual = @($catalog.cases | Where-Object { $_.scene -eq 'waveworks-ocean' })[0]
    if ((@($taa.actions.type) -join ',') -cne 'set-taa,set-taa' -or
        (@($shadows.actions.type) -join ',') -cne 'set-shadows,set-shadows' -or
        (@($streaming.actions.type) -join ',') -cne 'activate-streaming' -or -not $streaming.assetStreaming -or
        $dual.view -cne 'scene' -or @($dual.actions | Where-Object type -eq 'refresh-scene').Count -ne $dual.frames.Count) {
        throw 'Required Demo sequence controls are incomplete.'
    }
    return $catalog
}
