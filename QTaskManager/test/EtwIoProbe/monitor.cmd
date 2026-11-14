@echo off
setlocal
cd /d "%~dp0"
if not exist build\EtwIoProbe.exe (
    echo ERROR: build\EtwIoProbe.exe not found. Run build.cmd first.
    exit /b 1
)

net session >nul 2>nul
if errorlevel 1 (
    powershell.exe -NoProfile -Command "Start-Process -FilePath '%~dp0build\EtwIoProbe.exe' -ArgumentList '--monitor 30' -Verb RunAs -WorkingDirectory '%~dp0'"
    exit /b 0
)

build\EtwIoProbe.exe --monitor 30
