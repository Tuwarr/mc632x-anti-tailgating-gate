# -*- coding: utf-8 -*-
"""强健版：发 reboot，然后持续 60s 连发回车并监听，直到见到 U-Boot> 后 3 秒。"""
import time
import serial

PORT = "COM24"
DURATION = 60

ser = serial.Serial(PORT, 115200, timeout=0.2, rtscts=False, dsrdtr=False)
ser.reset_input_buffer()
time.sleep(0.2)

ser.write(b"reboot\r")
time.sleep(0.5)

tail = b""
got_uboot_at = None
t0 = time.time()
while time.time() - t0 < DURATION:
    ser.write(b"\r")
    time.sleep(0.08)
    c = ser.read(4096)
    if c:
        tail = (tail + c)[-8192:]
        if b"U-Boot>" in tail:
            if got_uboot_at is None:
                got_uboot_at = time.time()
                tail = b""
            elif time.time() - got_uboot_at > 3:
                break

print("uboot_captured=%s" % (got_uboot_at is not None))
if got_uboot_at:
    idx = max(0, len(tail) - 600)
    print(tail[idx:].decode("utf-8", "replace"))
ser.close()
