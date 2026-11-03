import serial
import time
import sys

print("============================================================")
print("  ESP32-C5 Live Boot Monitor")
print("  Waiting for normal boot (Click RST button or reconnect USB)...")
print("============================================================")

max_wait = 25
start_time = time.time()
connected = False
s = None

while time.time() - start_time < max_wait:
    try:
        if not s:
            s = serial.Serial('COM7', 115200, timeout=0.5)
            connected = True
        line = s.readline()
        if line:
            decoded = line.decode('utf-8', errors='replace').rstrip()
            print(decoded)
            if "SoftAP started" in decoded or "web_server: Web server started" in decoded or "main: =========" in decoded:
                pass
    except Exception:
        # Port might temporarily disappear on USB replug
        s = None
        time.sleep(0.5)

if s:
    s.close()
print("============================================================")

