<#
Quick helper for configuring/building/cleaning this CMake project on Windows.

Usage examples:
  # Configure + build Debug (default)
  powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\project.ps1

  # Configure + build Release (maps to RelWithDebInfo to match CMakeSettings.json)
  powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\project.ps1 -Config Release

  # Only configure
  powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\project.ps1 -Action configure -Config Debug

  # Clean build outputs for a config
  powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\project.ps1 -Action clean -Config Debug

  # Delete build dir for a config (nukes cache + deps for that config)
  powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\project.ps1 -Action clobber -Config Debug

  # Delete the entire build/ directory
  powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\project.ps1 -Action clobberAll

Notes:
  - This uses the Ninja generator (same as your CMakeSettings.json).
  - For MSVC + Ninja, run from a "Developer PowerShell for VS" so cl.exe is on PATH.
#>

param(
  [ValidateSet("Debug", "Release")]
  [string]$Config = "Debug",

  [ValidateSet("configure", "build", "rebuild", "clean", "clobber", "clobberAll")]
  [string]$Action = "build",

  [string]$Generator = "Ninja",

  # Used for Visual Studio generators (ignored by Ninja).
  [string]$Architecture = "x64",

  [switch]$NoUpdateCompileCommands
)

$ErrorActionPreference = "Stop"

function Require-Command([string]$name, [string]$hint) {
  if (-not (Get-Command $name -ErrorAction SilentlyContinue)) {
    Write-Host "Missing required command '$name'." -ForegroundColor Red
    if ($hint) { Write-Host $hint -ForegroundColor Yellow }
    exit 2
  }
}

$repo = Split-Path -Parent $PSScriptRoot

# Map friendly config to your existing CMakeSettings.json names.
$configName = switch ($Config) {
  "Debug"   { "x64-Debug" }
  "Release" { "x64-Release" }
}

# Match your CMakeSettings.json: Release => RelWithDebInfo.
$cmakeBuildType = switch ($Config) {
  "Debug"   { "Debug" }
  "Release" { "RelWithDebInfo" }
}

$genSlug = ($Generator.ToLowerInvariant() -replace "[^a-z0-9]+", "-").Trim("-")
if ([string]::IsNullOrWhiteSpace($genSlug)) { $genSlug = "cmake" }

# Define this before using it to compute $buildDir.
$usesNinja = $Generator -match "(?i)\bninja\b"

# Keep existing folder layout for Ninja (matches CMakeSettings.json + existing repo builds).
# If you use a different generator, use a different build folder to avoid CMakeCache generator conflicts.
$buildDir = if ($usesNinja) {
  Join-Path $repo "build/vs_studio/$configName"
} else {
  Join-Path $repo "build/vs_studio/$configName-$genSlug"
}

Require-Command "cmake" "Install CMake and ensure it's on PATH."
if ($usesNinja) {
  Require-Command "ninja" "Install Ninja (or ensure it is on PATH), or pass -Generator ""Visual Studio 17 2022""."
}

if (-not (Get-Command "cl.exe" -ErrorAction SilentlyContinue)) {
  Write-Host "Warning: cl.exe not found on PATH. If configure/build fails, use 'Developer PowerShell for VS'." -ForegroundColor Yellow
}

function Configure() {
  New-Item -ItemType Directory -Force -Path $buildDir | Out-Null
  Write-Host "Configuring ($Config => $cmakeBuildType) in $buildDir" -ForegroundColor Cyan

  $args = @("-S", $repo, "-B", $buildDir, "-G", $Generator, "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON")

  # Single-config generators (Ninja/Makefiles) need CMAKE_BUILD_TYPE.
  # Multi-config generators (Visual Studio) use --config at build time.
  if ($Generator -match "(?i)visual studio") {
    $args += @("-A", $Architecture)
  } else {
    $args += "-DCMAKE_BUILD_TYPE=$cmakeBuildType"
  }

  & cmake @args
}

function Build([switch]$CleanFirst) {
  if (-not (Test-Path $buildDir)) {
    Configure
  }
  $args = @("--build", $buildDir)
  if ($Generator -match "(?i)visual studio") {
    $args += @("--config", $cmakeBuildType)
  }
  if ($CleanFirst) { $args += "--clean-first" }
  Write-Host "Building ($Config)..." -ForegroundColor Cyan
  & cmake @args
}

function Clean() {
  if (-not (Test-Path $buildDir)) {
    Write-Host "Nothing to clean: $buildDir doesn't exist." -ForegroundColor Yellow
    return
  }
  Write-Host "Cleaning ($Config)..." -ForegroundColor Cyan
  try {
    $args = @("--build", $buildDir)
    if ($Generator -match "(?i)visual studio") {
      $args += @("--config", $cmakeBuildType)
    }
    $args += @("--target", "clean")
    & cmake @args
  } catch {
    Write-Host "Clean target failed; you can use -Action clobber to delete the build directory." -ForegroundColor Yellow
    throw
  }
}

function ClobberConfig() {
  if (-not (Test-Path $buildDir)) {
    Write-Host "Nothing to delete: $buildDir doesn't exist." -ForegroundColor Yellow
    return
  }
  Write-Host "Deleting $buildDir" -ForegroundColor Cyan
  Remove-Item -Recurse -Force $buildDir
}

function ClobberAll() {
  $allBuild = Join-Path $repo "build"
  if (-not (Test-Path $allBuild)) {
    Write-Host "Nothing to delete: $allBuild doesn't exist." -ForegroundColor Yellow
    return
  }
  Write-Host "Deleting $allBuild" -ForegroundColor Cyan
  Remove-Item -Recurse -Force $allBuild
}

function Update-CompileCommands() {
  if ($NoUpdateCompileCommands) { return }

  # Preferred: copy from the active build directory (works no matter what buildDir is).
  $src = Join-Path $buildDir "compile_commands.json"
  $dstDir = Join-Path $repo "build"
  $dst = Join-Path $dstDir "compile_commands.json"
  if (Test-Path $src) {
    if (!(Test-Path $dstDir)) { New-Item -ItemType Directory -Path $dstDir | Out-Null }
    Copy-Item -LiteralPath $src -Destination $dst -Force
    Write-Host "Updated $dst" -ForegroundColor Green
    Write-Host "Source:  $src"
    return
  }

  # Fallback: use the dedicated helper (may be useful for legacy layouts).
  $updateScript = Join-Path $repo "scripts/update-compile-commands.ps1"
  if (Test-Path $updateScript) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File $updateScript -Config $configName | Out-Host
  }
}

switch ($Action) {
  "configure" { Configure; Update-CompileCommands; break }
  "build"     { Configure; Build; Update-CompileCommands; break }
  "rebuild"   { Configure; Build -CleanFirst; Update-CompileCommands; break }
  "clean"     { Clean; break }
  "clobber"   { ClobberConfig; break }
  "clobberAll"{ ClobberAll; break }
}

