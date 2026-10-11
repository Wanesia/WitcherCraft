# Removes everything install.ps1 put into the game folder.
param([string]$Game = "C:\Program Files (x86)\Steam\steamapps\common\The Witcher 3")
$bin = Join-Path $Game "bin\x64_dx12"
$dll = Join-Path $bin "dinput8.dll"
if ((Test-Path $dll) -and (Select-String -Path $dll -Pattern "WitcherCraft" -SimpleMatch -Quiet)) { Remove-Item $dll -Force; Write-Host "Removed $dll" }
foreach ($f in "witchercraft_native.log", "witchercraft_native.ini") { $p = Join-Path $bin $f; if (Test-Path $p) { Remove-Item $p -Force } }
# A release install also brings bin\x64_dx12\witchercraft\ (the bundled Minecraft zip, readme, licences).
# Its unpacked Minecraft in %LOCALAPPDATA%\WitcherCraft (sign-in, world) is left alone.
$rel = Join-Path $bin "witchercraft"
if (Test-Path $rel) { Remove-Item $rel -Recurse -Force; Write-Host "Removed $rel" }
$mod = Join-Path $Game "mods\modWitcherCraft"
if (Test-Path $mod) { Remove-Item $mod -Recurse -Force; Write-Host "Removed $mod" }
