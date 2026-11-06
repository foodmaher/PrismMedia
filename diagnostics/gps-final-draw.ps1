# RenderDoc-confirmed calibration for ETS2 addon GPS displays. The plugin
# swaps Prism media only for one selected final draw; every unmatched display
# keeps the game's original SRV and native processing.
Write-Host 'Keep exactly one Prism custom display enabled and keep all GPS screens visible.'
Read-PrismObservation 'Return to the cockpit, then press Enter to start final-draw discovery' | Out-Null

# Clear a previous saved selection so this script always performs a visible
# comparison instead of immediately restoring the old route.
Send-Prism 'route clear' | Out-Null
Send-Prism 'route retrain' | Out-Null
$candidateText = ''
$candidateCount = 0
for ($attempt = 0; $attempt -lt 12 -and $candidateCount -eq 0; ++$attempt) {
    Wait-Prism 1
    $candidateText = Send-Prism 'route candidates'
    $match = [regex]::Match($candidateText, '^OK candidates=(\d+)')
    if ($match.Success) { $candidateCount = [int]$match.Groups[1].Value }
}
$candidateText | Set-Content (Join-Path $PrismOutputDirectory 'final-draw-candidates.txt')
if ($candidateCount -eq 0) {
    throw 'No stable 1024x1024 slot-6 GPS draw was found. Keep the screens visible and rerun.'
}

Write-Host $candidateText
$observations = @()
for ($index = 0; $index -lt $candidateCount; ++$index) {
    Send-Prism "route preview $index 10" | Out-Null
    $note = Read-PrismObservation "Candidate ${index}: which one screen shows Prism media? Describe left/right/dashboard"
    $status = Send-Prism 'route status'
    Send-Prism 'route preview off' | Out-Null
    $observations += [pscustomobject]@{
        candidate = $index
        observation = $note
        status = $status
    }
}
$observations | ConvertTo-Json -Depth 5 |
    Set-Content (Join-Path $PrismOutputDirectory 'final-draw-observations.json') -Encoding UTF8

do {
    $selectionText = Read-PrismObservation "Enter the candidate number for the configured Prism display (0..$($candidateCount - 1))"
    $selection = -1
    $valid = [int]::TryParse($selectionText, [ref]$selection) -and
        $selection -ge 0 -and $selection -lt $candidateCount
    if (-not $valid) { Write-Host 'Invalid candidate number; try again.' -ForegroundColor Yellow }
} until ($valid)

Write-Host (Send-Prism "route select $selection")
Wait-Prism 2
$finalStatus = Send-Prism 'route status'
$finalStatus | Set-Content (Join-Path $PrismOutputDirectory 'final-route-status.txt')
Write-Host $finalStatus
Write-Host 'Calibration complete. The selected display should show media while unmatched GPS screens remain native.'
