<#
.SYNOPSIS
  One-click setup: build (if needed), point Codex's DeepSeek provider at the
  local adapter, register login autostart + watchdog, start the adapter, and
  run a self-test.

.USAGE
  powershell -ExecutionPolicy Bypass -File setup.ps1

  -CodexHome    override ~/.codex (default: $env:USERPROFILE\.codex)
  -SkipBuild    use an existing adapter.exe (do not compile)
  -NoAutostart  skip login autostart and watchdog task
  -DryRun       print what would change without writing anything
#>
param(
  [string]$CodexHome = "",
  [switch]$SkipBuild,
  [switch]$NoAutostart,
  [switch]$DryRun
)
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$exe = Join-Path $here "adapter.exe"
if (-not $CodexHome) { $CodexHome = Join-Path $env:USERPROFILE ".codex" }
$config = Join-Path $CodexHome "config.toml"
$models = Join-Path $CodexHome "models.json"
$adapterUrl = "http://127.0.0.1:8787/"

function Step($msg) { Write-Host "== $msg" }
function Ok($msg) { Write-Host "OK  $msg" -ForegroundColor Green }
function Warn($msg) { Write-Host "WARN $msg" -ForegroundColor Yellow }

# ---- 1. adapter.exe ---------------------------------------------------------
Step "1/6 ensure adapter.exe"
if (-not (Test-Path -LiteralPath $exe)) {
  if ($SkipBuild) { throw "adapter.exe not found (use -SkipBuild only after building it)." }
  & (Join-Path $here "build.ps1")
} else {
  Ok "found $exe"
}

# ---- 2. config.toml (DeepSeek provider section only) -------------------------
Step "2/6 patch $config"
if (-not (Test-Path -LiteralPath $config)) {
  throw "config.toml not found: $config. Configure DeepSeek as your Codex provider first (see README)."
}
$bytes = [System.IO.File]::ReadAllBytes($config)
$hasBom = ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF)
$text = [System.IO.File]::ReadAllText($config)
if ($text -notmatch '(?m)^\s*\[model_providers\.deepseek\]\s*(#.*)?$') {
  throw "config.toml has no [model_providers.deepseek] section. See README step 0."
}

$inSection = $false
$found = $false
$changed = $false
$outLines = foreach ($line in ($text -split "\r?\n")) {
  if ($line -match '^\s*\[') {
    $inSection = ($line -match '^\s*\[model_providers\.deepseek\]\s*(#.*)?$')
  }
  if ($inSection -and $line -match '^\s*base_url\s*=') {
    $found = $true
    if ($line -match [regex]::Escape($adapterUrl)) {
      $line
    } else {
      $changed = $true
      'base_url = "http://127.0.0.1:8787/"'
    }
  } else {
    $line
  }
}
if (-not $found) { throw "No base_url line inside [model_providers.deepseek]. See README step 0." }

if ($changed) {
  $nl = if ($text.Contains("`r`n")) { "`r`n" } else { "`n" }
  $newText = [string]::Join($nl, $outLines)
  $bak = Join-Path $CodexHome "config.toml.bak-ds-adapter"
  if ($DryRun) {
    Warn "[DryRun] would set base_url = $adapterUrl (backup: $bak)"
  } else {
    if (-not (Test-Path -LiteralPath $bak)) {
      Copy-Item -LiteralPath $config -Destination $bak
      Ok "backup -> $bak"
    }
    $enc = if ($hasBom) { [System.Text.UTF8Encoding]::new($true) } else { [System.Text.UTF8Encoding]::new($false) }
    [System.IO.File]::WriteAllText($config, $newText, $enc)
    Ok "base_url -> $adapterUrl"
  }
} else {
  Ok "base_url already $adapterUrl"
}

# ---- 3. models.json (multi_agent_version v2 for deepseek models) -------------
Step "3/6 ensure deepseek models use multi_agent_version v2"
if (Test-Path -LiteralPath $models) {
  $json = Get-Content -LiteralPath $models -Raw | ConvertFrom-Json
  $needWrite = $false
  foreach ($m in @($json.models)) {
    if ($m.slug -like "deepseek*" -and $m.multi_agent_version -ne "v2") {
      $m.multi_agent_version = "v2"
      $needWrite = $true
    }
  }
  if ($needWrite) {
    $bak = Join-Path $CodexHome "models.json.bak-ds-adapter"
    if ($DryRun) {
      Warn "[DryRun] would set multi_agent_version=v2 for deepseek models (backup: $bak)"
    } else {
      if (-not (Test-Path -LiteralPath $bak)) {
        Copy-Item -LiteralPath $models -Destination $bak
        Ok "backup -> $bak"
      }
      $outJson = $json | ConvertTo-Json -Depth 100
      [System.IO.File]::WriteAllText($models, $outJson, [System.Text.UTF8Encoding]::new($false))
      Ok "deepseek models -> multi_agent_version v2"
    }
  } else {
    Ok "deepseek models already v2 (or no deepseek models found)"
  }
} else {
  Warn "models.json not found; skipping (spawn_agent tools may not appear)"
}

