# Like restart_mc.ps1, but starts Minecraft in PRODUCTION mode (gradlew prodClient: the built jar + the
# release's Fabric API, as Prism runs a release) for the offline tests. Log: logs\mc-prod.log.
#   -Window : keep Minecraft's window (-PstartHidden=false); the release starts hidden.
param([switch]$Window)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Get-CimInstance Win32_Process -Filter "Name='java.exe'" | Where-Object { $_.CommandLine -match 'net.fabricmc|KnotClient' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }
Start-Sleep -Seconds 3
New-Item -ItemType Directory -Force (Join-Path $root "logs") | Out-Null
$log = Join-Path $root "logs\mc-prod.log"
Remove-Item $log -ErrorAction SilentlyContinue
if (-not $env:JAVA_HOME) {
    $jdk = Get-ChildItem "C:\Program Files\Eclipse Adoptium" -Directory -Filter "jdk-25*" -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($jdk) { $env:JAVA_HOME = $jdk.FullName }
}
$hidden = if ($Window) { "false" } else { "true" }
$cmd = "Set-Location '$root\minecraft'; & .\gradlew.bat prodClient --no-daemon --console=plain --no-configuration-cache -PstartHidden=$hidden *> '$log'"
Start-Process -WindowStyle Hidden powershell -ArgumentList '-ExecutionPolicy', 'Bypass', '-Command', $cmd
Start-Sleep -Seconds 5
for ($i = 0; $i -lt 100; $i++) { if ((Test-Path $log) -and (Select-String -Path $log -Pattern "shared memory yet|BUILD FAILED" -Quiet)) { break }; Start-Sleep -Seconds 3 }
Select-String -Path $log -Pattern "BUILD FAILED|error:|shared memory yet" | Select-Object -First 4 | ForEach-Object Line
