param(
    [ValidateRange(10,300)][int]$Seconds = 60,
    [ValidateSet('Idle','Build','Copy','Network')][string]$Scenario = 'Idle',
    [string]$BuildDirectory = (Join-Path $PSScriptRoot '../../build-monitoring')
)
$ErrorActionPreference = 'Stop'
$build = (Resolve-Path -LiteralPath $BuildDirectory).Path
if (Get-Process QTaskManager -ErrorAction SilentlyContinue) { throw 'Close every GUI first; the off phase must have no monitoring clients.' }
if ((Get-Service QTaskManagerService).Status -ne 'Running') { throw 'Start the installed service first.' }
if ($Scenario -ne 'Idle') { throw 'Automated Build/Copy/Network workloads are not implemented yet. Do not label an idle run as a loaded benchmark.' }
$directory = Join-Path $PSScriptRoot 'results'
New-Item -ItemType Directory -Force -Path $directory | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$rows = [System.Collections.Generic.List[object]]::new()
for ($pair = 1; $pair -le 3; $pair++) {
    foreach ($mode in 'off','on') {
        $probe = $null
        try {
            if ($mode -eq 'off') { Start-Sleep -Seconds 6 }
            else { $probe = Start-Process (Join-Path $build 'MonitorProbe.exe') -ArgumentList ($Seconds + 3) -PassThru -WindowStyle Hidden -RedirectStandardOutput (Join-Path $directory "$stamp-$pair-etw.jsonl") }
            $before = Get-Process QTaskManagerService
            $cpuBefore = $before.TotalProcessorTime.TotalSeconds
            $timer = [Diagnostics.Stopwatch]::StartNew()
            Start-Sleep -Seconds $Seconds
            $after = Get-Process QTaskManagerService
            $rows.Add([pscustomobject]@{Pair=$pair;Mode=$mode;Scenario=$Scenario;Elapsed=$timer.Elapsed.TotalSeconds;CpuSeconds=$after.TotalProcessorTime.TotalSeconds-$cpuBefore;PrivateBytes=$after.PrivateMemorySize64;WorkingSet=$after.WorkingSet64})
        } finally { if ($probe) { $probe.WaitForExit(); if ($probe.ExitCode -ne 0) { throw "Probe failed: $($probe.ExitCode)" } } }
    }
}
$rows | Export-Csv -NoTypeInformation -LiteralPath (Join-Path $directory "$stamp-overhead.csv")
$rows | Format-Table
Write-Output 'Service-only idle measurements. GUI/system CPU and workload throughput have NOT been measured.'
