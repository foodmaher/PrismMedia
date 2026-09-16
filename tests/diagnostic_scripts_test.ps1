$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
foreach ($file in Get-ChildItem (Join-Path $root 'diagnostics') -Filter '*.ps1') {
    $tokens = $null; $errors = $null
    [Management.Automation.Language.Parser]::ParseFile($file.FullName,[ref]$tokens,[ref]$errors) | Out-Null
    if ($errors.Count) { throw ($errors | Out-String) }
}
$PrismProcessId = Get-Random -Minimum 100000 -Maximum 900000
$PrismOutputDirectory = $null
. (Join-Path $root 'diagnostics\PrismApi.ps1')
$job = Start-Job -ArgumentList $PrismProcessId -ScriptBlock {
    param($Target)
    $pipe = [IO.Pipes.NamedPipeServerStream]::new("PrismMediaDiagnostic-$Target",
        [IO.Pipes.PipeDirection]::InOut,1,[IO.Pipes.PipeTransmissionMode]::Message,
        [IO.Pipes.PipeOptions]::Asynchronous,65536,8192)
    try {
        Write-Output 'ready'
        $pipe.WaitForConnection()
        $buffer = New-Object byte[] 2048
        $n=$pipe.Read($buffer,0,$buffer.Length)
        if ([Text.Encoding]::UTF8.GetString($buffer,0,$n) -ne 'ping') { throw 'Bad command framing.' }
        $bytes = [Text.Encoding]::UTF8.GetBytes('OK ' + ('x' * 20000))
        $pipe.Write($bytes,0,$bytes.Length)
        $n=$pipe.Read($buffer,0,$buffer.Length)
        if ([Text.Encoding]::UTF8.GetString($buffer,0,$n) -ne 'ack') { throw 'Missing response acknowledgement.' }
    } finally { $pipe.Dispose() }
}
try {
    $watch=[Diagnostics.Stopwatch]::StartNew()
    while (-not $job.HasMoreData -and $watch.Elapsed.TotalSeconds -lt 10) { Start-Sleep -Milliseconds 50 }
    $response=Send-Prism 'ping'
    if ($response.Length -ne 20003) { throw 'Multi-buffer response was truncated.' }
    $job | Wait-Job -Timeout 10 | Out-Null
    if ($job.State -ne 'Completed') { throw "Pipe test failed: $($job.State)" }
    $job | Receive-Job -ErrorAction Stop | Out-Null
} finally { $job | Stop-Job; $job | Remove-Job -Force }
Write-Host 'Script parsing and real named-pipe framing passed.'
