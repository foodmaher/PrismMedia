# Edit this file and rerun "script gps-ab.ps1"; no compilation is needed.
# Keep the same truck, viewpoint and accessories for this sequence.
Write-Host 'Park the truck with both screens visible. These tests do not request a reload.'
Read-PrismObservation 'Return to the truck, then confirm you are ready' | Out-Null
Send-Prism 'snapshot' | Out-Null
Send-Prism 'displays' | Set-Content (Join-Path $PrismOutputDirectory 'displays.txt')
$captures = @{}
$observations = @()

# Return to the initial test state to separate persistent changes from
# on/off differences. Each phase gets a broad survey plus the previously
# observed game draw caller. If the game changed, broad surveys still run.
foreach ($phase in @('on-first','off','on-repeat')) {
    $mode = if ($phase -eq 'off') { 'off' } else { 'on' }
    Send-Prism "fallback $mode" | Out-Null
    Send-Prism "mark $phase" | Out-Null
    Write-Host "Fallback $mode. Return to the same cockpit view; capture starts in 8 seconds."
    Wait-Prism 8
    $captures[$phase] = Get-PrismCapture -Name $phase -Seconds 5 -Options 'slot=6 every=7 limit=512 budget=30000'
    Get-PrismCapture -Name "$phase-known-caller" -Seconds 3 -Options 'slot=6 caller=0x2A62BF every=1 limit=256 budget=15000' | Out-Null
    # Also isolate each configured live custom texture. This does not assume
    # the separate native phone uses that texture or shares its material.
    $displayLines = (Send-Prism 'displays') -split "`n"
    $ordinal = 0
    foreach ($line in $displayLines) {
        if ($line -match 'enabled=1 live=1 resource=(0x[0-9A-Fa-f]+)') {
            $resource = $Matches[1]
            ++$ordinal
            if ($ordinal -gt 8) { break }
            Get-PrismCapture -Name "$phase-custom-$ordinal" -Seconds 2 `
                -Options "slot=6 resource=$resource every=1 limit=128 budget=30000" | Out-Null
        }
    }
    $note = Read-PrismObservation 'What do you see? Describe CUSTOM MEDIA and SMARTPHONE GPS separately'
    $observations += [pscustomobject]@{ phase=$phase; fallback=$mode; observation=$note; time=(Get-Date -Format o) }
    $observations | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $PrismOutputDirectory 'observations.json') -Encoding UTF8
}
Write-Host 'Comparing candidate draws. Missing rows can reflect sampling; they are not proof that a screen was skipped.'
Update-PrismLease
@(Compare-PrismCaptures $captures['on-first'] $captures['off']) |
    ConvertTo-Json -Depth 20 | Set-Content (Join-Path $PrismOutputDirectory 'on-vs-off.json') -Encoding UTF8
Update-PrismLease
@(Compare-PrismCaptures $captures['on-first'] $captures['on-repeat']) |
    ConvertTo-Json -Depth 20 | Set-Content (Join-Path $PrismOutputDirectory 'repeat-control.json') -Encoding UTF8
Write-Host 'Comparison complete. The runner will restore the fallback mode you started with.'
