$ErrorActionPreference = 'Stop'

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Required = @(
    'PrismTextureStreamerFB\environment_audio.cpp',
    'PrismTextureStreamerFB\environment_audio.h',
    'PrismMediaClient\MainForm.cs',
    'V3.6-TELEMETRY-ENVIRONMENT.md'
)

foreach ($RelativePath in $Required) {
    if (-not (Test-Path (Join-Path $Root $RelativePath))) {
        throw "The update was not extracted into the repository root. Missing: $RelativePath"
    }
}

$Obsolete = @(
    'PrismTextureStreamerFB\wind_audio.cpp',
    'PrismTextureStreamerFB\wind_audio.h',
    'PrismMediaClient\WindAudioController.cs',
    'V3.4-WINDOW-WIND.md',
    'V3.5-ENVIRONMENT-AUDIO.md'
)

foreach ($RelativePath in $Obsolete) {
    $Path = Join-Path $Root $RelativePath
    if (Test-Path $Path) {
        Remove-Item $Path -Force
        Write-Host "Removed $RelativePath"
    }
}

Write-Host ''
Write-Host 'PrismTextureStreamerFB v3.6 source update is ready.' -ForegroundColor Green
Write-Host 'Commit the changes, push them, then run the Build Windows DLL workflow.'
