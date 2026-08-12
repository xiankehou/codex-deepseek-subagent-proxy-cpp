# Build adapter.exe with MSVC (Visual Studio 2019/2022 or Build Tools).
# Requires the "Desktop development with C++" workload. No third-party deps.
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$src = Join-Path $here "adapter.cpp"
$out = Join-Path $here "adapter.exe"

$vcvars = $null
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path -LiteralPath $vswhere) {
  $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
  if ($vs) {
    $cand = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
    if (Test-Path -LiteralPath $cand) { $vcvars = $cand }
  }
}
if (-not $vcvars) {
  $roots = @(
    (Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio"),
    (Join-Path ${env:ProgramFiles} "Microsoft Visual Studio")
  )
  foreach ($root in $roots) {
    foreach ($edition in @("2022\BuildTools", "2022\Community", "2022\Professional", "2022\Enterprise",
                           "2019\BuildTools", "2019\Community", "2019\Professional", "2019\Enterprise")) {
      $cand = Join-Path $root "$edition\VC\Auxiliary\Build\vcvars64.bat"
      if (Test-Path -LiteralPath $cand) { $vcvars = $cand; break }
    }
    if ($vcvars) { break }
  }
}
if (-not $vcvars) {
  Write-Host "vcvars64.bat not found. Install Visual Studio 2022 Build Tools with the 'Desktop development with C++' workload, then rerun." -ForegroundColor Red
  exit 1
}

Write-Host "[build] using $vcvars"
$cmdline = "`"$vcvars`" >nul 2>&1 && cl /nologo /O2 /EHsc /MT /W3 `"$src`" /Fe:`"$out`" ws2_32.lib winhttp.lib"
cmd.exe /d /c $cmdline
if ($LASTEXITCODE -ne 0) {
  Write-Host "build failed (exit $LASTEXITCODE)" -ForegroundColor Red
  exit $LASTEXITCODE
}
$size = (Get-Item -LiteralPath $out).Length
Write-Host "[build] adapter.exe built: $([math]::Round($size / 1KB, 1)) KB"
