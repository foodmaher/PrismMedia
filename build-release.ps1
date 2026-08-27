$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$ProjectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$Solution = Join-Path $ProjectRoot 'PrismTextureStreamerFB.sln'
$VsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $VsWhere)) {
    throw 'Visual Studio 2022 Build Tools with Desktop development with C++ are required.'
}
$MsBuild = & $VsWhere -latest -products * -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (-not $MsBuild) { throw 'MSBuild was not found.' }

& $MsBuild $Solution /m /restore /t:Build /p:Configuration=Release /p:Platform=x64
if ($LASTEXITCODE -ne 0) { throw "MSBuild failed with exit code $LASTEXITCODE." }

$Output = Join-Path $ProjectRoot 'x64\Release'
$Dll = Join-Path $Output 'PrismMedia.dll'
$BridgeDll = Join-Path $Output 'PrismCameraBridge.dll'
$ClientDirectory = Join-Path $ProjectRoot 'PrismMediaClient\bin\x64\Release\net48'
foreach ($required in @($Dll, $BridgeDll, (Join-Path $ClientDirectory 'PrismMediaClient.exe'))) {
    if (-not (Test-Path $required)) { throw "Expected build output is missing: $required" }
}

$Package = Join-Path $Output 'PrismMedia-4.0.1-early-render-probe'
if (Test-Path $Package) { Remove-Item $Package -Recurse -Force }
New-Item -ItemType Directory -Path $Package | Out-Null
$Runtime = Join-Path $Package 'PrismMedia'
New-Item -ItemType Directory -Path $Runtime | Out-Null
Copy-Item $Dll $Package
Copy-Item (Join-Path $ClientDirectory '*') $Runtime -Recurse
Get-ChildItem $Runtime -Recurse -File | Where-Object { $_.Extension -in '.pdb', '.xml' } | Remove-Item -Force

$SpfBridge = Join-Path $Runtime 'SPF-OPTIONAL\PrismCameraBridge'
New-Item -ItemType Directory -Path $SpfBridge -Force | Out-Null
Copy-Item $BridgeDll $SpfBridge
Copy-Item (Join-Path $ProjectRoot 'SPF-BRIDGE-INSTALL.txt') (Join-Path $Runtime 'SPF-OPTIONAL')
Copy-Item (Join-Path $ProjectRoot 'config-recommended.ini') $Runtime
Copy-Item (Join-Path $ProjectRoot 'PERFORMANCE-NOTES.md') $Runtime
Copy-Item (Join-Path $ProjectRoot 'README.md') $Package

@"
PrismMedia 4.0.1 Early Render Probe

Remove the old PrismTextureStreamerFB.dll, then copy PrismMedia.dll and the
PrismMedia folder into:
<ETS2 or ATS>\bin\win_x64\plugins

Keep the DLL directly inside plugins. Open the interface with Ctrl+F8.
Settings apply live and auto-save. Only texture identity changes need the
System > Reload game textures action.
"@ | Set-Content (Join-Path $Package 'INSTALL.txt')

$Hash = Get-FileHash -Algorithm SHA256 $Dll
Write-Host "Build succeeded: $Package" -ForegroundColor Green
Write-Host "DLL SHA-256: $($Hash.Hash)"
