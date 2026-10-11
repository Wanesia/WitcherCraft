# Configure + build (Release, x64) with the VS 2022 generator.
#   powershell -ExecutionPolicy Bypass -File native\build.ps1 [-Config Release|Debug] [-Scan]
param(
    [string]$Config = "Release",
    [switch]$Scan
)
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$cmake = "cmake"
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) { $cmake = "C:\Program Files\CMake\bin\cmake.exe" }

& $cmake -S $here -B "$here\build" -G "Visual Studio 17 2022" -A x64
if ($LASTEXITCODE -ne 0) { throw "configure failed" }
& $cmake --build "$here\build" --config $Config --parallel
if ($LASTEXITCODE -ne 0) { throw "build failed" }

$out = "$here\build\$Config"
Write-Host "Built: $out\dinput8.dll, $out\witchercraft.asi, $out\wcraft_scan.exe"
if ($Scan) { & "$out\wcraft_scan.exe" }
