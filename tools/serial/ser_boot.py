# -*- coding: utf-8 -*-
"""发 reset 并抓启动日志（应用阶段提示符未知，靠静默判断结束）"""
import sys, time
import serial

PORT = "COM24"
sec = float(sys.argv[1]) if len(sys.argv) > 1 else 40
send = sys.argv[2].encode() if len(sys.argv) > 2 else b"reset"

ser = serial.Serial(PORT, 115200, timeout=0.3, rtscts=False, dsrdtr=False)
ser.reset_input_buffer()
time.sleep(0.2)
ser.write(send + b"\r\n")

buf = b""
t0 = time.time()
idle = 0.0
while time.time() - t0 < sec and idle < 6:
    c = ser.read(4096)
    if c:
        buf += c
        idle = 0.0
    else:
        idle += 0.3
ser.close()
sys.stdout.buffer.write(buf if buf else b"<no output>")
print()
