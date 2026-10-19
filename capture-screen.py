"""Capture the device's real LVGL RGB565 frame over its USB console."""
import argparse
import base64
from pathlib import Path
import re
import struct
import time
import zlib
import serial

parser = argparse.ArgumentParser()
parser.add_argument('--port', default='COM3')
parser.add_argument('--page', type=int, choices=range(1, 8))
parser.add_argument('--wait', type=float, default=2)
parser.add_argument('--output', required=True)
args = parser.parse_args()
port = serial.Serial()
port.port, port.baudrate, port.timeout = args.port, 115200, 1
port.write_timeout = 2
port.dtr, port.rts = True, False
port.open()
port.set_buffer_size(rx_size=1024 * 1024, tx_size=4096)
if args.page:
    port.write(str(args.page).encode('ascii'))
time.sleep(args.wait)
port.reset_input_buffer()
port.write(b'S')
deadline = time.monotonic() + 35
collecting = False
frame = bytearray()
pending = bytearray()
complete = False
while time.monotonic() < deadline:
    pending.extend(port.read(port.in_waiting or 1))
    while b'\n' in pending:
        line, _, rest = pending.partition(b'\n')
        pending = bytearray(rest)
        line = line.strip()
        if line.startswith(b'CPR_FRAME_BEGIN'):
            _, width, height = line.split()
            width, height = int(width), int(height)
            collecting = True
        elif line == b'CPR_FRAME_END':
            complete = True
            break
        elif collecting and re.fullmatch(rb'[A-Za-z0-9+/=]+', line):
            frame.extend(base64.b64decode(line, validate=True))
    if complete:
        break
port.close()
if not collecting or len(frame) != width * height * 2:
    raise RuntimeError(f'Incomplete screen capture: {len(frame)} bytes')
rows = bytearray()
for y in range(height):
    rows.append(0)
    for x in range(width):
        value = struct.unpack_from('<H', frame, (y * width + x) * 2)[0]
        rows.extend(((value >> 11) * 255 // 31, ((value >> 5) & 63) * 255 // 63, (value & 31) * 255 // 31))
def chunk(kind, data):
    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))
png += chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b'')
output = Path(args.output)
output.parent.mkdir(parents=True, exist_ok=True)
output.write_bytes(png)
print(f'Screen saved: {output}')
