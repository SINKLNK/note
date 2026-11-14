param()

$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
Set-Location $Root

nuget restore .\packages.config -PackagesDirectory .\packages\
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

cmd /c "build\build_init.cmd"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

cmd /c "build\build_release.cmd"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$release = Join-Path $Root "build\output\Release64"
$exe = Join-Path $release "SystemInformer.exe"
if (-not (Test-Path $exe)) {
    throw "SystemInformer.exe was not produced at $exe"
}

$stageParent = Join-Path ([System.IO.Path]::GetTempPath()) "systeminformer-package"
$stage = Join-Path $stageParent "SystemInformer"
if (Test-Path $stageParent) { Remove-Item $stageParent -Recurse -Force }
New-Item -ItemType Directory -Force -Path $stage | Out-Null

$skip = @(".pdb", ".iobj", ".ipdb", ".exp", ".lib")
Get-ChildItem $release -Recurse -File | Where-Object { $skip -notcontains $_.Extension } | ForEach-Object {
    $relative = $_.FullName.Substring($release.Length).TrimStart("\")
    $destination = Join-Path $stage $relative
    $destinationDir = Split-Path $destination -Parent
    if (-not (Test-Path $destinationDir)) {
        New-Item -ItemType Directory -Force -Path $destinationDir | Out-Null
    }
    Copy-Item $_.FullName $destination
}

$outDir = Join-Path $Root "packaging\output"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$zip = Join-Path $outDir "SystemInformer-win64.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path $stage -DestinationPath $zip

Write-Host "Packaged $zip"
Get-Item $exe, $zip | Format-Table Name, Length -AutoSize
