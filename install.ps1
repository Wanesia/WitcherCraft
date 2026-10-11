# Installs WitcherCraft into The Witcher 3 (patch 5.0 or 5.01). Run uninstall.ps1 to remove.
param([string]$Game = "C:\Program Files (x86)\Steam\steamapps\common\The Witcher 3")
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$bin = Join-Path $Game "bin\x64_dx12"

$exe = Get-Item (Join-Path $bin "witcher3.exe")
# Builds checked offline with wcraft_scan + col_scan (signatures resolve).
$tested = "5.0.0.1044392", "5.0.0.1048522"
if (-not ($tested | Where-Object { $exe.VersionInfo.FileVersion -like "$_*" })) {
    Write-Warning "witcher3.exe is $($exe.VersionInfo.FileVersion); WitcherCraft was checked on $($tested -join ', '). Run native\build\Release\wcraft_scan.exe and col_scan.exe; the native part disables itself if signatures don't match."
}

$dll = Join-Path $bin "dinput8.dll"
if ((Test-Path $dll) -and -not (Select-String -Path $dll -Pattern "WitcherCraft" -SimpleMatch -Quiet)) {
    throw "A different dinput8.dll is already in $bin (probably an ASI loader). Not overwriting it."
}
$built = Join-Path $root "native\build\Release\dinput8.dll"
if (-not (Test-Path $built)) { throw "Build the plugin first: powershell -ExecutionPolicy Bypass -File native\build.ps1" }
# A release install's bundled Minecraft would start alongside the dev one ("already running").
if (Test-Path (Join-Path $bin "witchercraft\WitcherCraft-Minecraft.zip")) {
    Write-Warning "A release install is here too (bin\x64_dx12\witchercraft\): run uninstall.ps1 first, or it starts its own Minecraft."
}
Copy-Item $built $dll -Force
# No ini: the plugin's defaults are native\witchercraft_native.example.ini. A witchercraft_native.ini
# already in the game folder (your own settings) is kept.

$modDst = Join-Path $Game "mods\modWitcherCraft"
if (Test-Path $modDst) { Remove-Item $modDst -Recurse -Force }
New-Item -ItemType Directory -Force (Join-Path $Game "mods") | Out-Null
Copy-Item (Join-Path $root "mod\modWitcherCraft") $modDst -Recurse -Force

Write-Host "Installed: $dll"
Write-Host "Installed: $modDst"
