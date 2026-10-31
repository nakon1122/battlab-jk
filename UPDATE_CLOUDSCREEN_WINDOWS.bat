@echo off
setlocal
cd /d "%~dp0"
echo BATTLAB CLOUD SCREEN update - keeps Wi-Fi, pair code and touch calibration
py -3.11 --version
if errorlevel 1 goto failed
py -3.11 -m esptool version >nul 2>nul
if errorlevel 1 (
 py -3.11 -m pip install esptool==4.5.1
 if errorlevel 1 goto failed
)
set /p BATT_COM=Enter display board COM port (e.g. COM6): 
if not defined BATT_COM exit /b 1
py -3.11 -m esptool --chip esp32 --port "%BATT_COM%" --baud 460800 write_flash 0x1000 firmware\CYD_BOOTLOADER.bin 0x8000 firmware\CYD_PARTITIONS.bin 0xe000 firmware\BOOT_APP0.bin 0x10000 firmware\BATTLAB_CLOUDSCREEN_APP_UPDATE.bin
if errorlevel 1 goto failed
echo SUCCESS. Settings were kept.
pause
exit /b 0
:failed
echo FAILED. Close serial monitor. Check COM/cable (CH340 driver). Hold BOOT and tap RST if needed.
echo If the board does not start after an update, use FLASH_CLOUDSCREEN_WINDOWS.bat (full install).
pause
exit /b 1
