# -*- coding: utf-8 -*-
"""从任意状态进入 uboot：msh 里发 reboot，bootdelay 期间连打回车打断。"""
import sys, time
import serial

PORT = "COM24"
ser = serial.Serial(PORT, 115200, timeout=0.3, rtscts=False, dsrdtr=False)
ser.reset_input_buffer()
time.sleep(0.2)

ser.write(b"\r")
time.sleep(1.0)
probe = ser.read(4096)

if b"U-Boot>" in probe:
    print("[already in U-Boot]")
else:
    print("[in app shell, rebooting...]")
    ser.write(b"reboot\r")
    buf = b""
    t0 = time.time()
    while time.time() - t0 < 10:
        ser.write(b"\r")          # bootdelay=2s，连打打断
        time.sleep(0.25)
        c = ser.read(4096)
        if c:
            buf += c
        if b"U-Boot>" in buf:
            break
    sys.stdout.buffer.write(buf[-1500:])
    print()
    print("[in U-Boot]" if b"U-Boot>" in buf else "[FAILED to interrupt]")
ser.close()
