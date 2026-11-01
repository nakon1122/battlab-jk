@echo off
setlocal
cd /d "%~dp0"
echo Bambu Screen CYD 3.5in - update, keeps Wi-Fi and printer settings
py -3.11 -m esptool version >nul 2>nul
if errorlevel 1 py -3.11 -m pip install esptool==4.5.1
set /p BB_COM=Enter COM port (e.g. COM6): 
if not defined BB_COM exit /b 1
py -3.11 -m esptool --chip esp32 --port "%BB_COM%" --baud 460800 write_flash 0x10000 firmware\BAMBU_SCREEN_CYD35_APP_UPDATE.bin
if errorlevel 1 goto failed
echo SUCCESS.
pause
exit /b 0
:failed
echo FAILED. Close serial monitor. Check COM/cable. Hold BOOT and tap RST if needed.
pause
exit /b 1
