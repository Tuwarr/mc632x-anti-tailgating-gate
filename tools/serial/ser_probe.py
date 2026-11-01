# -*- coding: utf-8 -*-
"""探测 COM3：发送回车，抓取 uboot/固件的串口输出，判断板卡当前状态"""
import sys, time
import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM3"
BAUD = 115200

ser = serial.Serial(PORT, BAUD, timeout=0.2, rtscts=False, dsrdtr=False)
ser.reset_input_buffer()
time.sleep(0.2)

# 连发几次回车打断自动启动
for _ in range(3):
    ser.write(b"\r")
    time.sleep(0.3)

buf = b""
t0 = time.time()
while time.time() - t0 < 4:
    chunk = ser.read(4096)
    if chunk:
        buf += chunk
ser.close()

sys.stdout.buffer.write(buf if buf else b"<no output in 4s>")
print()
print("--- repr head ---")
print(repr(buf[:600]))
