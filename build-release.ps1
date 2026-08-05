$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$ProjectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$Solution = Join-Path $ProjectRoot 'PrismTextureStreamerFB.sln'
$VsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'

if (-not (Test-Path $VsWhere)) {
    throw 'Visual Studio 2022 Build Tools were not found. Install the "Desktop development with C++" workload, then run this script again.'
}

$MsBuild = & $VsWhere -latest -products * -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' |
    Select-Object -First 1

if (-not $MsBuild -or -not (Test-Path $MsBuild)) {
    throw 'MSBuild was not found. Install Visual Studio 2022 Build Tools with Desktop development with C++.'
}

& $MsBuild $Solution /m /restore /t:Build /p:Configuration=Release /p:Platform=x64
if ($LASTEXITCODE -ne 0) {
    throw "MSBuild failed with exit code $LASTEXITCODE."
}

$Dll = Join-Path $ProjectRoot 'x64\Release\PrismTextureStreamerFB.dll'
$BridgeDll = Join-Path $ProjectRoot 'x64\Release\PrismCameraBridge.dll'
if (-not (Test-Path $Dll)) {
    throw "Build completed but the expected DLL was not found at: $Dll"
}
if (-not (Test-Path $BridgeDll)) {
    throw "Build completed but the SPF bridge DLL was not found at: $BridgeDll"
}

$Hash = Get-FileHash -Algorithm SHA256 $Dll
$ClientDirectory = Join-Path $ProjectRoot 'PrismMediaClient\bin\x64\Release\net48'
if (-not (Test-Path (Join-Path $ClientDirectory 'PrismMediaClient.exe'))) {
    throw "Build completed but PrismMediaClient.exe was not found at: $ClientDirectory"
}

$Package = Join-Path $ProjectRoot 'x64\Release\PrismTextureStreamerFB-3.9.1-spotify-audio-parity'
if (Test-Path $Package) {
    Remove-Item $Package -Recurse -Force
}
New-Item -ItemType Directory -Path $Package | Out-Null
$Runtime = Join-Path $Package 'PrismTextureStreamerFB'
$Docs = Join-Path $Runtime 'docs'
New-Item -ItemType Directory -Path $Runtime -Force | Out-Null
New-Item -ItemType Directory -Path $Docs -Force | Out-Null
Copy-Item $Dll $Package
Copy-Item (Join-Path $ClientDirectory '*') $Runtime -Recurse
Get-ChildItem $Runtime -Recurse -File |
    Where-Object { $_.Extension -in '.pdb', '.xml' } |
    Remove-Item -Force
Copy-Item (Join-Path $ProjectRoot 'PERFORMANCE-NOTES.md') $Docs
Copy-Item (Join-Path $ProjectRoot 'V3.3-DYNAMIC-OUTSIDE-AUDIO.md') $Docs
Copy-Item (Join-Path $ProjectRoot 'V3.6-TELEMETRY-ENVIRONMENT.md') $Docs
Copy-Item (Join-Path $ProjectRoot 'V3.7-PLAYLIST-CORE-BALANCE.md') $Docs
Copy-Item (Join-Path $ProjectRoot 'V3.8-SPOTIFY-GAMEPAD-DIAGNOSTICS.md') $Docs
Copy-Item (Join-Path $ProjectRoot 'V3.9-SPOTIFY-SESSION-BACKUPS.md') $Docs
$SpfBridge = Join-Path $Runtime 'SPF-OPTIONAL\PrismCameraBridge'
New-Item -ItemType Directory -Path $SpfBridge -Force | Out-Null
Copy-Item $BridgeDll $SpfBridge
Copy-Item (Join-Path $ProjectRoot 'SPF-BRIDGE-INSTALL.txt') (
    Join-Path $Runtime 'SPF-OPTIONAL')

@"
PrismTextureStreamerFB 3.9.1 Spotify Audio Parity + Session Backups

Copy PrismTextureStreamerFB.dll and the PrismTextureStreamerFB folder into
<ETS2 or ATS>\bin\win_x64\plugins. The DLL must stay directly in plugins.
Open the menu with Ctrl+F8.
"@ | Set-Content (Join-Path $Package 'INSTALL.txt')

Write-Host ''
Write-Host "Build succeeded: $Dll" -ForegroundColor Green
Write-Host "Install package: $Package" -ForegroundColor Green
Write-Host "SHA-256: $($Hash.Hash)"
