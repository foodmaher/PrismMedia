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
if (-not (Test-Path $Dll)) {
    throw "Build completed but the expected DLL was not found at: $Dll"
}

$Hash = Get-FileHash -Algorithm SHA256 $Dll
$ClientDirectory = Join-Path $ProjectRoot 'PrismMediaClient\bin\x64\Release\net48'
if (-not (Test-Path (Join-Path $ClientDirectory 'PrismMediaClient.exe'))) {
    throw "Build completed but PrismMediaClient.exe was not found at: $ClientDirectory"
}

$Package = Join-Path $ProjectRoot 'x64\Release\PrismTextureStreamerFB-2.1.0'
if (Test-Path $Package) {
    Remove-Item $Package -Recurse -Force
}
New-Item -ItemType Directory -Path $Package | Out-Null
Copy-Item $Dll $Package
Copy-Item (Join-Path $ClientDirectory '*') $Package -Recurse
Copy-Item (Join-Path $ProjectRoot 'PERFORMANCE-NOTES.md') $Package
Copy-Item (Join-Path $ProjectRoot 'V2.1-ADAPTIVE-TESTING.md') $Package

Write-Host ''
Write-Host "Build succeeded: $Dll" -ForegroundColor Green
Write-Host "Install package: $Package" -ForegroundColor Green
Write-Host "SHA-256: $($Hash.Hash)"
