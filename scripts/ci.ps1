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
                                       # (built before the engine presets, which
                                       # run the legacy build in a test)
  scripts/ci.ps1 -Desktop              # the desktop label (real windows on this
                                       # desktop) on msvc-dev and msvc-asan, and
                                       # pacing.unpatched_standard in
                                       # msvc-unpatched-dev; with -Presets or
                                       # -Legacy, after those
#>
param(
  [string[]]$Presets = @('msvc-dev', 'msvc-asan', 'clangcl-dev'),
  [switch]$Milestone,
  [switch]$Legacy,
  [switch]$Desktop
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
# The gate checks goldens and never rewrites them: with SE_UPDATE_GOLDENS=1 left
# in the shell, every golden test would re-baseline its golden and pass.
if (Test-Path Env:SE_UPDATE_GOLDENS) {
  Write-Warning 'SE_UPDATE_GOLDENS is set; ci.ps1 clears it so that the golden tests check their goldens.'
  Remove-Item Env:SE_UPDATE_GOLDENS
}
# SDL's assertion dialog would wait for a click in an unattended run: a
# failed SDL assertion aborts instead (ADR-070; the engine with
# SE_NO_MESSAGE_BOX=1, the test executables and render_shot also install a
# handler that prints and aborts).
$env:SDL_ASSERT = 'abort'
# Nothing a test runs may be heard on this machine's speakers: every App
# mixes on miniaudio's null backend (M8; the one real-device test opens the
# device at volume 0 by itself).
$env:SE_AUDIO = 'null'
# powershell -File passes "a,b" as one string; accept both forms.
$Presets = @($Presets | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
if ($Milestone) {
  $Presets = @('msvc-debug', 'msvc-dev', 'msvc-release', 'msvc-asan', 'clangcl-dev',
               'wsl:linux-clang-tsan', 'wsl:linux-clang-asan')
}
if ($Legacy) {
  $Presets = @($Presets) + @('legacy-msvc-debug', 'legacy-msvc-release')
}
# Desktop tests (label desktop) open real windows, so the commit-gate test
# presets exclude them; -Desktop runs them on msvc-dev and msvc-asan, and
# -Milestone includes them. Alone, -Desktop runs nothing else.
$desktopPresets = @()
if ($Desktop -or $Milestone) {
  $desktopPresets = @('msvc-dev', 'msvc-asan')
  if ($Desktop -and -not $Milestone -and -not $Legacy -and
      -not $PSBoundParameters.ContainsKey('Presets')) {
    $Presets = @()
  }
}
function ConvertTo-WslPath([string]$Path) {
  $full = [System.IO.Path]::GetFullPath($Path)
  '/mnt/' + $full.Substring(0, 1).ToLowerInvariant() + ($full.Substring(2) -replace '\\', '/')
}
$failed = @()
# Engine and legacy builds test each other: render.legacy_image.* runs the
# legacy build's legacy_replay, and legacy.image_compare.* runs the engine
# build's render_shot. Each must see the other built from this checkout, not
# from an older one, so with both kinds selected the legacy presets are
# built (not tested) first; their workflows then test after the engine's.
$legacySelected = @($Presets | Where-Object { $_.StartsWith('legacy-') })
$engineSelected = @($Presets | Where-Object {
  -not $_.StartsWith('legacy-') -and -not $_.StartsWith('wsl:') })
if ($legacySelected.Count -gt 0 -and $engineSelected.Count -gt 0) {
  foreach ($preset in $legacySelected) {
    Write-Host "=== build first: $preset ===" -ForegroundColor Cyan
    $msvc = "`"$repo\scripts\with-msvc.cmd`""
    & cmd.exe /c "$msvc cmake --preset $preset && $msvc cmake --build --preset $preset"
    if ($LASTEXITCODE -ne 0) { $failed += "build:$preset" }
  }
}
foreach ($preset in $Presets) {
  Write-Host "=== $preset ===" -ForegroundColor Cyan
  if ($preset.StartsWith('wsl:')) {
    # WSL builds the committed tip of this checkout's branch. A worktree's
    # branch is fetched from the main repository (worktrees share its refs)
    # into a clone of its own, so parallel worktrees do not collide.
    $name = $preset.Substring(4)
    $branch = (git -C $repo rev-parse --abbrev-ref HEAD).Trim()
    $common = (git -C $repo rev-parse --path-format=absolute --git-common-dir).Trim()
    $main = Split-Path -Parent $common
    $wslMain = ConvertTo-WslPath $main
    $clone = '~/se-wsl'
    if ([System.IO.Path]::GetFullPath($main) -ne [System.IO.Path]::GetFullPath($repo)) {
      $clone = "~/se-wsl-$($branch -replace '[^A-Za-z0-9._-]', '-')"
    }
    wsl.exe -d Ubuntu -- bash -lc "SE_WIN_REPO=$wslMain SE_BRANCH=$branch SE_WSL_CLONE=$clone bash $wslMain/scripts/wsl-build.sh $name"
  } else {
    & cmd.exe /c "`"$repo\scripts\with-msvc.cmd`" cmake --workflow --preset $preset"
  }
  if ($LASTEXITCODE -ne 0) { $failed += $preset }
}
foreach ($preset in $desktopPresets) {
  Write-Host "=== desktop: $preset ===" -ForegroundColor Cyan
  # No message box may wait for a click during a desktop test.
  $env:SE_NO_MESSAGE_BOX = '1'
  $msvc = "`"$repo\scripts\with-msvc.cmd`""
  $tree = "`"$repo\out\build\$preset`""
  & cmd.exe /c "$msvc cmake --preset $preset && $msvc cmake --build --preset $preset && $msvc ctest --test-dir $tree -L desktop --output-on-failure"
  if ($LASTEXITCODE -ne 0) { $failed += "desktop:$preset" }
}
# The M6 exit "rampage runs in Standard unpatched" needs a window, and the
# msvc-unpatched-dev test preset excludes the desktop label: run that one
# desktop test in the unpatched tree (it skips in the patched ones).
$unpatchedDesktop = @()
if ($desktopPresets.Count -gt 0) {
  $unpatchedDesktop = @('msvc-unpatched-dev')
  Write-Host "=== desktop: msvc-unpatched-dev (pacing.unpatched_standard) ===" -ForegroundColor Cyan
  $env:SE_NO_MESSAGE_BOX = '1'
  $msvc = "`"$repo\scripts\with-msvc.cmd`""
  $tree = "`"$repo\out\build\msvc-unpatched-dev`""
  & cmd.exe /c "$msvc cmake --preset msvc-unpatched-dev && $msvc cmake --build --preset msvc-unpatched-dev && $msvc ctest --test-dir $tree -L desktop -R pacing[.]unpatched_standard --no-tests=error --output-on-failure"
  if ($LASTEXITCODE -ne 0) { $failed += 'desktop:msvc-unpatched-dev' }
}
if ($failed.Count -gt 0) {
  Write-Host "FAILED: $($failed -join ', ')" -ForegroundColor Red
  exit 1
}
$passed = @($Presets) + @(@($desktopPresets) + @($unpatchedDesktop) |
  ForEach-Object { "desktop:$_" })
Write-Host "All presets passed: $($passed -join ', ')" -ForegroundColor Green
