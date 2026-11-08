<#
.SYNOPSIS
  One measured KytyPS5 session for the performance lab (Windows).

.DESCRIPTION
  Starts the launcher of the given build with KYTY_PERF_CSV pointing at a new CSV in -OutDir,
  plus any extra environment switches (for A/B runs of one binary). The emulator inherits the
  environment from the launcher. If -PresentMon is given, PresentMon records the emulator's
  presents for -Seconds after -Delay seconds, independently of the emulator's own counters.

  Nothing is written outside -OutDir. The build folder, saves and game files are not touched.

.EXAMPLE
  .\run-session.ps1 -Build D:\KytyPS5_Optimization\builds\official -Label official-wall-1 `
      -OutDir D:\KytyPS5_Optimization\runs -PresentMon C:\Tools\PresentMon.exe

.EXAMPLE
  # Same binary, one optimization switched off:
  .\run-session.ps1 -Build D:\KytyPS5_Optimization\builds\perf -Label perf-nowake-wall-1 `
      -OutDir D:\KytyPS5_Optimization\runs -Env @{ KYTY_CP_WAKEUP = '0' }
#>
param(
    [Parameter(Mandatory)] [string] $Build,
    [Parameter(Mandatory)] [string] $Label,
    [Parameter(Mandatory)] [string] $OutDir,
    [hashtable] $Env = @{},
    [string] $PresentMon = '',
    [int] $Delay = 60,
    [int] $Seconds = 60,
    [int] $IntervalMs = 1000,
    [switch] $NoCounters
)

$ErrorActionPreference = 'Stop'
$launcher = Join-Path $Build 'launcher.exe'
if (-not (Test-Path $launcher)) { throw "launcher.exe not found in $Build" }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$base = Join-Path $OutDir "$stamp-$Label"
$csv = "$base.kyty.csv"

$saved = @{}
$set = @{}
if (-not $NoCounters) {
    $set['KYTY_PERF_CSV'] = $csv
    $set['KYTY_PERF_CSV_INTERVAL_MS'] = "$IntervalMs"
}
foreach ($k in $Env.Keys) { $set[$k] = [string]$Env[$k] }
foreach ($k in $set.Keys) {
    $saved[$k] = [Environment]::GetEnvironmentVariable($k, 'Process')
    [Environment]::SetEnvironmentVariable($k, $set[$k], 'Process')
}

# Record what this run was, so results stay attributable.
@{
    label = $Label; build = (Resolve-Path $Build).Path; started = (Get-Date).ToString('o')
    environment = $set; presentmon_delay_s = $Delay; presentmon_seconds = $Seconds
} | ConvertTo-Json | Set-Content "$base.run.json"

try {
    Write-Host "Starting $launcher"
    Write-Host "Counters: $(if ($NoCounters) { 'off' } else { $csv })"
    $proc = Start-Process -FilePath $launcher -WorkingDirectory $Build -PassThru

    if ($PresentMon) {
        Write-Host "Load the save and walk to the test position. PresentMon starts in $Delay s" `
            "and records $Seconds s: keep the camera still for the whole capture."
        & $PresentMon --process_name kyty_emulator.exe --output_file "$base.presentmon.csv" `
            --delay $Delay --timed $Seconds --terminate_after_timed --no_console_stats | Out-Null
        Write-Host "PresentMon capture written to $base.presentmon.csv"
    }
    Write-Host 'Close the emulator when finished.'
    $proc.WaitForExit()
}
finally {
    foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k], 'Process') }
}

Write-Host "Done: $base.*"
Write-Host "Analyze: python tools\perf-lab\analyze_perf_csv.py `"$csv`" --from <t0> --to <t1>"
