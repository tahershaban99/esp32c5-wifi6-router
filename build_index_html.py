import os
import re
import sys

with open('preview_dashboard_v2.html', 'r', encoding='utf-8') as f:
    html = f.read()

# 1. Update Title and Header Badge
html = html.replace(
    '<span class="badge badge-gaming" style="font-size:0.72rem">v3.2.0-PRO</span>',
    '<span class="badge badge-gaming" style="font-size:0.72rem">v3.2.0-PRO / ESP32-C5 5GHz</span>'
)

# 2. Update Card 6 CPU clock string
html = html.replace(
    '<span class="metric-sub" id="cpu-clock-str">التردد: 240 MHz (Dual Core)</span>',
    '<span class="metric-sub" id="cpu-clock-str">التردد: 240 MHz (RISC-V + 5GHz Wi-Fi 6)</span>'
)

# 3. Update Card 7 WAN details
html = html.replace(
    '<span class="metric-sub" id="wan-details-metric">-58 dBm 📶 | 192.168.1.105</span>',
    '<span class="metric-sub" id="wan-details-metric">-58 dBm 📶 | 5GHz Ch 36 | 192.168.1.105</span>'
)

# 4. In Tab 5, insert Frequency Band Mode Selector before Wi-Fi Protocol & Bandwidth Selector
old_proto_block = '''        <div class="form-group" style="margin-top:10px">
          <label style="font-weight:600">معيار وسرعة بث الواي فاي (Wi-Fi Protocol / Bandwidth):</label>
          <select name="wifi_protocol" id="cfg_wifi_protocol">
            <option value="1" selected>802.11b/g/n Mixed (HT40) — حتى 150 Mbps (السرعة القصوى والأداء الأسرع)</option>
            <option value="0">802.11b/g/n Mixed (HT20) — حتى 72.2 Mbps (استقرار عالٍ وتوافق شامل)</option>
            <option value="2">802.11n Only (HT40) — حتى 150 Mbps (للأجهزة الحديثة فقط)</option>
            <option value="3">802.11n Only (HT20) — حتى 72.2 Mbps</option>
          </select>
        </div>'''

new_proto_block = '''        <div class="form-group" style="margin-top:10px">
          <label style="font-weight:600">نطاق تردد بث الواي فاي للـ ESP32 (Wi-Fi Band Mode):</label>
          <select name="ap_band_mode" id="cfg_ap_band_mode">
            <option value="3" selected>Dual-Band Auto (2.4GHz + 5GHz) — التبديل الذكي التلقائي</option>
            <option value="2">5 GHz Only (802.11a/n/ac/ax — سرعة فائقة وتداخل معدوم)</option>
            <option value="1">2.4 GHz Only (802.11b/g/n/ax — تغطية ومدى أقصى)</option>
          </select>
        </div>
        <div class="form-group" style="margin-top:10px">
          <label style="font-weight:600">معيار وسرعة بث الواي فاي (Wi-Fi Protocol / Bandwidth):</label>
          <select name="wifi_protocol" id="cfg_wifi_protocol">
            <option value="4" selected>802.11ax (Wi-Fi 6 HE40 - 5GHz/2.4GHz) — أقصى سرعة وأقل زمن تأخير</option>
            <option value="5">802.11a/n/ac/ax Mixed (5GHz HT40/VHT40) — فائق السرعة</option>
            <option value="1">802.11b/g/n/ax Mixed (HT40) — متوافق شامل</option>
            <option value="0">802.11b/g/n Mixed (HT20) — استقرار وتوافق أقصى</option>
          </select>
        </div>'''

if old_proto_block in html:
    html = html.replace(old_proto_block, new_proto_block)
    print("Replaced SoftAP protocol block with 5GHz band & Wi-Fi 6 selectors!")

out_dir = 'esp32c5-pro-router/main/web'
os.makedirs(out_dir, exist_ok=True)
out_path = os.path.join(out_dir, 'index.html')
with open(out_path, 'w', encoding='utf-8') as f:
    f.write(html)
print(f"Base updated HTML written to {out_path}")
