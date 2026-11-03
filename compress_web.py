import gzip
import os

in_path = 'esp32c5-pro-router/main/web/index.html'
gz_path = 'esp32c5-pro-router/main/web/index.html.gz'
h_path = 'esp32c5-pro-router/main/index_html_gz.h'

with open(in_path, 'rb') as f:
    raw = f.read()

compressed = gzip.compress(raw, 9)

with open(gz_path, 'wb') as f:
    f.write(compressed)

print(f"Compressed {len(raw)} bytes to {len(compressed)} bytes ({len(compressed)/len(raw)*100:.1f}%)")

# Generate C header with array
with open(h_path, 'w', encoding='utf-8') as f:
    f.write('/* Auto-generated gzipped web dashboard */\n')
    f.write('#pragma once\n\n')
    f.write('#include <stddef.h>\n')
    f.write('#include <stdint.h>\n\n')
    f.write(f'#define INDEX_HTML_GZ_SIZE {len(compressed)}\n\n')
    f.write('static const uint8_t index_html_gz[] = {\n')
    for i in range(0, len(compressed), 16):
        chunk = compressed[i:i+16]
        hex_str = ', '.join(f'0x{b:02x}' for b in chunk)
        f.write(f'    {hex_str},\n')
    f.write('};\n')

print(f"Generated {h_path} ({len(compressed)} bytes)")

