# Reusable PowerShell 5.1 API. Dot-source for interactive development or use
# Invoke-PrismScript.ps1. The server reads snapshots; scripts run outside ETS2.
Set-StrictMode -Version Latest

function Send-Prism {
    param([Parameter(Mandatory=$true)][string]$Command)
    $bytes = [Text.Encoding]::UTF8.GetBytes($Command)
    if ($bytes.Length -gt 2047) { throw 'Command exceeds 2047 UTF-8 bytes.' }
    $pipe = [IO.Pipes.NamedPipeClientStream]::new('.', "PrismMediaDiagnostic-$PrismProcessId",
        [IO.Pipes.PipeDirection]::InOut, [IO.Pipes.PipeOptions]::Asynchronous)
    try {
        $pipe.Connect(3000)
        $pipe.ReadMode = [IO.Pipes.PipeTransmissionMode]::Message
        $pipe.Write($bytes, 0, $bytes.Length)
        $stream = [IO.MemoryStream]::new()
        try {
            $buffer = New-Object byte[] 8192
            do {
                $pending = $pipe.BeginRead($buffer, 0, $buffer.Length, $null, $null)
                try {
                    if (-not $pending.AsyncWaitHandle.WaitOne(15000)) { throw 'Prism command response timed out.' }
                    $received = $pipe.EndRead($pending)
                } finally { $pending.AsyncWaitHandle.Close() }
                if ($received -eq 0) { throw 'Prism disconnected before completing the response.' }
                $stream.Write($buffer, 0, $received)
                if ($stream.Length -gt 65536) { throw 'Response exceeds protocol limit.' }
            } until ($pipe.IsMessageComplete)
            $response = [Text.Encoding]::UTF8.GetString($stream.ToArray())
            $ack = [Text.Encoding]::ASCII.GetBytes('ack')
            $pipe.Write($ack, 0, $ack.Length)
        } finally { $stream.Dispose() }
        if ($response.StartsWith('ERROR')) { throw $response }
        if ($PrismOutputDirectory) {
            $line = '{0:o} {1}' -f [DateTime]::Now, $Command
            [IO.File]::AppendAllText((Join-Path $PrismOutputDirectory 'commands.txt'), $line + "`r`n")
        }
        return $response
    } finally { $pipe.Dispose() }
}

function Update-PrismLease {
    Send-Prism 'lease renew 120' | Out-Null
}

function Wait-Prism {
    param([ValidateRange(0,300)][double]$Seconds = 1)
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $renewAt = 0.0
    while ($watch.Elapsed.TotalSeconds -lt $Seconds) {
        if ($watch.Elapsed.TotalSeconds -ge $renewAt) {
            Update-PrismLease
            $renewAt = $watch.Elapsed.TotalSeconds + 10
        }
        Start-Sleep -Milliseconds 100
    }
}

function Read-PrismObservation {
    param([string]$Prompt = 'Describe custom media and smartphone GPS')
    Write-Host "$Prompt (Enter to continue, Escape to abort):"
    if ([Console]::IsInputRedirected) { throw 'This comparison requires an interactive console.' }
    $text = ''
    $nextRenew = [DateTime]::MinValue
    while ($true) {
        if ([DateTime]::UtcNow -ge $nextRenew) {
            Update-PrismLease
            $nextRenew = [DateTime]::UtcNow.AddSeconds(10)
        }
        if ([Console]::KeyAvailable) {
            $key = [Console]::ReadKey($true)
            if ($key.Key -eq [ConsoleKey]::Escape) { throw 'User aborted comparison.' }
            if ($key.Key -eq [ConsoleKey]::Enter) { Write-Host ''; return $text }
            if ($key.Key -eq [ConsoleKey]::Backspace) {
                if ($text.Length -gt 0) { $text = $text.Substring(0,$text.Length-1); [Console]::Write("`b `b") }
            } elseif (-not [char]::IsControl($key.KeyChar)) {
                $text += $key.KeyChar; [Console]::Write($key.KeyChar)
            }
        }
        Start-Sleep -Milliseconds 50
    }
}

