@echo off
setlocal
cd /d "%~dp0"

where cl.exe >nul 2>nul
if errorlevel 1 (
    echo ERROR: cl.exe is not available.
    echo Run this from an x64 Developer Command Prompt for Visual Studio.
    exit /b 1
)

if not exist build mkdir build

echo Building EtwIoProbe.exe...
cl.exe /nologo /std:c++20 /EHsc /O2 /W4 /permissive- /DUNICODE /D_UNICODE ^
    /Fe:build\EtwIoProbe.exe src\EtwIoProbe.cpp ^
    /link advapi32.lib tdh.lib winhttp.lib ws2_32.lib
if errorlevel 1 (
    echo.
    echo BUILD FAILED.
    exit /b 1
)

echo.
echo BUILD OK: %CD%\build\EtwIoProbe.exe
exit /b 0
