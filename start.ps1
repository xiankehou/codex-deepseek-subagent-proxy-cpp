# Start the local adapter if it is not already running (no window).
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$exe = Join-Path $here "adapter.exe"
if (-not (Test-Path -LiteralPath $exe)) {
  Write-Host "adapter.exe not found. Run build.ps1 first." -ForegroundColor Red
  exit 1
}

$alive = $false
try {
  $r = Invoke-WebRequest -UseBasicParsing -Uri "http://127.0.0.1:8787/__ping" -TimeoutSec 2
  $alive = ($r.StatusCode -eq 200)
} catch { $alive = $false }
if ($alive) {
  Write-Host "adapter already running."
  exit 0
}

Start-Process -FilePath $exe -WorkingDirectory $here -WindowStyle Hidden
Start-Sleep -Milliseconds 800
try {
  $r = Invoke-WebRequest -UseBasicParsing -Uri "http://127.0.0.1:8787/__ping" -TimeoutSec 3
  Write-Host "adapter running (HTTP $($r.StatusCode))."
} catch {
  Write-Host "adapter may not have started; check adapter.log" -ForegroundColor Yellow
}
