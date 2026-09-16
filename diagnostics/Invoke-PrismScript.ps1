param(
    [Parameter(Mandatory=$true)][int]$ProcessId,
    [Parameter(Mandatory=$true)][string]$ScriptPath
)
$ErrorActionPreference = 'Stop'
$PrismProcessId = $ProcessId
$PrismOutputDirectory = Join-Path ([Environment]::GetFolderPath('MyDocuments')) (
    'PrismMedia-Diagnostics\{0}-{1}-{2}' -f (Get-Date -Format 'yyyyMMdd-HHmmss'), $ProcessId, ([guid]::NewGuid().ToString('N').Substring(0,6)))
New-Item -ItemType Directory -Path $PrismOutputDirectory -Force | Out-Null
. (Join-Path $PSScriptRoot 'PrismApi.ps1')
$leased = $false
$failed = $false
try {
    $capabilities = Send-Prism 'capabilities'
    if ($capabilities -notmatch 'liveDraw=1' -or $capabilities -notmatch 'fallbackLease=1') {
        throw 'The game loaded an older plugin. Install the extensible 4.0.0 build and restart the game once.'
    }
    $capabilities | Set-Content (Join-Path $PrismOutputDirectory 'capabilities.txt')
    Send-Prism 'lease begin 120' | Out-Null
    $leased = $true
    Write-Host "Results: $PrismOutputDirectory"
    # Reload from disk on every invocation. This is ordinary PowerShell with
    # loops, conditions, functions and the reusable Send-Prism API in scope.
    & (Resolve-Path -LiteralPath $ScriptPath).Path
} catch {
    $failed = $true
    $_ | Out-String | Set-Content (Join-Path $PrismOutputDirectory 'error.txt')
    Write-Host "Script stopped: $_" -ForegroundColor Red
} finally {
    if ($leased) {
        try { Send-Prism 'capture stop' | Out-Null } catch { Write-Warning $_ }
        try { Send-Prism 'lease end' | Out-Null } catch {
            Write-Warning 'Could not restore immediately. The 120-second lease restores fallback on the next telemetry update after expiry.'
        }
    }
    Write-Host "Results saved in $PrismOutputDirectory"
    try {
        Compress-Archive -Path (Join-Path $PrismOutputDirectory '*') -DestinationPath "$PrismOutputDirectory.zip"
        Write-Host "Send this ZIP with PrismMedia.log: $PrismOutputDirectory.zip"
    } catch { Write-Warning 'Could not create the ZIP; the result files are still in the session folder.' }
}
if ($failed) { exit 1 }
