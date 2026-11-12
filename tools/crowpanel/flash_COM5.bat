@echo off
setlocal EnableExtensions
title HD2 Macropad v1.9.1-cp7.2 - FULL flash (erases presets)
cd /d "%~dp0"
rem Advanced recovery. Not the normal first install.
rem The normal first install is HD2MacropadLink-v1.2.6.exe.
rem This script always backs up 16 MB first. If that fails, it stops.
rem It erases the chip only after you type ERASE. There is no skip.
rem Usage: flash_COM5.bat [COMx] [baud]
rem Default port is COM5 only because that was the maintainer's port.
rem Default baud is 460800, then 115200. 921600 is refused.

set "PORT=%~1"
if "%PORT%"=="" set "PORT=COM5"
set "BAUD=%~2"
if "%BAUD%"=="" set "BAUD=460800"
set "BIN=hd2_macropad_v1.9.1-cp7.2_crowpanel_adv7_0x0.bin"

call :checkbaud
if errorlevel 1 exit /b 1
if not exist "%BIN%" (
  echo %BIN% was not found next to this script.
  echo Build the merged image with docs\crowpanel\README.md and copy it here under that name.
  echo To keep presets on a board that is already on the cp7 layout, use update_COM5.bat instead.
  pause
  exit /b 1
)
call :findpython
if errorlevel 1 exit /b 1

echo This is the advanced full-erase path, not the normal first install.
echo The normal first install is HD2MacropadLink-v1.2.6.exe. It does not mass-erase.
echo This script will read a 16 MB backup of %PORT% first.
echo If that backup fails, nothing is erased.
echo A full erase deletes presets and settings. Connectivity must be set back to USB.
echo Do not flash an upstream JC3248 install.bin onto this 7 inch board.
echo.
echo === Backing up 16 MB before any erase ===
call "%~dp0backup_COM5.bat" "%PORT%" "%BAUD%" "" NOPAUSE
if errorlevel 1 (
  echo Backup failed. Nothing was erased.
  exit /b 1
)
echo.
echo Backup step finished. Type ERASE to erase the chip. Anything else cancels.
set /p CONFIRM=
if /I not "%CONFIRM%"=="ERASE" (
  echo Cancelled. Nothing was erased.
  exit /b 1
)
echo.
echo === Closing HD2 Macropad Link so %PORT% is free ===
taskkill /im HD2MacropadLink.exe /f >nul 2>&1
taskkill /im HD2MacropadLink-v1.1.exe /f >nul 2>&1
taskkill /im HD2MacropadLink-v1.2.exe /f >nul 2>&1
taskkill /im HD2MacropadLink-v1.2.2.exe /f >nul 2>&1
taskkill /im HD2MacropadLink-v1.2.3.exe /f >nul 2>&1
taskkill /im HD2MacropadLink-v1.2.4.exe /f >nul 2>&1
taskkill /im HD2MacropadLink-v1.2.5.exe /f >nul 2>&1
taskkill /im HD2MacropadLink-v1.2.6.exe /f >nul 2>&1
timeout /t 2 /nobreak >nul

echo.
echo === Erasing flash on %PORT% at %BAUD% ===
%PY% -m esptool --chip esp32s3 --port "%PORT%" --baud %BAUD% erase_flash
if not errorlevel 1 goto writefast
if "%BAUD%"=="115200" goto failed
echo.
echo === Retrying erase at 115200 ===
set "BAUD=115200"
%PY% -m esptool --chip esp32s3 --port "%PORT%" --baud %BAUD% erase_flash
if errorlevel 1 goto failed

:writefast
echo.
echo === Writing %BIN% at 0x0 ===
%PY% -m esptool --chip esp32s3 --port "%PORT%" --baud %BAUD% write_flash 0x0 "%BIN%"
if not errorlevel 1 goto done
if "%BAUD%"=="115200" goto failed
echo.
echo === Retrying erase and write at 115200 ===
set "BAUD=115200"
%PY% -m esptool --chip esp32s3 --port "%PORT%" --baud %BAUD% erase_flash
if errorlevel 1 goto failed
%PY% -m esptool --chip esp32s3 --port "%PORT%" --baud %BAUD% write_flash 0x0 "%BIN%"
if errorlevel 1 goto failed

:done
echo.
echo ==== FLASH COMPLETE ====
echo Set Config, Connectivity to USB. The full erase cleared that setting.
echo Speaker audio needs DIP S1=0 and S0=0, a PH2.0 speaker on SPK, and mute off.
call :startlink
pause
exit /b 0

:failed
echo.
echo FLASH FAILED.
echo Close anything else using %PORT% and run this again.
echo If auto-reset fails, hold BOOT, tap RESET, release BOOT, and retry at 115200:
echo   flash_COM5.bat %PORT% 115200
call :startlink
pause
exit /b 1

:startlink
if exist "%~dp0HD2MacropadLink-v1.2.6.exe" (
  echo Starting HD2MacropadLink-v1.2.6.exe ...
  start "" "%~dp0HD2MacropadLink-v1.2.6.exe"
) else if exist "%~dp0HD2MacropadLink-v1.2.5.exe" (
  echo Starting HD2MacropadLink-v1.2.5.exe ...
  start "" "%~dp0HD2MacropadLink-v1.2.5.exe"
) else if exist "%~dp0HD2MacropadLink-v1.2.4.exe" (
  echo Starting HD2MacropadLink-v1.2.4.exe ...
  start "" "%~dp0HD2MacropadLink-v1.2.4.exe"
) else (
  echo Link exe not next to this script. Start HD2MacropadLink-v1.2.6.exe yourself.
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
