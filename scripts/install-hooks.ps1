<#
.SYNOPSIS
  Installs an optional pre-commit hook that runs the lint gate on new code.

.DESCRIPTION
  The hook runs clang-format (check only), cpplint and the source checks
  before each commit. clang-tidy needs a build tree, so it runs in
  scripts/ci.ps1 instead. Remove .git/hooks/pre-commit to uninstall.
#>
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$hook = Join-Path $repo '.git/hooks/pre-commit'
@'
#!/usr/bin/env sh
# simple-engine-2 lint gate (installed by scripts/install-hooks.ps1)
python tools/lint/run_lint.py clang-format || exit 1
python tools/lint/run_lint.py cpplint || exit 1
'@ | Set-Content -Path $hook -Encoding ascii -NoNewline
Write-Host "Installed $hook"
