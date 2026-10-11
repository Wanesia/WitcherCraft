# Starts the hidden Minecraft (offline dev client) for WitcherCraft.
# Keeps its own rendering light so it doesn't compete with Witcher 3 for the GPU.
#   -HudOnly  : upstream SkyCraft HUD-only overlay instead of the M3b world layer
#               (Minecraft's blocks/entities/hand drawn over the Witcher frame).
#   -NoDepth  : don't export Minecraft's depth (no occlusion by Witcher geometry).
#   -OldLook  : the pre-M5b texture settings (RGSS filtering, no anisotropy, dynamic FOV) for comparisons.
#               Only until Minecraft links: the mod then applies the M5b values itself (SkyClient.applyLinkedOptions).
param([switch]$HudOnly, [switch]$NoDepth, [switch]$OldLook)
$ErrorActionPreference = "Stop"
$mc = Join-Path $PSScriptRoot "minecraft"
$opts = Join-Path $mc "run\options.txt"

$want = [ordered]@{ renderDistance = "4"; simulationDistance = "5"; maxFps = "60"; enableVsync = "false"; menuBackgroundBlurriness = "0" }
# M5b "crisp blocks": anisotropic filtering 8x instead of RGSS (RGSS
# softens every texture, and mipmaps without anisotropy smear blocks seen at grazing angles), and no
# FOV pumping when sprinting (the Witcher camera must use exactly Minecraft's FOV every frame).
# menuBackgroundBlurriness 0 (always): MC blurs its level behind chat/inventory, i.e. the whole world layer.
if ($OldLook) {
    $want["textureFiltering"] = "1"; $want["maxAnisotropyBit"] = "1"; $want["fovEffectScale"] = "1.0"
} else {
    $want["textureFiltering"] = "2"; $want["maxAnisotropyBit"] = "3"; $want["mipmapLevels"] = "4"; $want["fovEffectScale"] = "0.0"
}
if (Test-Path $opts) {
    $seen = @{}
    $lines = Get-Content $opts -Encoding UTF8 | ForEach-Object {
        $k = ($_ -split ":", 2)[0]
        if ($want.Contains($k)) { $seen[$k] = $true; "${k}:$($want[$k])" } else { $_ }
    }
    foreach ($k in $want.Keys) { if (-not $seen.ContainsKey($k)) { $lines += "${k}:$($want[$k])" } }
    # UTF-8 without a BOM, as Minecraft writes it (a BOM would hide the first option).
    [System.IO.File]::WriteAllLines($opts, [string[]]$lines, (New-Object System.Text.UTF8Encoding $false))
}

# JDK 25: JAVA_HOME if it is one, else the newest Temurin 25 in its default folder.
if (-not ($env:JAVA_HOME -and (Test-Path (Join-Path $env:JAVA_HOME "bin\java.exe")) -and ((Get-Item (Join-Path $env:JAVA_HOME "release") -ErrorAction SilentlyContinue | Get-Content) -match 'JAVA_VERSION="25')) ) {
    $jdk = Get-ChildItem "C:\Program Files\Eclipse Adoptium" -Directory -Filter "jdk-25*" -ErrorAction SilentlyContinue | Sort-Object Name | Select-Object -Last 1
    if (-not $jdk) { throw "JDK 25 not found: install Temurin 25 or set JAVA_HOME to a JDK 25" }
    $env:JAVA_HOME = $jdk.FullName
}
Set-Location $mc
$wl = if ($HudOnly) { "false" } else { "true" }
$depth = if ($NoDepth) { "false" } else { "true" }
Write-Host "WitcherCraft: world layer = $wl, depth export = $depth (-Dwitchercraft.worldLayer / -Dwitchercraft.depth)"
& .\gradlew.bat runClient --no-daemon --console=plain "-PworldLayer=$wl" "-Pdepth=$depth"
