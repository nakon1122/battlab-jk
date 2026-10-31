@echo off
setlocal
cd /d "%~dp0"
echo BATTLAB for ESP32-3248S035 (3.5 inch display board)
py -3.11 --version
if errorlevel 1 goto failed
py -3.11 -m esptool version >nul 2>nul
if errorlevel 1 (
 py -3.11 -m pip install esptool==4.5.1
 if errorlevel 1 goto failed
)
set /p BATT_COM=Enter display board COM port (e.g. COM6): 
if not defined BATT_COM exit /b 1
py -3.11 -m esptool --chip esp32 --port "%BATT_COM%" --baud 460800 write_flash 0x0 firmware\BATTLAB_CYD35_INSTALL.bin
if errorlevel 1 goto failed
echo SUCCESS. The screen shows the dashboard after reset.
echo Tap the screen to switch pages. Web: connect to Wi-Fi BATTLAB-JK, open http://192.168.4.1
pause
exit /b 0
:failed
echo FAILED. Close serial monitor. Check COM/cable (CH340 driver). Hold BOOT and tap RST if needed.
pause
exit /b 1
