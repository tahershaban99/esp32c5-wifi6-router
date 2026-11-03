@echo off
setlocal enabledelayedexpansion

echo ========================================================
echo   ESP32-C5 Dual-Band Wi-Fi 6 Router Firmware Flasher
echo ========================================================
echo.

set /p COMPORT=Enter COM Port (e.g. COM3, COM4, COM5): 
if "%COMPORT%"=="" (
    echo [ERROR] No COM port entered!
    pause
    exit /b 1
)

echo.
echo Flashing ESP32-C5 on %COMPORT%...
echo.

python -m esptool --chip esp32c5 -p %COMPORT% -b 460800 --before default-reset --after hard-reset write-flash --flash-mode dio --flash-size 4MB --flash-freq 80m 0x2000 esp32c5-pro-router/build/bootloader/bootloader.bin 0x8000 esp32c5-pro-router/build/partition_table/partition-table.bin 0xf000 esp32c5-pro-router/build/ota_data_initial.bin 0x20000 esp32c5-pro-router/build/esp32c5-pro-router.bin

if %ERRORLEVEL% EQU 0 (
    echo.
    echo ========================================================
    echo   FLASH SUCCESSFUL!
    echo   SoftAP SSID: ESP32-C5-5G-Router
    echo   Password:    12345678
    echo   Dashboard:   http://192.168.4.1
    echo ========================================================
) else (
    echo.
    echo [ERROR] Flashing failed! Check cable, drivers, and COM port.
)

pause

