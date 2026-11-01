# -*- coding: utf-8 -*-
"""上电捕获 v3（长跑等待型）：
- 持续发回车（ROM 态下无害，冷启动后能打断 bootdelay）
- 抓到 U-Boot> -> state=uboot 退出
- 看到异常刷屏或 msh 提示符 -> state=missed（窗口错过，固件已自启）退出
- 硬超时 480s
"""
import time
import serial

PORT = "COM24"
DURATION = 480

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
        if b"Execption:" in tail or b"msh />" in tail:
            state = "missed"
            break

print("state=%s elapsed=%.0fs" % (state, time.time() - t0))
print(tail[-500:].decode("utf-8", "replace"))
ser.close()