# ---- 4. autostart ------------------------------------------------------------
Step "4/6 autostart + watchdog"
if ($NoAutostart) {
  Warn "skipped (-NoAutostart)"
} else {
  $startup = [Environment]::GetFolderPath("Startup")
  $vbs = Join-Path $startup "codex-ds-adapter.vbs"
  $vbsBody = 'Set ws = CreateObject("Wscript.Shell")' + "`r`n" + 'ws.Run """' + $exe + '""", 0, False' + "`r`n"
  if ($DryRun) {
    Warn "[DryRun] would write $vbs (login autostart, hidden)"
  } else {
    [System.IO.File]::WriteAllText($vbs, $vbsBody, [System.Text.UnicodeEncoding]::new($false, $true))
    Ok "login autostart -> $vbs"
  }

  $taskName = "CodexDSAdapterWatchdog"
  $tr = '"' + $exe + '" watchdog'
  if ($DryRun) {
    Warn "[DryRun] would register scheduled task $taskName (every 1 min, hidden): $tr"
  } else {
    try {
      & schtasks.exe /Create /F /TN $taskName /SC MINUTE /MO 1 /TR $tr | Out-Null
      if ($LASTEXITCODE -eq 0) {
        Ok "watchdog task $taskName registered"
      } else {
        Warn "schtasks exited $LASTEXITCODE (watchdog optional; adapter still starts at login)"
      }
    } catch {
      Warn "watchdog task failed: $($_.Exception.Message)"
    }
  }
}

# ---- 5. start adapter --------------------------------------------------------
Step "5/6 start adapter"
$alive = $false
try {
  $r = Invoke-WebRequest -UseBasicParsing -Uri "http://127.0.0.1:8787/__ping" -TimeoutSec 2
  $alive = ($r.StatusCode -eq 200)
} catch { $alive = $false }
if ($alive) {
  Ok "adapter already listening on 8787"
} elseif ($DryRun) {
  Warn "[DryRun] would start $exe"
} else {
  Start-Process -FilePath $exe -WorkingDirectory $here -WindowStyle Hidden
  $ok = $false
  for ($i = 0; $i -lt 10; $i++) {
    Start-Sleep -Milliseconds 300
    try {
      $r = Invoke-WebRequest -UseBasicParsing -Uri "http://127.0.0.1:8787/__ping" -TimeoutSec 1
      if ($r.StatusCode -eq 200) { $ok = $true; break }
    } catch {}
  }
  if ($ok) { Ok "adapter started" } else { Warn "adapter did not answer; check adapter.log" }
}

# ---- 6. selftest -------------------------------------------------------------
Step "6/6 selftest"
if ($DryRun) {
  Warn "[DryRun] would run: $exe selftest"
  return
}
$sp = Start-Process -FilePath $exe -ArgumentList @('selftest') -WorkingDirectory $here -Wait -PassThru -WindowStyle Hidden
$rc = $sp.ExitCode
$selftestLog = Join-Path $here "selftest.log"
if (Test-Path -LiteralPath $selftestLog) {
  Get-Content -LiteralPath $selftestLog | ForEach-Object { Write-Host "  $_" }
}
if ($rc -eq 0) {
  Write-Host ""
  Write-Host "Setup complete. Next steps:" -ForegroundColor Green
  Write-Host "  1. Fully quit Codex (all windows) and reopen it (recommended; base_url changes usually apply to existing sessions too)." -ForegroundColor Green
  Write-Host "  2. Start a NEW task and ask the main agent to spawn a subagent, e.g.:" -ForegroundColor Green
  Write-Host "     TOKEN=DS_OK_12345   only reply with this token." -ForegroundColor Green
  Write-Host "  3. adapter.log should contain 'rewrote agent message' when it happens." -ForegroundColor Green
} else {
  Write-Host "Self-test FAILED - see selftest.log / adapter.log" -ForegroundColor Red
  exit 1
}
