@echo off
setlocal
cd /d "%~dp0"
echo Bambu Screen CYD 3.5in - first install (erases settings)
py -3.11 --version
if errorlevel 1 goto failed
py -3.11 -m esptool version >nul 2>nul
if errorlevel 1 (
 py -3.11 -m pip install esptool==4.5.1
 if errorlevel 1 goto failed
)
set /p BB_COM=Enter COM port (e.g. COM6): 
if not defined BB_COM exit /b 1
py -3.11 -m esptool --chip esp32 --port "%BB_COM%" --baud 460800 erase_flash
if errorlevel 1 goto failed
py -3.11 -m esptool --chip esp32 --port "%BB_COM%" --baud 460800 write_flash 0x0 firmware\BAMBU_SCREEN_CYD35_INSTALL.bin
if errorlevel 1 goto failed
echo SUCCESS. Press RST on the board.
pause
exit /b 0
:failed
echo FAILED. Close serial monitor. Check COM/cable. Hold BOOT and tap RST if needed.
pause
exit /b 1
