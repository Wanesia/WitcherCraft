# Release check without the game: runs fake_host tests against the Minecraft a release installs
# (%LOCALAPPDATA%\WitcherCraft: bundled Prism, signed in once). For each test: start Minecraft through the
# plugin's own launcher code (launcher_test.exe, bundle from dist\), wait until it waits for the host,
# run the test, then wait for Minecraft to quit by itself (it follows the host). Logs: logs\prism-<test>.log.
#   powershell -ExecutionPolicy Bypass -File native\tools\prism_tests.ps1 [control identity ...]
param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Tests)
$ErrorActionPreference = "Stop"
if (-not $Tests) { $Tests = @("control", "identity", "combat", "features", "flight", "walls", "tnt") }
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$rel = "$root\native\build\Release"
$mcLog = "$env:LOCALAPPDATA\WitcherCraft\Prism\instances\WitcherCraft\.minecraft\logs\latest.log"
$env:WC_MC_LOG = $mcLog # the tests that read Minecraft's log read this one, not the dev client's
$version = (Select-String -Path "$root\minecraft\gradle.properties" -Pattern '^version=(.+)$').Matches[0].Groups[1].Value.Trim()

New-Item -ItemType Directory -Force "$rel\witchercraft", "$root\logs" | Out-Null
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::OpenRead("$root\dist\WitcherCraft-$version.zip")
try {
    $entry = $zip.Entries | Where-Object FullName -eq "bin/x64_dx12/witchercraft/WitcherCraft-Minecraft.zip"
    [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, "$rel\witchercraft\WitcherCraft-Minecraft.zip", $true)
} finally { $zip.Dispose() }
# Same timestamp as the copy already unpacked, so the launcher doesn't unpack it again.
(Get-Item "$rel\witchercraft\WitcherCraft-Minecraft.zip").LastWriteTime = $entry.LastWriteTime.DateTime

function Test-McRunning {
    try { $m = [System.Threading.Mutex]::OpenExisting("Local\SkyCraft_v1_minecraft"); $m.Dispose(); return $true } catch { return $false }
}

$wcConfig = "$env:LOCALAPPDATA\WitcherCraft\Prism\instances\WitcherCraft\.minecraft\config\witchercraft.properties"
$results = @()
foreach ($t in $Tests) {
    $start = Get-Date
    # The flight test's climb step uses the "Climbing help" option (off by default): on for that test
    # only, the player's own file put back afterwards. Minecraft reads it when it starts (below).
    $savedConfig = $null
    if ($t -eq "flight") {
        $savedConfig = if (Test-Path $wcConfig) { Get-Content -Raw $wcConfig } else { "" }
        New-Item -ItemType Directory -Force (Split-Path $wcConfig) | Out-Null
        $lines = @(if ($savedConfig) { $savedConfig -split "`r?`n" | Where-Object { $_ -and $_ -notmatch '^climbHelp=' } }) + "climbHelp=true"
        Set-Content -Path $wcConfig -Value $lines -Encoding ascii
    }
    $ready = (Test-McRunning) -and (Select-String -Path $mcLog -Pattern "shared memory yet" -Quiet) -and -not (Select-String -Path $mcLog -Pattern "linked to The Witcher" -Quiet)
    if (-not $ready) { & "$rel\launcher_test.exe" $env:LOCALAPPDATA *> "$root\logs\prism-$t-launcher.log" } # (a Minecraft already waiting is used as is)
    for ($i = 0; $i -lt 120 -and -not $ready; $i++) {
        Start-Sleep -Seconds 2
        if ((Test-Path $mcLog) -and (Get-Item $mcLog).LastWriteTime -gt $start -and (Select-String -Path $mcLog -Pattern "shared memory yet" -Quiet)) { $ready = $true; break }
    }
    if (-not $ready) {
        Copy-Item "$env:LOCALAPPDATA\WitcherCraft\Prism\logs\PrismLauncher-0.log" "$root\logs\prism-$t-prism.log" -ErrorAction SilentlyContinue
        $results += "${t}: Minecraft not ready after 240 s (see logs\prism-$t-launcher.log, -prism.log)"; continue
    }
    & "$rel\fake_host.exe" $t *> "$root\logs\prism-$t.log"
    $code = $LASTEXITCODE
    $line = (Select-String -Path "$root\logs\prism-$t.log" -Pattern "test: (PASS|FAIL)" | Select-Object -Last 1).Line
    $results += "${t} (exit $code): " + $(if ($line) { ($line -split '\] ', 2)[-1].Substring(0, [Math]::Min(150, ($line -split '\] ', 2)[-1].Length)) } else { "no result line" })
    Copy-Item $mcLog "$root\logs\prism-$t-mc.log" -ErrorAction SilentlyContinue
    for ($i = 0; $i -lt 30 -and (Test-McRunning); $i++) { Start-Sleep -Seconds 1 }
    if (Test-McRunning) { $results += "  ${t}: Minecraft did NOT quit 30 s after the host closed" }
    if ($null -ne $savedConfig) {
        if ($savedConfig) { Set-Content -Path $wcConfig -Value $savedConfig -NoNewline -Encoding ascii } else { Remove-Item $wcConfig -ErrorAction SilentlyContinue }
    }
}
$results
