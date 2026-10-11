# Builds both halves of WitcherCraft and packs a release into dist\:
#   WitcherCraft-<version>.zip        the Witcher 3 mod, laid out like the game folder (unzip into
#                                     the game folder): the plugin, the scripts, and
#                                     WitcherCraft-Minecraft.zip, the Minecraft it starts
#   witchercraft-minecraft-<version>.jar  the Minecraft mod on its own (for your own launcher)
#   WitcherCraft-SHA256SUMS.txt           checksums of the two files above
# (GitHub lists release files alphabetically, ignoring case: these names keep the zip first.)
# and, kept out of dist\ (not for publishing: a PDB names the folders it was built in):
#   symbols\WitcherCraft-<version>-pdb.zip   the plugin's debug symbols, for reading crash dumps
#
# WitcherCraft-Minecraft.zip holds a portable Prism Launcher with a ready "WitcherCraft" instance
# (Minecraft 26.3, Fabric, Fabric API, WitcherCraft). The plugin unpacks it to %LOCALAPPDATA%\WitcherCraft
# and starts it (native\src\launcher.cpp); Prism asks the player to sign in once, then downloads
# Minecraft and Java itself. Minecraft and The Witcher 3 are never included.
#
#   powershell -ExecutionPolicy Bypass -File release\package.ps1 [-NoBuild]
param([switch]$NoBuild)
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"
$root = Split-Path -Parent $PSScriptRoot
$version = (Select-String -Path "$root\minecraft\gradle.properties" -Pattern '^version=(.+)$').Matches[0].Groups[1].Value.Trim()

# Pinned downloads (checked against these hashes; same builds SkyCraft 0.1.2 ships).
$prismVersion = "11.1.1"
$prismZip = "PrismLauncher-Windows-MSVC-Portable-$prismVersion.zip"
$prismUrl = "https://github.com/PrismLauncher/PrismLauncher/releases/download/$prismVersion/$prismZip"
$prismSha256 = "ab35a770fb06d89d2ccc098079db5db329fb4e68f42b72babd8b095efde3d2d7"
$prismLicenseUrl = "https://raw.githubusercontent.com/PrismLauncher/PrismLauncher/$prismVersion/LICENSE"
$fabricApiJar = "fabric-api-0.161.0+26.3.jar"
$fabricApiUrl = "https://cdn.modrinth.com/data/P7dR8mSH/versions/bNnaTiuM/fabric-api-0.161.0%2B26.3.jar"
$fabricApiSha512 = "ed6b2586d6fde11fde8472f5a527c51e99b67026e46f94d4bfd85e7e28ce5ee299173ee16ad576ceb51f39f98d30a811086a6deb1a86a524859cc16e12da109d"

function Get-Pinned([string]$url, [string]$path, [string]$algorithm, [string]$hash) {
    if (-not (Test-Path $path)) {
        New-Item -ItemType Directory (Split-Path $path) -Force | Out-Null
        Invoke-WebRequest -Uri $url -OutFile $path -UseBasicParsing
    }
    if ($hash -and (Get-FileHash $path -Algorithm $algorithm).Hash -ne $hash.ToUpper()) {
        Remove-Item $path
        throw "$path doesn't match its pinned $algorithm hash"
    }
}

