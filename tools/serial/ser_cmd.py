# -*- coding: utf-8 -*-
"""向 COM3 的 uboot 逐条发命令，等到 U-Boot> 提示符回来再发下一条。
用法: python ser_cmd.py "cmd1" "cmd2" ...
每条命令超时 90s（sf erase/write 可能较慢），输出完整 transcript。
"""
import sys, time
import serial

PORT = "COM24"
BAUD = 115200
PROMPT = b"U-Boot> "

cmds = sys.argv[1:]
ser = serial.Serial(PORT, BAUD, timeout=0.3, rtscts=False, dsrdtr=False)
ser.reset_input_buffer()
time.sleep(0.2)
ser.write(b"\r")
time.sleep(0.5)
ser.reset_input_buffer()

def drain(sec):
    out = b""
    t0 = time.time()
    idle = 0.0
    while time.time() - t0 < sec:
        c = ser.read(4096)
        if c:
            out += c
            idle = 0.0
        else:
            idle += 0.3
            # 已经看到提示符且静默 0.9s -> 结束
            if out.rstrip().endswith(PROMPT.strip()) and idle > 0.9:
                break
    return out

for cmd in cmds:
    print("\n######## CMD: %s ########" % cmd, flush=True)
    ser.write(cmd.encode() + b"\r\n")
    out = drain(180)
    sys.stdout.buffer.write(out)
    sys.stdout.buffer.flush()
ser.close()
