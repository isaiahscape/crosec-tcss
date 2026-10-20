@echo off
REM =========================================================================
REM build.cmd — Automated build script for CrosTcssMux.sys
REM
REM Prerequisites:
REM   - Windows Driver Kit (WDK) 10.0.22621.0 or newer
REM   - Visual Studio 2022 or later with C++ desktop workload
REM   - KMDF 1.33 (ships with WDK for Windows 11 22H2+)
REM
REM Usage:
REM   build.cmd [Debug|Release]
REM =========================================================================

setlocal enabledelayedexpansion

set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Release

echo.
echo ====================================================
echo  CrosTcssMux.sys Build Script
echo  Configuration: %CONFIG%
echo  Platform: x64
echo ====================================================
echo.

REM --- Locate Visual Studio installation ---
set VSWHERE="%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"

if exist %VSWHERE% (
    for /f "usebackq tokens=*" %%i in (`%VSWHERE% -latest -property installationPath`) do (
        set VSINSTALLDIR=%%i
    )
)

if not defined VSINSTALLDIR (
    echo ERROR: Visual Studio installation not found.
    echo Please install Visual Studio 2022 with C++ desktop workload and WDK.
    exit /b 1
)

echo Found Visual Studio at: %VSINSTALLDIR%

REM --- Setup build environment ---
if exist "%VSINSTALLDIR%\VC\Auxiliary\Build\vcvars64.bat" (
    call "%VSINSTALLDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
) else (
    echo ERROR: vcvars64.bat not found.
    exit /b 1
)

REM --- Verify WDK is available ---
where /q msbuild.exe
if errorlevel 1 (
    echo ERROR: msbuild.exe not found in PATH after vcvars setup.
    exit /b 1
)

REM --- Prepare log file ---
for /f "usebackq" %%a in (`powershell -NoProfile -Command "Get-Date -Format 'yyyyMMdd_HHmmss'"`) do set TIMESTAMP=%%a
set LOGFILE=build_error_%TIMESTAMP%.log

REM --- Build ---
echo.
echo Building CrosTcssMux.sys (%CONFIG% x64)...
echo.

msbuild CrosTcssMux.vcxproj ^
    /p:Configuration=%CONFIG% ^
    /p:Platform=x64 ^
    /p:SignMode=Off ^
    /verbosity:normal ^
    /nologo ^
    /fl /flp:logfile=%LOGFILE%;verbosity=diagnostic

if errorlevel 1 (
    echo.
    echo ====================================================
    echo  BUILD FAILED
    echo  Error log saved to: %LOGFILE%
    echo ====================================================
    echo.
    echo Last 30 error lines:
    echo ----------------------------------------------------
    findstr /i /c:"error " /c:"fatal" /c:"cannot open" /c:"unresolved" %LOGFILE% 2>nul | more +0 /e /p
    echo ----------------------------------------------------
    exit /b 1
)

REM --- Build succeeded, clean up the log ---
if exist "%LOGFILE%" del "%LOGFILE%"

echo.
echo ====================================================
echo  BUILD SUCCEEDED
echo ====================================================
echo.

REM --- Test-sign driver package ---
set PKGDIR=x64\%CONFIG%\CrosTcssMux
set SIGNTOOL="C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe"
set INF2CAT="C:\Program Files (x86)\Windows Kits\10\bin\10.0.28000.0\x86\Inf2Cat.exe"

if exist "%PKGDIR%\CrosTcssMux.sys" (
    echo Signing driver package with test certificate...
    %SIGNTOOL% sign /v /s My /n "CrosTcssMuxTestCert" /fd SHA256 "%PKGDIR%\CrosTcssMux.sys" >nul 2>&1
    if exist %INF2CAT% (
        %INF2CAT% /driver:"%PKGDIR%" /os:10_X64 >nul 2>&1
        %SIGNTOOL% sign /v /s My /n "CrosTcssMuxTestCert" /fd SHA256 "%PKGDIR%\crostcssmux.cat" >nul 2>&1
    )
    echo Driver package signed successfully.
)

REM --- Show output location ---
set OUTDIR=x64\%CONFIG%
if exist "%OUTDIR%\CrosTcssMux.sys" (
    echo.
    echo Output files:
    echo   %OUTDIR%\CrosTcssMux.sys
    echo   %OUTDIR%\CrosTcssMux.inf
    echo   %OUTDIR%\CrosTcssMux.pdb
    echo   %PKGDIR%\CrosTcssMuxTestCert.cer
    echo.
    echo To install:
    echo   Right-click install-driver.cmd and select "Run as administrator"
)

exit /b 0
