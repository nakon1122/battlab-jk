@echo off
setlocal
cd /d "%~dp0"
echo Bambu Bridge (ESP32-S3) - UPDATE
py -3.11 -m esptool version >nul 2>nul
if errorlevel 1 py -3.11 -m pip install esptool==4.5.1
echo.
echo Which ESP32-S3 board? (look at the chip / shop page)
echo   1 = N16R8 / N8R8 / N32R8  (octal PSRAM, most common - camera works)
echo   2 = N8R2 / N4R2           (quad PSRAM - camera works)
echo   3 = no PSRAM              (no camera)
set /p BB_T=Choose 1, 2 or 3: 
set BB_V=_N16R8
if "%BB_T%"=="2" set BB_V=_N8R2
if "%BB_T%"=="3" set BB_V=
set /p BB_COM=Enter COM port (e.g. COM6): 
if not defined BB_COM exit /b 1
py -3.11 -m esptool --chip esp32s3 --port "%BB_COM%" --baud 460800 write_flash 0x10000 firmware\BAMBU_BRIDGE_S3%BB_V%_APP_UPDATE.bin
if errorlevel 1 goto failed
echo SUCCESS. Press RST on the board.
pause
exit /b 0
:failed
echo FAILED. Close serial monitor. Check COM/cable. Hold BOOT and tap RST if needed.
pause
exit /b 1
