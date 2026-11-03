import subprocess
import sys
import time

try:
    sys.stdout.reconfigure(encoding='utf-8')
except Exception:
    pass

esptool_py = r"E:\Espressif\tools\python\v6.1\venv\Scripts\python.exe"
bootloader_bin = r"esp32c5-pro-router\build\bootloader\bootloader.bin"
partition_bin = r"esp32c5-pro-router\build\partition_table\partition-table.bin"
ota_data_bin = r"esp32c5-pro-router\build\ota_data_initial.bin"
app_bin = r"esp32c5-pro-router\build\esp32c5-pro-router.bin"

cmd = [
    esptool_py, "-m", "esptool",
    "--chip", "esp32c5",
    "-p", "COM7",
    "-b", "460800",
    "--before", "default-reset",
    "--after", "hard-reset",
    "write-flash",
    "--flash-mode", "dio",
    "--flash-size", "4MB",
    "--flash-freq", "80m",
    "0x2000", bootloader_bin,
    "0x8000", partition_bin,
    "0xf000", ota_data_bin,
    "0x20000", app_bin
]

print("="*60)
print("  ESP32-C5 Auto-Flasher")
print("="*60)
print("Please enter Download Mode on ESP32-C5:")
print("1) Hold BOOT button")
print("2) Click RST button once")
print("3) Release BOOT button")
print("="*60)

max_attempts = 30
for attempt in range(1, max_attempts + 1):
    print(f"[Attempt {attempt}/{max_attempts}] Checking COM7...")
    sys.stdout.flush()
    res = subprocess.run(cmd, text=True, capture_output=True)
    if res.returncode == 0:
        print("\n" + "="*60)
        print("FLASH SUCCESSFUL! 100%")
        print("="*60)
        print(res.stdout)
        sys.stdout.flush()
        sys.exit(0)
    else:
        err = res.stderr + "\n" + res.stdout
        if "Failed to connect" in err or "No serial data received" in err:
            print("  Waiting for Download Mode (Hold BOOT, click RST)...")
        elif "Failed to start stub" in err:
            print("  Detected! Click RST to enter clean ROM bootloader...")
        else:
            print("  Retrying...")
        sys.stdout.flush()
        time.sleep(1)

print("\nAttempts timed out.")
sys.exit(1)
