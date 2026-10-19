"""Read the device USB console without resetting it."""
import argparse
import time
import serial

parser = argparse.ArgumentParser()
parser.add_argument('--port', default='COM3')
parser.add_argument('--seconds', type=float, default=25)
args = parser.parse_args()
port = serial.Serial()
port.port, port.baudrate, port.timeout = args.port, 115200, 1
port.dtr, port.rts = True, False
port.open()
deadline = time.monotonic() + args.seconds
while time.monotonic() < deadline:
    line = port.readline()
    if line:
        print(line.decode('utf-8', errors='replace'), end='', flush=True)
port.close()
