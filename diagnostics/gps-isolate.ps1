# Follow-up for gps-ab.ps1. This derives the configured custom screen's draw
# geometry, then asks what resource occupies the same geometry with the global
# fallback off and on. Edit/rerun without rebuilding PrismMedia.dll.
Write-Host 'Keep the same truck, accessories, seat and camera used for gps-ab.ps1.'
Read-PrismObservation 'Return to the cockpit with custom media and the extra smartphone visible; press Enter' | Out-Null

$displayText = Send-Prism 'displays'
$displayText | Set-Content (Join-Path $PrismOutputDirectory 'displays.txt')
$match = [regex]::Match($displayText, 'enabled=1 live=1 resource=(0x[0-9A-Fa-f]+)')
if (-not $match.Success) { throw 'No enabled live custom display resource was reported.' }
$customResource = $match.Groups[1].Value

Send-Prism 'fallback on' | Out-Null
Send-Prism 'mark isolate-anchor-on' | Out-Null
Write-Host 'Fallback ON: deriving the configured custom screen geometry in 6 seconds.'
Wait-Prism 6
$custom = Get-PrismCapture -Name 'isolate-anchor-on' -Seconds 4 `
    -Options "slot=6 resource=$customResource every=1 limit=256 budget=100000"
if (-not $custom.rows -or $custom.rows.Count -eq 0) {
    throw 'The configured custom resource was not drawn. Keep the screen visible and rerun.'
}

# Multiple passes/states may draw one surface. Group on stable mesh/draw
# arguments and select the group with the largest summed hit count. This is a
# candidate mapping and the result file keeps every alternative.
$groups = @{}
foreach ($row in $custom.rows) {
    $key = "$($row.kind)|$($row.ib)|$($row.vb0)|$($row.stride)|$($row.vbOffset)|$($row.ibOffset)|$($row.ibFormat)|$($row.topology)|$($row.count)|$($row.start)|$($row.base)|$($row.instances)|$($row.firstInstance)"
    if (-not $groups.ContainsKey($key)) { $groups[$key] = @() }
    $groups[$key] += $row
}
$ranked = foreach ($key in $groups.Keys) {
    [pscustomobject]@{
        key = $key
        rows = @($groups[$key])
        hits = [int64](($groups[$key] | Measure-Object -Property hits -Sum).Sum)
    }
}
$selected = $ranked | Sort-Object hits -Descending | Select-Object -First 1
$anchor = $selected.rows[0]
$ranked | Select-Object key,hits,@{n='rowCount';e={$_.rows.Count}} |
    ConvertTo-Json -Depth 5 | Set-Content (Join-Path $PrismOutputDirectory 'geometry-candidates.json') -Encoding UTF8
$anchor | ConvertTo-Json -Depth 16 | Set-Content (Join-Path $PrismOutputDirectory 'selected-anchor.json') -Encoding UTF8

function Select-AnchorGeometry {
    param($Capture)
    return @($Capture.rows | Where-Object {
        $_.kind -ceq $anchor.kind -and $_.ib -ceq $anchor.ib -and $_.vb0 -ceq $anchor.vb0 -and
        $_.stride -eq $anchor.stride -and $_.vbOffset -eq $anchor.vbOffset -and
        $_.ibOffset -eq $anchor.ibOffset -and $_.ibFormat -eq $anchor.ibFormat -and
        $_.topology -eq $anchor.topology -and $_.count -eq $anchor.count -and
        $_.start -eq $anchor.start -and $_.base -eq $anchor.base -and
        $_.instances -eq $anchor.instances -and $_.firstInstance -eq $anchor.firstInstance
    })
}

$results = @()
foreach ($phase in @('off','on-repeat')) {
    $mode = if ($phase -eq 'off') { 'off' } else { 'on' }
    Send-Prism "fallback $mode" | Out-Null
    Send-Prism "mark isolate-$phase" | Out-Null
    Write-Host "Fallback ${mode}: keep the same view; targeted capture begins in 6 seconds."
    Wait-Prism 6
    # Filtering by the anchor's IB and draw count is supported by the existing
    # host. Exact start/base/vertex geometry is selected after export.
    $pool = Get-PrismCapture -Name "isolate-$phase-pool" -Seconds 5 `
        -Options "slot=6 ib=$($anchor.ib) count=$($anchor.count) every=1 limit=512 budget=100000"
    $target = Select-AnchorGeometry $pool
    $target | ConvertTo-Json -Depth 16 |
        Set-Content (Join-Path $PrismOutputDirectory "isolate-$phase-target.json") -Encoding UTF8
    $resources = @($target | Group-Object { "$($_.texture.resource)|$($_.texture.width)x$($_.texture.height)|fmt=$($_.texture.format)|viewfmt=$($_.texture.viewFormat)" } |
        Sort-Object Count -Descending | ForEach-Object { [pscustomobject]@{ resource=$_.Name; states=$_.Count } })
    $note = Read-PrismObservation "Fallback ${mode}: describe CUSTOM MEDIA, PRIMARY GPS and EXTRA SMARTPHONE separately"
    $results += [pscustomobject]@{
        phase=$phase; fallback=$mode; targetStates=$target.Count
        resources=$resources; observation=$note; captureStatus=$pool.status
    }
    $results | ConvertTo-Json -Depth 10 |
        Set-Content (Join-Path $PrismOutputDirectory 'isolation-summary.json') -Encoding UTF8
}

Write-Host "Anchor: $($anchor.kind), count=$($anchor.count), start=$($anchor.start), base=$($anchor.base), ib=$($anchor.ib)"
Write-Host 'Isolation complete. The runner will restore the fallback mode you started with and make a ZIP.'