function Get-PrismCapture {
    param(
        [Parameter(Mandatory=$true)][ValidatePattern('^[A-Za-z0-9_-]{1,64}$')][string]$Name,
        [ValidateRange(1,30)][int]$Seconds = 5,
        [string]$Options = 'slot=6 every=7 limit=512 budget=30000'
    )
    Update-PrismLease
    Send-Prism "capture start $Name $Seconds $Options" | Out-Null
    $watch = [Diagnostics.Stopwatch]::StartNew()
    do {
        Wait-Prism 0.25
        $status = Send-Prism 'capture status' | ConvertFrom-Json
        if ($watch.Elapsed.TotalSeconds -gt ($Seconds + 5)) { throw 'Capture did not finish within its deadline.' }
    } while ($status.active)
    $rows = @()
    for ($i = 0; $i -lt $status.rows; ++$i) {
        if (($i % 32) -eq 0) { Update-PrismLease }
        $rows += (Send-Prism "capture row $i" | ConvertFrom-Json)
    }
    $result = [pscustomobject]@{ status=$status; options=$Options; rows=$rows }
    $path = Join-Path $PrismOutputDirectory "$Name.json"
    $result | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath $path -Encoding UTF8
    Write-Host "$Name : $($status.rows) distinct states, $($status.matched) matching draws; reason=$($status.reason), overflow=$($status.overflow)"
    if ($status.reason -eq 'capture-error' -or $status.reason -eq 'hook-error') { throw "Capture failed: $($status.reason)" }
    return $result
}

function Compare-PrismCaptures {
    param([Parameter(Mandatory=$true)]$Before, [Parameter(Mandatory=$true)]$After)
    # Geometry identity is only a candidate correspondence, not proof of a
    # screen. Keep all alternatives; never label the first changed draw as GPS.
    $fields = @('vs','ps','gs','hs','ds','layout','texture','targets','depthTarget','blend','depth','raster',
        'sampleMask','stencilRef','blendFactor','blendDesc','depthDesc','rasterDesc','viewports','scissors','psSrvs','psSamplers','psConstantBuffers')
    $groups = @{}
    foreach ($r in $After.rows) {
        $key = "$($r.context)|$($r.kind)|$($r.ib)|$($r.vb0)|$($r.stride)|$($r.vbOffset)|$($r.ibOffset)|$($r.count)|$($r.start)|$($r.base)|$($r.instances)|$($r.firstInstance)"
        if (-not $groups.ContainsKey($key)) { $groups[$key] = @() }
        $values = @{}
        foreach ($field in $fields) { $values[$field] = $r.$field | ConvertTo-Json -Depth 12 -Compress }
        $groups[$key] += [pscustomobject]@{ row=$r; values=$values }
    }
    $reported = 0
    foreach ($r in $Before.rows) {
        if ($reported -ge 2048) {
            [pscustomobject]@{ result='comparison-truncated'; reason='2048 candidate limit; narrow filters and rerun' }
            return
        }
        $key = "$($r.context)|$($r.kind)|$($r.ib)|$($r.vb0)|$($r.stride)|$($r.vbOffset)|$($r.ibOffset)|$($r.count)|$($r.start)|$($r.base)|$($r.instances)|$($r.firstInstance)"
        if (-not $groups.ContainsKey($key)) {
            ++$reported
            [pscustomobject]@{ geometry=$key; result='not-observed-after'; before=$r; after=$null }
            continue
        }
        $values = @{}
        foreach ($field in $fields) { $values[$field] = $r.$field | ConvertTo-Json -Depth 12 -Compress }
        if ($groups[$key].Count -gt 32) {
            ++$reported
            [pscustomobject]@{ geometry=$key; result='ambiguous-geometry'; alternatives=$groups[$key].Count }
            continue
        }
        foreach ($candidate in $groups[$key]) {
            $s = $candidate.row
            $changes = @()
            foreach ($field in $fields) {
                if ($values[$field] -cne $candidate.values[$field]) { $changes += $field }
            }
            if ($changes.Count) {
                ++$reported
                [pscustomobject]@{ geometry=$key; result='candidate-change'; fields=$changes; before=$r; after=$s }
            }
        }
    }
}