# Zip entries named explicitly with forward slashes, as the zip format (and every mod manager)
# expects; Windows PowerShell's own zipping writes backslashes.
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
function New-Zip([string]$path, [System.Collections.IDictionary]$entries) {
    $zip = [System.IO.Compression.ZipFile]::Open($path, [System.IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($name in $entries.Keys) {
            [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $entries[$name], $name, [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
        }
    } finally { $zip.Dispose() }
}
function Add-Folder([System.Collections.IDictionary]$entries, [string]$prefix, [string]$folder) {
    $base = (Resolve-Path $folder).Path.TrimEnd('\') + '\'
    Get-ChildItem $folder -Recurse -File | Sort-Object FullName | ForEach-Object {
        $entries[$prefix + $_.FullName.Substring($base.Length).Replace('\', '/')] = $_.FullName
    }
}

# Nothing published may name the folders it was built in (the builder's user folder: their Windows user
# name). Checks the bytes of each file, and of each entry of a .jar/.zip, as ANSI and as UTF-16 text.
$private = @($env:USERPROFILE, $root) | Where-Object { $_ } | ForEach-Object { $_.TrimEnd('\') } | Sort-Object -Unique
$latin1 = [System.Text.Encoding]::GetEncoding(28591)
function Test-Bytes([byte[]]$bytes, [string]$what) {
    $ansi = $latin1.GetString($bytes)
    $wide = [System.Text.Encoding]::Unicode.GetString($bytes)
    $wideOdd = if ($bytes.Length -gt 1) { [System.Text.Encoding]::Unicode.GetString($bytes, 1, $bytes.Length - 1) } else { "" }
    foreach ($p in $private) {
        foreach ($form in @($p, $p.Replace('\', '/'))) {
            if ($ansi.IndexOf($form, [StringComparison]::OrdinalIgnoreCase) -ge 0 -or $wide.IndexOf($form, [StringComparison]::OrdinalIgnoreCase) -ge 0 -or
                $wideOdd.IndexOf($form, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
                throw "$what contains the build folder '$form'; not publishing it"
            }
        }
    }
}
function Assert-NoPrivatePaths([string]$path) {
    if ($path -match '\.(jar|zip)$') {
        $zip = [System.IO.Compression.ZipFile]::OpenRead($path)
        try {
            foreach ($e in $zip.Entries) {
                if ($e.Length -eq 0) { continue }
                $s = $e.Open(); $m = New-Object System.IO.MemoryStream
                try { $s.CopyTo($m) } finally { $s.Dispose() }
                Test-Bytes $m.ToArray() "$path!$($e.FullName)"
            }
        } finally { $zip.Dispose() }
    } else {
        Test-Bytes ([System.IO.File]::ReadAllBytes($path)) $path
    }
}

if (-not $NoBuild) {
    & powershell -ExecutionPolicy Bypass -File "$root\native\build.ps1"
    if ($LASTEXITCODE) { throw "the native plugin didn't build" }
    if (-not $env:JAVA_HOME) {
        $jdk = Get-ChildItem "C:\Program Files\Eclipse Adoptium" -Directory -Filter "jdk-25*" -ErrorAction SilentlyContinue | Select-Object -Last 1
        if ($jdk) { $env:JAVA_HOME = $jdk.FullName }
    }
    Push-Location "$root\minecraft"
    try {
        # --no-daemon: a daemon left running keeps this script's output handles open (callers hang)
        .\gradlew.bat build --no-daemon --no-configuration-cache --console=plain
        if ($LASTEXITCODE) { throw "the Fabric mod didn't build" }
    } finally { Pop-Location }
}

$dll = "$root\native\build\Release\dinput8.dll"
$pdb = "$root\native\build\Release\dinput8.pdb"
$jar = "$root\minecraft\build\libs\witchercraft-$version.jar"
$scripts = "$root\mod\modWitcherCraft"
foreach ($f in @($dll, $pdb, $jar, $scripts)) {
    if (-not (Test-Path $f)) { throw "missing $f (build first, or drop -NoBuild)" }
}
$cache = "$root\release\.cache"
Get-Pinned $prismUrl "$cache\$prismZip" SHA256 $prismSha256
Get-Pinned $fabricApiUrl "$cache\$fabricApiJar" SHA512 $fabricApiSha512
Get-Pinned $prismLicenseUrl "$cache\PrismLauncher-$prismVersion-LICENSE.txt" "" ""

$dist = "$root\dist"
New-Item -ItemType Directory $dist -Force | Out-Null
Get-ChildItem $dist | Remove-Item -Recurse -Force

# The bundled Minecraft: Prism (portable), the WitcherCraft instance, its mods, Prism's default settings.
$bundle = "$dist\bundle"
Copy-Item -Recurse "$root\release\minecraft-bundle" $bundle
Expand-Archive "$cache\$prismZip" "$bundle\Prism" -Force
Copy-Item "$cache\PrismLauncher-$prismVersion-LICENSE.txt" "$bundle\Prism\LICENSE-PrismLauncher.txt"
(Get-Content "$bundle\Prism\THIRD-PARTY.txt" -Raw).Replace("{PRISM_VERSION}", $prismVersion) | Set-Content "$bundle\Prism\THIRD-PARTY.txt" -NoNewline
$mods = "$bundle\Prism\instances\WitcherCraft\.minecraft\mods"
New-Item -ItemType Directory $mods -Force | Out-Null
Copy-Item "$cache\$fabricApiJar" $mods
Copy-Item $jar "$mods\witchercraft-$version.jar"
Set-Content "$bundle\bundle-version.txt" "WitcherCraft $version, Prism Launcher $prismVersion, $fabricApiJar" -NoNewline
$bundleEntries = [ordered]@{}
Add-Folder $bundleEntries "" $bundle
New-Zip "$dist\WitcherCraft-Minecraft.zip" $bundleEntries
Remove-Item -Recurse -Force $bundle

# The mod, laid out like the game folder.
$entries = [ordered]@{
    "bin/x64_dx12/dinput8.dll" = $dll
    "bin/x64_dx12/witchercraft/WitcherCraft-Minecraft.zip" = "$dist\WitcherCraft-Minecraft.zip"
    "bin/x64_dx12/witchercraft/README.txt" = "$root\release\README.txt"
    "bin/x64_dx12/witchercraft/LICENSE.txt" = "$root\LICENSE"
    "bin/x64_dx12/witchercraft/THIRD-PARTY-NOTICES.md" = "$root\THIRD-PARTY-NOTICES.md"
}
Add-Folder $entries "mods/modWitcherCraft/" $scripts
New-Zip "$dist\WitcherCraft-$version.zip" $entries
$symbols = "$root\symbols"
New-Item -ItemType Directory $symbols -Force | Out-Null
Remove-Item "$symbols\WitcherCraft-$version-pdb.zip" -ErrorAction SilentlyContinue
New-Zip "$symbols\WitcherCraft-$version-pdb.zip" ([ordered]@{ "dinput8.pdb" = $pdb })
Copy-Item $jar "$dist\witchercraft-minecraft-$version.jar"
foreach ($f in @($dll, $jar, "$dist\WitcherCraft-Minecraft.zip", "$dist\WitcherCraft-$version.zip")) {
    try { Assert-NoPrivatePaths $f } catch { Get-ChildItem $dist | Remove-Item -Recurse -Force; throw }
}
Write-Host "Checked: no build-folder paths in the published files"
Remove-Item "$dist\WitcherCraft-Minecraft.zip"

$files = Get-ChildItem $dist -File | Sort-Object Name
$files | ForEach-Object { "{0}  {1}" -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower(), $_.Name } | Set-Content "$dist\WitcherCraft-SHA256SUMS.txt"
$files | ForEach-Object { "{0,-36} {1,12:N0} bytes" -f $_.Name, $_.Length }
