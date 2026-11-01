# -*- coding: utf-8 -*-
"""上电捕获 v2：
- 持续发回车抓 bootdelay 进 U-Boot
- 抓到 U-Boot> -> 打印并退出
- 检测到 ROM 模式(ERR: 刷屏) -> 立即退出报 rom_mode（不死循环, 不锁串口）
- 硬超时退出
"""
import time
import serial

PORT = "COM24"
DURATION = 300

ser = serial.Serial(PORT, 115200, timeout=0.2, rtscts=False, dsrdtr=False)
ser.reset_input_buffer()

tail = b""
t0 = time.time()
state = "timeout"
while time.time() - t0 < DURATION:
    ser.write(b"\r")
    time.sleep(0.1)
    c = ser.read(4096)
    if c:
        tail = (tail + c)[-8192:]
        if b"U-Boot>" in tail:
            state = "uboot"
            break
        if tail.count(b"ERR:\t203") >= 3:
            state = "rom_mode"
            break

print("state=%s elapsed=%.0fs" % (state, time.time() - t0))
if state == "uboot":
    print(tail[-400:].decode("utf-8", "replace"))
ser.close()
