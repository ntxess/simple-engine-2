param(
  [Parameter(Mandatory = $false)]
  [string]$Config = "x64-Debug"
)

$ErrorActionPreference = "Stop"

$repo = Split-Path -Parent $PSScriptRoot
$src = Join-Path $repo "build/vs_studio/$Config/compile_commands.json"
$dstDir = Join-Path $repo "build"
$dst = Join-Path $dstDir "compile_commands.json"

if (!(Test-Path $src)) {
  Write-Host "compile_commands.json not found for config '$Config'." -ForegroundColor Red
  Write-Host "Expected: $src" -ForegroundColor Red
  Write-Host ""
  Write-Host "Fix: configure/build that CMake config once so CMake generates compile_commands.json." -ForegroundColor Yellow
  exit 2
}

if (!(Test-Path $dstDir)) {
  New-Item -ItemType Directory -Path $dstDir | Out-Null
}

Copy-Item -LiteralPath $src -Destination $dst -Force
Write-Host "Updated $dst" -ForegroundColor Green
Write-Host "Source:  $src"

