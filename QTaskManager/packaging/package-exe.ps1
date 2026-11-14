# Build QTaskManager and zip a runnable Windows folder.
# Requires MSVC, CMake, and Qt 6 (Widgets + Svg) on PATH or in -QtPrefix.
param(
    [string]$QtPrefix = $env:CMAKE_PREFIX_PATH
)

$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
Set-Location $Root

$cmakeArgs = @("-S", ".", "-B", "build")
if ($QtPrefix) {
    $cmakeArgs += "-DCMAKE_PREFIX_PATH=$QtPrefix"
}

cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

cmake --build build --config Release --target monitoring-package
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$packageDir = Join-Path $Root "build\monitoring-package"
if (-not (Test-Path (Join-Path $packageDir "QTaskManager.exe"))) {
    Write-Error "QTaskManager.exe was not produced in $packageDir"
}

$outDir = Join-Path $Root "packaging\output"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$zip = Join-Path $outDir "QTaskManager-win64.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $packageDir "*") -DestinationPath $zip
Write-Host "Packaged $zip"
Get-ChildItem $packageDir | Select-Object Name, Length | Format-Table -AutoSize
