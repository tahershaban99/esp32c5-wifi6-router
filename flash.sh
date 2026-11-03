#!/usr/bin/env bash
set -e

echo "========================================================"
echo "  ESP32-C5 Dual-Band Wi-Fi 6 Router Firmware Flasher"
echo "========================================================"
echo ""

read -rp "Enter Serial Port (e.g. /dev/ttyUSB0 or /dev/ttyACM0): " PORT
if [ -z "$PORT" ]; then
    echo "[ERROR] No serial port entered!"
    exit 1
fi

echo ""
echo "Flashing ESP32-C5 on $PORT..."
echo ""

python3 -m esptool --chip esp32c5 -p "$PORT" -b 460800 --before default-reset --after hard-reset write-flash \
  --flash-mode dio --flash-size 4MB --flash-freq 80m \
  0x2000 esp32c5-pro-router/build/bootloader/bootloader.bin \
  0x8000 esp32c5-pro-router/build/partition_table/partition-table.bin \
  0xf000 esp32c5-pro-router/build/ota_data_initial.bin \
  0x20000 esp32c5-pro-router/build/esp32c5-pro-router.bin

echo ""
echo "========================================================"
echo "  FLASH SUCCESSFUL!"
echo "  SoftAP SSID: ESP32-C5-5G-Router"
echo "  Password:    12345678"
echo "  Dashboard:   http://192.168.4.1"
echo "========================================================"

