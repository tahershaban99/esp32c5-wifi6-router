@echo off
setlocal enabledelayedexpansion

echo ========================================================
echo   ESP32-C5 Dual-Band Wi-Fi 6 Router Firmware Flasher
echo ========================================================
echo.

set /p COMPORT=Enter COM Port [Default: COM7]: 
if "%COMPORT%"=="" set COMPORT=COM7

echo.
echo Flashing ESP32-C5 on %COMPORT%...
echo (If connection fails, hold BOOT button, click RST button, then release BOOT)
echo.

python -m esptool --chip esp32c5 -p %COMPORT% -b 460800 --before default-reset --after hard-reset write-flash --flash-mode dio --flash-size 4MB --flash-freq 80m 0x2000 esp32c5-pro-router\build\bootloader\bootloader.bin 0x8000 esp32c5-pro-router\build\partition_table\partition-table.bin 0xf000 esp32c5-pro-router\build\ota_data_initial.bin 0x20000 esp32c5-pro-router\build\esp32c5-pro-router.bin

if %ERRORLEVEL% EQU 0 (
    echo.
    echo ========================================================
    echo   FLASH SUCCESSFUL!
    echo   SoftAP SSID: ESP32-C5-Router
    echo   Password:    12345678
    echo   Dashboard:   http://192.168.4.1
    echo ========================================================
) else (
    echo.
    echo [ERROR] Flashing failed!
    echo Hint: Hold the BOOT button, click RST, then run again.
)

pause
