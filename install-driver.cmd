@echo off
setlocal
echo ====================================================
echo  CrosTcssMux Driver & Certificate Installer
echo ====================================================
echo.

:: Check for administrative privileges
net session >nul 2>&1
if %errorLevel% neq 0 (
    echo [ERROR] This script must be run as Administrator!
    echo Please right-click install-driver.cmd and select "Run as administrator".
    echo.
    pause
    exit /b 1
)

set CERTPATH=%~dp0x64\Release\CrosTcssMux\CrosTcssMuxTestCert.cer
set INFPATH=%~dp0x64\Release\CrosTcssMux\CrosTcssMux.inf

if not exist "%CERTPATH%" (
    echo [ERROR] Certificate not found at: %CERTPATH%
    echo Please run build.cmd first.
    pause
    exit /b 1
)

echo [1/3] Adding test certificate to Trusted Root and Trusted Publishers...
certutil -addstore -f "Root" "%CERTPATH%" >nul
certutil -addstore -f "TrustedPublisher" "%CERTPATH%" >nul
if %errorLevel% neq 0 (
    echo [ERROR] Failed to add certificate to stores.
    pause
    exit /b 1
)
echo Certificate installed successfully.

echo.
echo [2/3] Installing driver package to system...
pnputil /add-driver "%INFPATH%" /install

echo.
echo [3/3] Checking device status...
powershell -NoProfile -Command "Get-PnpDevice | Where-Object { $_.InstanceId -like '*GOOG0014*' } | Format-Table InstanceId, Status, FriendlyName"

echo.
echo ====================================================
echo Done!
echo ====================================================
pause
