@echo off
setlocal EnableDelayedExpansion
title ChromeOS TCSS Mux Controller Installer

:: Check for administrative rights and self-elevate if needed
net session >nul 2>&1
if %errorLevel% neq 0 (
    echo [INFO] Requesting administrator privileges...
    powershell -NoProfile -Command "Start-Process cmd -ArgumentList '/c \"\"%~f0\"\"' -Verb RunAs"
    exit /b
)

cd /d "%~dp0"

echo ==========================================================
echo  ChromeOS TCSS Mux Controller Driver Installer
echo  The Scape Network - Leonardo (@isaiahscape)
echo ==========================================================
echo.

set CERTPATH=%~dp0TheScapeNetwork.cer
if not exist "%CERTPATH%" set CERTPATH=%~dp0x64\Release\CrosTcssMux\TheScapeNetwork.cer

set INFPATH=%~dp0CrosTcssMux.inf
if not exist "%INFPATH%" set INFPATH=%~dp0x64\Release\CrosTcssMux\CrosTcssMux.inf

if not exist "%INFPATH%" (
    echo [ERROR] Driver package files not found!
    echo Please make sure CrosTcssMux.inf is present or run build.cmd first.
    echo.
    pause
    exit /b 1
)

:: 1. Install certificate
if exist "%CERTPATH%" (
    echo [1/3] Installing The Scape Network security certificate...
    certutil -addstore -f "Root" "%CERTPATH%" >nul 2>&1
    certutil -addstore -f "TrustedPublisher" "%CERTPATH%" >nul 2>&1
    echo [OK] Certificate installed into Trusted Root and Trusted Publishers.
) else (
    echo [WARNING] TheScapeNetwork.cer not found. Skipping certificate store update.
)

:: 2. Install driver package via pnputil
echo.
echo [2/3] Installing driver package to system...
pnputil /add-driver "%INFPATH%" /install

:: 3. Check hardware status
echo.
echo [3/3] Checking ACPI\GOOG0014 hardware device status...
powershell -NoProfile -Command "Get-PnpDevice | Where-Object { $_.InstanceId -like '*GOOG0014*' } | Format-Table -Property InstanceId, Status, FriendlyName"

echo.
echo ==========================================================
echo  Installation complete!
echo ==========================================================
echo.
pause
