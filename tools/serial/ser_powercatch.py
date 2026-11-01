# -*- coding: utf-8 -*-
"""上电捕获：在板子断电状态下先启动本脚本，它将持续向 COM24 发回车，
板子一上电进入 bootdelay=2s 倒计时就会被打断进入 U-Boot。
持续 120 秒或捕获到 U-Boot> 后 3 秒退出。输出追加到 powercatch.log。
"""
import time
import serial

PORT = "COM24"
LOG = "powercatch.log"
DURATION = 600

ser = serial.Serial(PORT, 115200, timeout=0.2, rtscts=False, dsrdtr=False)
ser.reset_input_buffer()

f = open(LOG, "ab", buffering=0)
f.write(b"\n==== powercatch start %s ====\n" % time.strftime("%Y-%m-%d %H:%M:%S").encode())

tail = b""
got_uboot_at = None
t0 = time.time()
while time.time() - t0 < DURATION:
    ser.write(b"\r")
    time.sleep(0.1)
    c = ser.read(4096)
    if c:
        f.write(c)
        tail = (tail + c)[-8192:]
        if b"U-Boot>" in tail:
            if got_uboot_at is None:
                got_uboot_at = time.time()
                tail = b""          # 只触发一次，之后观察 3 秒
            elif time.time() - got_uboot_at > 3:
                break
    elif got_uboot_at is not None and time.time() - got_uboot_at > 3:
        break

f.write(b"\n==== powercatch end %s uboot=%s ====\n" % (
    time.strftime("%Y-%m-%d %H:%M:%S").encode(), str(got_uboot_at is not None).encode()))
f.close()
ser.close()
print("done, uboot_captured=%s" % (got_uboot_at is not None))
