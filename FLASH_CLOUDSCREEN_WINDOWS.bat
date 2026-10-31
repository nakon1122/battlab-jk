@echo off
setlocal
cd /d "%~dp0"
echo BATTLAB CLOUD SCREEN for ESP32-3248S035 - shows all batteries from BattLab Cloud
py -3.11 --version
if errorlevel 1 goto failed
py -3.11 -m esptool version >nul 2>nul
if errorlevel 1 (
 py -3.11 -m pip install esptool==4.5.1
 if errorlevel 1 goto failed
)
set /p BATT_COM=Enter display board COM port (e.g. COM6): 
if not defined BATT_COM exit /b 1
py -3.11 -m esptool --chip esp32 --port "%BATT_COM%" --baud 460800 write_flash 0x0 firmware\BATTLAB_CLOUDSCREEN_INSTALL.bin
if errorlevel 1 goto failed
echo SUCCESS. Set Wi-Fi on the screen, then add the pair code on the website.
echo Website: https://nakon1122.github.io/battlab-cloud/
pause
exit /b 0
:failed
echo FAILED. Close serial monitor. Check COM/cable (CH340 driver). Hold BOOT and tap RST if needed.
pause
exit /b 1
