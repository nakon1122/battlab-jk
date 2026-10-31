@echo off
setlocal
cd /d "%~dp0"
py -3.11 --version
if errorlevel 1 goto failed
py -3.11 -m esptool version >nul 2>nul
if errorlevel 1 (
 py -3.11 -m pip install esptool==4.5.1
 if errorlevel 1 goto failed
)
set /p BATT_COM=Enter ESP32-S3 COM port: 
if not defined BATT_COM exit /b 1
py -3.11 -m esptool --chip esp32s3 --port "%BATT_COM%" --baud 115200 write_flash 0x0 firmware\BATTLAB_S3_BLEFIX_INSTALL.bin
if errorlevel 1 goto failed
echo SUCCESS. Press RST. Connect BATTLAB-JK without Wi-Fi password.
echo Open http://192.168.4.1 - BMS code is optional for reading.
pause
exit /b 0
:failed
echo FAILED. Close serial monitor. Check COM/cable. Hold BOOT and tap RST if needed.
pause
exit /b 1
