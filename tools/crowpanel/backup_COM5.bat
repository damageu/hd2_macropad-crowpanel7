@echo off
setlocal EnableExtensions
title HD2 Macropad CrowPanel - backup flash
cd /d "%~dp0"
rem Read-only 16 MB backup. Does not erase or write the board.
rem Usage: backup_COM5.bat [COMx] [baud] [outfile]
rem Baud must be 115200, 230400, or 460800. 921600 is refused.

set "PORT=%~1"
if "%PORT%"=="" set "PORT=COM5"
set "BAUD=%~2"
if "%BAUD%"=="" set "BAUD=460800"
set "OUT=%~3"

call :checkbaud
if errorlevel 1 exit /b 1
call :findpython
if errorlevel 1 exit /b 1

if not "%OUT%"=="" goto haveout
set "STAMP=manual"
for /f %%I in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd-HHmmss"') do set "STAMP=%%I"
set "OUT=flash-backup-%PORT%-%STAMP%.bin"
:haveout

echo.
echo === Closing HD2 Macropad Link so %PORT% is free ===
taskkill /im HD2MacropadLink.exe /f >nul 2>&1
taskkill /im HD2MacropadLink-v1.1.exe /f >nul 2>&1
taskkill /im HD2MacropadLink-v1.2.exe /f >nul 2>&1
timeout /t 2 /nobreak >nul

echo.
echo Reading 16 MB from %PORT% at %BAUD% into:
echo   %CD%\%OUT%
echo This only reads flash. It can take several minutes.
%PY% -m esptool --chip esp32s3 --port "%PORT%" --baud %BAUD% read_flash 0x0 0x1000000 "%OUT%"
if not errorlevel 1 goto done
if "%BAUD%"=="115200" goto failed
echo.
echo === Retrying the read at 115200 ===
%PY% -m esptool --chip esp32s3 --port "%PORT%" --baud 115200 read_flash 0x0 0x1000000 "%OUT%"
if errorlevel 1 goto failed

:done
echo.
echo Backup written: %CD%\%OUT%
echo Keep this file somewhere other than the only copy on the PC.
pause
exit /b 0

:failed
echo.
echo BACKUP FAILED. Nothing was erased.
echo Close HD2 Macropad Link and any other program using %PORT%, then try again.
echo If auto-reset fails, hold BOOT, tap RESET, release BOOT, and retry at 115200.
pause
exit /b 1

:checkbaud
if /I "%BAUD%"=="115200" exit /b 0
if /I "%BAUD%"=="230400" exit /b 0
if /I "%BAUD%"=="460800" exit /b 0
echo Refusing baud %BAUD%.
echo Use 460800, 230400, or 115200. 921600 corrupts reads on this PC.
pause
exit /b 1

:findpython
set "PY="
where py >nul 2>&1 && set "PY=py -3"
if not defined PY (
  where python >nul 2>&1 && set "PY=python"
)
if not defined PY (
  echo Python 3 was not found.
  echo Install Python, then install esptool yourself: python -m pip install esptool
  echo This script does not install or upgrade packages.
  pause
  exit /b 1
)
%PY% -m esptool version >nul 2>&1
if errorlevel 1 (
  echo esptool is not installed for this Python.
  echo Install it yourself: %PY% -m pip install esptool
  echo This script does not install or upgrade packages.
  pause
  exit /b 1
)
echo Using: %PY%
exit /b 0
