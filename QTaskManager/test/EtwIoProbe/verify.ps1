$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $root

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
$isAdmin = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

if (-not $isAdmin) {
    Write-Host 'ETW kernel tracing needs elevation. Requesting UAC...'
    $args = @(
        '-NoProfile',
        '-ExecutionPolicy', 'Bypass',
        '-File', ('"' + $MyInvocation.MyCommand.Path + '"')
    )
    $p = Start-Process -FilePath 'powershell.exe' -ArgumentList $args -Verb RunAs -Wait -PassThru -WorkingDirectory $root
    exit $p.ExitCode
}

$exe = Join-Path $root 'build\EtwIoProbe.exe'
if (-not (Test-Path $exe)) {
    Write-Host 'ERROR: build\EtwIoProbe.exe not found.' -ForegroundColor Red
    Write-Host 'Run build.cmd first from an x64 Visual Studio Developer Command Prompt.'
    exit 1
}

$report = Join-Path $root 'report.txt'
Write-Host 'Running ETW self-test. This writes a 512 MiB temporary unbuffered file and downloads 64 MiB.'
Write-Host 'The temporary disk file is deleted automatically.'
Write-Host ''

$oldEncoding = [Console]::OutputEncoding
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)
try {
    & $exe --selftest --disk-mib 512 --network-mib 64 2>&1 | Tee-Object -FilePath $report
    $code = $LASTEXITCODE
}
finally {
    [Console]::OutputEncoding = $oldEncoding
}

Write-Host ''
Write-Host "Report saved to: $report"
Write-Host 'Send me report.txt (or paste its contents).'
Write-Host ''
if ($code -eq 0) {
    Write-Host 'Self-test exit code: 0 (PASS)' -ForegroundColor Green
} elseif ($code -eq 2) {
    Write-Host 'Self-test exit code: 2 (PARTIAL / one validation inconclusive)' -ForegroundColor Yellow
} else {
    Write-Host "Self-test exit code: $code (FAIL)" -ForegroundColor Red
}

Read-Host 'Press Enter to close' | Out-Null
exit $code
