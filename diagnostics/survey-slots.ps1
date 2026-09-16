# A wider test using the same host. Narrow this slot list or add resource,
# shader, index-buffer, draw-count or caller filters without rebuilding.
Write-Host 'Return to the cockpit. Slot survey begins in 8 seconds and keeps your current fallback mode.'
Wait-Prism 8
foreach ($slot in 0..15) {
    Get-PrismCapture -Name "slot-$slot" -Seconds 2 -Options "slot=$slot every=13 limit=256 budget=10000" | Out-Null
}
Send-Prism 'snapshot' | Out-Null
