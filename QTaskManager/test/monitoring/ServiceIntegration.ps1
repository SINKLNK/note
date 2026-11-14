param(
    [ValidateSet('Status','Install','Lifecycle','Etw','Uninstall')][string]$Scenario = 'Status',
    [string]$BuildDirectory = (Join-Path $PSScriptRoot '../../build-monitoring'),
    [switch]$AllowSystemChanges
)
$ErrorActionPreference = 'Stop'
$build = (Resolve-Path -LiteralPath $BuildDirectory).Path
$resultDirectory = Join-Path $PSScriptRoot 'results'
New-Item -ItemType Directory -Force -Path $resultDirectory | Out-Null
$log = Join-Path $resultDirectory ((Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + $Scenario + '.txt')
$results = [System.Collections.Generic.List[string]]::new()
function Record([string]$State,[string]$Message) { $line = "$State $Message"; $results.Add($line); Write-Output $line }
try {
    if ($Scenario -in 'Install','Uninstall') {
        if (!$AllowSystemChanges) { throw 'This scenario requires explicit -AllowSystemChanges consent.' }
        $action = if ($Scenario -eq 'Install') { 0 } else { 3 }
        $setup = Start-Process -FilePath (Join-Path $build 'QTaskManagerSetup.exe') -ArgumentList "--action $action" -Verb RunAs -Wait -PassThru -WindowStyle Hidden
        if ($setup.ExitCode -ne 0) { throw "Setup exit code $($setup.ExitCode)" }
    }
    $service = Get-Service -Name QTaskManagerService -ErrorAction SilentlyContinue
    if ($Scenario -eq 'Uninstall') {
        if ($service) { throw 'Service is still registered (possibly pending deletion).' }
        Record PASS 'Service removed. PawnIO was not requested for removal.'
    } elseif (!$service) { throw 'Service is not installed.' }
    else {
        Record PASS "SCM: $($service.Status), start type $($service.StartType)"
        if ($Scenario -in 'Lifecycle','Etw') {
            & (Join-Path $build 'MonitorProbe.exe') 10 | Add-Content -LiteralPath $log
            if ($LASTEXITCODE -ne 0) { throw "Probe failed: $LASTEXITCODE" }
            Record PASS 'Authenticated handshake and fresh snapshots received.'
            Record SKIPPED 'Exact disk/TCP/UDP IPv4/IPv6 attribution requires a separately controlled workload; receiving snapshots does not validate byte counts.'
        }
        Record SKIPPED 'Boot/resume, another account, UAC cancellation, occupied-file rollback and global ETW loss require interactive system validation.'
    }
} catch { Record FAIL $_.Exception.Message; $results | Add-Content -LiteralPath $log; exit 1 }
$results | Add-Content -LiteralPath $log
