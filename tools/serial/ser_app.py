# -*- coding: utf-8 -*-
"""媒体应用 shell 交互：回车取提示符 -> 依次发命令，最后持续抓一段输出。
用法: python ser_app.py "cmd1" "cmd2" ... [总时长秒]
与 uboot 不同，提示符未知，靠静默 1.5s 判定命令结束。
"""
import sys, time
import serial

PORT = "COM24"
BAUD = 115200

args = sys.argv[1:]
total = 20.0
if args and args[-1].replace(".", "").isdigit():
    total = float(args[-1]); args = args[:-1]

ser = serial.Serial(PORT, BAUD, timeout=0.3, rtscts=False, dsrdtr=False)
ser.reset_input_buffer()
time.sleep(0.2)

def pump(sec, quiet_end=1.5):
    buf = b""
    t0 = time.time()
    idle = 0.0
    while time.time() - t0 < sec and idle < quiet_end:
        c = ser.read(4096)
        if c:
            buf += c; idle = 0.0
        else:
            idle += 0.3
    sys.stdout.buffer.write(buf); sys.stdout.buffer.flush()
    return buf

print("==== press enter to get shell ====", flush=True)
ser.write(b"\r")
pump(3)

for cmd in args:
    print("\n==== CMD: %s ====" % cmd, flush=True)
    ser.write(cmd.encode() + b"\r")
    if cmd.startswith("vlcview"):
        pump(total, quiet_end=total)  # 推流是长驻命令，抓满时长
    else:
        pump(8)

ser.close()
print("\n==== serial closed (stream keeps running on board) ====")
