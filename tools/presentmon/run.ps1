<#
.SYNOPSIS
  Captures rampage with PresentMon and summarises the capture with analyze.py.

.DESCRIPTION
  The PresentMon gates of docs/gates.md (M6): displayed-time latency,
  dropped and repeated frames of rampage in a pacing mode. PresentMon is
  not installed on the development machine and may not be downloaded by
  the agents; install the console build of PresentMon 2.x
  (https://github.com/GameTechDev/PresentMon/releases) and pass its path
  with -PresentMon, or put PresentMon.exe on PATH. PresentMon needs an
  elevated shell (or membership of "Performance Log Users").

  The script starts rampage (--pacing MODE, without taking focus), lets it
  settle for 2 s, records -Seconds with PresentMon's v1 metric columns, then
  closes rampage's window (the two-phase quit) and runs analyze.py on the
  CSV. Its exit status is analyze.py's (1 when a -Max* limit is exceeded).

.EXAMPLE
  tools/presentmon/run.ps1 -Mode paced -Seconds 20 -MaxP99Ms 25
  tools/presentmon/run.ps1 -Mode vrr -PresentMon C:\Tools\PresentMon.exe
#>
param(
  [ValidateSet('paced', 'standard', 'vrr', 'auto')]
  [string]$Mode = 'paced',
  [int]$Seconds = 20,
  [string]$PresentMon = 'PresentMon.exe',
  [string]$Rampage = '',
  [string]$Out = '',
  [double]$RefreshHz = 0,
  [double]$MaxP99Ms = -1,
  [int]$MaxDropped = -1,
  [double]$MaxRepeatedPercent = -1
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot\..\..").Path
if (-not $Rampage) { $Rampage = Join-Path $repo 'out\build\msvc-release\bin\rampage.exe' }
if (-not $Out) { $Out = Join-Path $repo 'out\presentmon' }
$tool = Get-Command $PresentMon -ErrorAction SilentlyContinue
if (-not $tool) {
  [Console]::Error.WriteLine("run.ps1: PresentMon not found ('$PresentMon'): install it (docs/gates.md, PresentMon) and pass -PresentMon PATH")
  exit 2
}
if (-not (Test-Path $Rampage)) {
  [Console]::Error.WriteLine("run.ps1: rampage not found at $Rampage (build msvc-release first: scripts/ci.ps1 -Presets msvc-release)")
  exit 2
}
New-Item -ItemType Directory -Force $Out | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$csv = Join-Path $Out "rampage-$Mode-$stamp.csv"
$log = Join-Path $Out "rampage-$Mode-$stamp.log"

$game = Start-Process -FilePath $Rampage -PassThru -RedirectStandardError $log `
  -ArgumentList @('--pacing', $Mode, '--no-activate', '--return-on-quit')
try {
  Start-Sleep -Seconds 2
  & $tool.Source --process_name 'rampage.exe' --output_file $csv `
    --timed $Seconds --terminate_after_timed --v1_metrics --no_console_stats
  if ($LASTEXITCODE -ne 0) {
    [Console]::Error.WriteLine("run.ps1: PresentMon exited with $LASTEXITCODE")
    exit 2
  }
} finally {
  if (-not $game.HasExited) {
    # The window's close request runs the two-phase quit.
    $null = $game.CloseMainWindow()
    if (-not $game.WaitForExit(10000)) { $game.Kill() }
  }
}
Write-Host "capture: $csv"
Write-Host "rampage log: $log"
Select-String -Path $log -Pattern 'pacing: |pacing at shutdown' | ForEach-Object { Write-Host $_.Line }

$analyze = @((Join-Path $PSScriptRoot 'analyze.py'), $csv, '--process', 'rampage.exe')
if ($RefreshHz -gt 0) { $analyze += @('--refresh-hz', $RefreshHz) }
if ($MaxP99Ms -ge 0) { $analyze += @('--max-p99-ms', $MaxP99Ms) }
if ($MaxDropped -ge 0) { $analyze += @('--max-dropped', $MaxDropped) }
if ($MaxRepeatedPercent -ge 0) { $analyze += @('--max-repeated-percent', $MaxRepeatedPercent) }
& python @analyze
exit $LASTEXITCODE
