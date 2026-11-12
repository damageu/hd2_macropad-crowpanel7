@echo off
setlocal EnableExtensions
title HD2 Macropad v1.9.1-cp7.2 - app update (NVS presets kept)
cd /d "%~dp0"
rem Writes the application only, at 0x10000. Does not erase the chip.
rem This is the manual form of an app update for a board that already
rem uses the cp7 partition table. A stock board should use
rem HD2MacropadLink-v1.2.4.exe instead, which also writes the bootloader
rem and the partition table and does not mass-erase.
rem Usage: update_COM5.bat [COMx] [baud]
rem Put the app bin next to this script. Close HD2 Macropad Link first;
rem this script closes it as well.

set "PORT=%~1"
if "%PORT%"=="" set "PORT=COM5"
set "BAUD=%~2"
if "%BAUD%"=="" set "BAUD=460800"
set "BIN=hd2_macropad_v1.9.1-cp7.2_app_0x10000.bin"

call :checkbaud
if errorlevel 1 exit /b 1
if not exist "%BIN%" (
  echo %BIN% was not found next to this script.
  echo Build it with the steps in docs\crowpanel\README.md and copy firmware.bin here under that name.
  echo For a full image, including a board that is not already on the cp7 layout, use flash_COM5.bat.
  pause
  exit /b 1
)
call :findpython
if errorlevel 1 exit /b 1

echo This writes only the app at 0x10000 on %PORT%.
echo NVS presets and settings are kept when the board is already on the cp7 layout.
echo It does not erase the chip. Run backup_COM5.bat first if you want a copy.
echo.

echo === Closing HD2 Macropad Link so %PORT% is free ===
taskkill /im HD2MacropadLink.exe /f >nul 2>&1
taskkill /im HD2MacropadLink-v1.1.exe /f >nul 2>&1
taskkill /im HD2MacropadLink-v1.2.exe /f >nul 2>&1
taskkill /im HD2MacropadLink-v1.2.2.exe /f >nul 2>&1
taskkill /im HD2MacropadLink-v1.2.3.exe /f >nul 2>&1
taskkill /im HD2MacropadLink-v1.2.4.exe /f >nul 2>&1
timeout /t 2 /nobreak >nul

echo.
echo === Writing the app at 0x10000, %BAUD% baud ===
%PY% -m esptool --chip esp32s3 --port "%PORT%" --baud %BAUD% write_flash 0x10000 "%BIN%"
if not errorlevel 1 goto done
if "%BAUD%"=="115200" goto failed
echo.
echo === Retrying at 115200 ===
%PY% -m esptool --chip esp32s3 --port "%PORT%" --baud 115200 write_flash 0x10000 "%BIN%"
if errorlevel 1 goto failed

:done
echo.
echo ==== UPDATE COMPLETE ====
echo The board is restarting. Connectivity should still be USB.
echo About should show v1.9.1-cp7.2 after you build this tree.
call :startlink
timeout /t 5
exit /b 0

:failed
echo.
echo UPDATE FAILED. The chip was not erased by this script.
echo Close anything else using %PORT% and run this again.
echo If auto-reset fails, hold BOOT, tap RESET, release BOOT, and retry at 115200.
call :startlink
pause
exit /b 1

:startlink
if exist "%~dp0HD2MacropadLink-v1.2.4.exe" (
  echo Starting HD2MacropadLink-v1.2.4.exe ...
  start "" "%~dp0HD2MacropadLink-v1.2.4.exe"
) else (
  echo Link exe not next to this script. Start HD2MacropadLink-v1.2.4.exe yourself.
)
exit /b 0

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
