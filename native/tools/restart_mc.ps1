# Restarts the hidden dev Minecraft for the offline tests (native\tools\fake_host*.cpp).
# Kills only the Fabric dev client, starts run_minecraft_dev.ps1 as player WcTest (never the
# default player Geralt) and waits until it reaches the shared-memory wait or the build fails. Log: logs\mc-run.log.
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Get-CimInstance Win32_Process -Filter "Name='java.exe'" | Where-Object { $_.CommandLine -match 'net.fabricmc|KnotClient' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }
Start-Sleep -Seconds 4
New-Item -ItemType Directory -Force (Join-Path $root "logs") | Out-Null
$log = Join-Path $root "logs\mc-run.log"
Remove-Item $log -ErrorAction SilentlyContinue
$run = Join-Path $root "run_minecraft_dev.ps1"
$cmd = "`$env:WITCHERCRAFT_USERNAME='WcTest'; & '$run' *> '$log'"
Start-Process -WindowStyle Hidden powershell -ArgumentList '-ExecutionPolicy', 'Bypass', '-Command', $cmd
Start-Sleep -Seconds 5
for ($i = 0; $i -lt 80; $i++) { if ((Test-Path $log) -and (Select-String -Path $log -Pattern "shared memory yet|BUILD FAILED" -Quiet)) { break }; Start-Sleep -Seconds 3 }
Select-String -Path $log -Pattern "BUILD FAILED|error:|shared memory yet|Setting user" | Select-Object -First 8 | ForEach-Object Line
