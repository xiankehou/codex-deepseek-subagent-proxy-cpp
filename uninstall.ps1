<#
.SYNOPSIS
  Remove the ds-adapter: stop the local adapter, remove autostart + watchdog,
  and (by default) restore config.toml / models.json from the setup backups.

.USAGE
  powershell -ExecutionPolicy Bypass -File uninstall.ps1

  -KeepConfig   leave config.toml / models.json untouched
  -CodexHome    override ~/.codex (must match what setup.ps1 used)
#>
param(
  [switch]$KeepConfig,
  [string]$CodexHome = ""
)
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$exe = Join-Path $here "adapter.exe"
if (-not $CodexHome) { $CodexHome = Join-Path $env:USERPROFILE ".codex" }

# 1. stop only adapter processes that were launched from this directory
Get-Process -Name adapter -ErrorAction SilentlyContinue |
  Where-Object { $_.Path -eq $exe } |
  Stop-Process -Force
Write-Host "adapter stopped (if it was running from this folder)."

# 2. remove login autostart
$vbs = Join-Path ([Environment]::GetFolderPath("Startup")) "codex-ds-adapter.vbs"
if (Test-Path -LiteralPath $vbs) {
  Remove-Item -LiteralPath $vbs -Force
  Write-Host "removed login autostart: $vbs"
}

# 3. remove watchdog task
& schtasks.exe /Delete /F /TN "CodexDSAdapterWatchdog" 2>$null | Out-Null
Write-Host "watchdog task removed (if present)."

# 4. restore config backups unless asked to keep current settings
if (-not $KeepConfig) {
  $bakConfig = Join-Path $CodexHome "config.toml.bak-ds-adapter"
  $bakModels = Join-Path $CodexHome "models.json.bak-ds-adapter"
  if (Test-Path -LiteralPath $bakConfig) {
    Copy-Item -LiteralPath $bakConfig -Destination (Join-Path $CodexHome "config.toml") -Force
    Write-Host "config.toml restored from backup."
  } else {
    Write-Host "no config.toml backup found; base_url still points at the adapter. Set it back to https://api.deepseek.com/ manually."
  }
  if (Test-Path -LiteralPath $bakModels) {
    Copy-Item -LiteralPath $bakModels -Destination (Join-Path $CodexHome "models.json") -Force
    Write-Host "models.json restored from backup."
  }
}

Write-Host "uninstall complete. Restart Codex for changes to take effect."
