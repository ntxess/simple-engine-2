<#
.SYNOPSIS
  Local CI: configures, builds and tests the given workflow presets.

.DESCRIPTION
  Nothing is pushed from this repository, so this script is the CI. Each
  workflow preset runs inside the MSVC 14.51 environment (scripts/with-msvc.cmd).
  Linux presets run in WSL through scripts/wsl-build.sh.

.EXAMPLE
  scripts/ci.ps1                       # commit gate: msvc-dev, msvc-asan, clangcl-dev
  scripts/ci.ps1 -Milestone            # adds msvc-debug, msvc-release and WSL TSan/ASan
  scripts/ci.ps1 -Presets msvc-dev     # one preset
  scripts/ci.ps1 -Presets legacy-msvc-debug,legacy-msvc-release
                                       # the legacy reference (build + "legacy" tests)
  scripts/ci.ps1 -Legacy               # appends both legacy presets to the selection
#>
param(
  [string[]]$Presets = @('msvc-dev', 'msvc-asan', 'clangcl-dev'),
  [switch]$Milestone,
  [switch]$Legacy
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
# The gate checks goldens and never rewrites them: with SE_UPDATE_GOLDENS=1 left
# in the shell, every golden test would re-baseline its golden and pass.
if (Test-Path Env:SE_UPDATE_GOLDENS) {
  Write-Warning 'SE_UPDATE_GOLDENS is set; ci.ps1 clears it so that the golden tests check their goldens.'
  Remove-Item Env:SE_UPDATE_GOLDENS
}
# powershell -File passes "a,b" as one string; accept both forms.
$Presets = @($Presets | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
if ($Milestone) {
  $Presets = @('msvc-debug', 'msvc-dev', 'msvc-release', 'msvc-asan', 'clangcl-dev',
               'wsl:linux-clang-tsan', 'wsl:linux-clang-asan')
}
if ($Legacy) {
  $Presets = @($Presets) + @('legacy-msvc-debug', 'legacy-msvc-release')
}
$failed = @()
foreach ($preset in $Presets) {
  Write-Host "=== $preset ===" -ForegroundColor Cyan
  if ($preset.StartsWith('wsl:')) {
    $name = $preset.Substring(4)
    wsl.exe -- bash -lc "~/se-wsl/scripts/wsl-build.sh $name"
  } else {
    & cmd.exe /c "`"$repo\scripts\with-msvc.cmd`" cmake --workflow --preset $preset"
  }
  if ($LASTEXITCODE -ne 0) { $failed += $preset }
}
if ($failed.Count -gt 0) {
  Write-Host "FAILED: $($failed -join ', ')" -ForegroundColor Red
  exit 1
}
Write-Host "All presets passed: $($Presets -join ', ')" -ForegroundColor Green
