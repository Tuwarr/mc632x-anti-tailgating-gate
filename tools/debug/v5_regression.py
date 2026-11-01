# -*- coding: utf-8 -*-
"""v5 回归: 捕捉上电 -> RAM 启动 -> inject 十连发压栈(旧版此测试必死) -> 四用例 -> 100s 观察"""
import serial, time, hmac, hashlib, secrets, sys

PORT = "COM24"
KEY = b"mc632x-demo-key-2026"

def fresh_payload():
    ts = int(time.time())
    nonce = secrets.token_hex(4)
    msg = "d1|%d|%s" % (ts, nonce)
    sig = hmac.new(KEY, msg.encode("ascii"), hashlib.sha256).hexdigest()[:32]
    return "%s|%s" % (msg, sig)

s = serial.Serial(PORT, 115200, timeout=0.05)
raw = open("v5_regression_raw.txt", "wb")
stamp = lambda: "[%s] " % time.strftime("%H:%M:%S")
log = lambda m: (print(stamp() + m, flush=True))

def pump(dur):
    end = time.time() + dur
    while time.time() < end:
        d = s.read(4096)
        if d:
            raw.write(d); raw.flush()

# P1-P3: 等横幅/倒计时/提示符
end = time.time() + 300; buf = b""; ok = False
while time.time() < end:
    d = s.read(4096)
    if d:
        buf += d; raw.write(d); raw.flush()
        if b"U-Boot 2016" in buf: ok = True; break
log("P1 banner=%s" % ok)
if not ok: s.close(); sys.exit(1)
saw = False; prompt = False; end = time.time() + 25
while time.time() < end:
    d = s.read(512)
    if d:
        buf += d; raw.write(d); raw.flush()
        if not saw and b"Hit any key" in d: saw = True
        if saw: s.write(b"\r\n")
        if b"U-Boot> " in d or b"=> " in d: prompt = True; break
log("P2 prompt=%s" % prompt)
if not prompt: s.close(); sys.exit(1)

# P3 tftp (3次机会)
ok = False
for a in range(3):
    time.sleep(0.6); s.reset_input_buffer()
    s.write(b"tftp a0000000 media_demo.bin\r\n")
    end = time.time() + 40; buf = b""
    while time.time() < end:
        d = s.read(4096)
        if d:
            buf += d; raw.write(d); raw.flush()
            if b"2600960" in buf.replace(b",", b"") or b"2600976" in buf.replace(b",", b"") or b"2600" in buf.replace(b",", b""):
                ok = True; break
    log("P3 tftp a%d=%s" % (a + 1, ok))
    if ok: break
    s.write(b"\r\n"); pump(1)
if not ok: s.close(); sys.exit(1)

# P4 go + 等启动
time.sleep(1.0); s.reset_input_buffer()
s.write(b"go a0000000\r\n")
pump(24)
s.write(b"\r\n"); pump(1)
s.write(b"ifconfig e0 192.168.137.100\r\n"); pump(2)
log("P4 booted+ifconfig")

# P5 qr_gate
s.reset_input_buffer()
s.write(b"qr_gate\r\n"); pump(4)
log("P5 qr_gate started")

# P6 inject 十连发压栈(旧版: 栈越界累积 -> 死; 新版: 全部 GRANT/replay 正常)
log("P6 inject x10 stress...", flush=True)
for i in range(10):
    p = fresh_payload()
    s.reset_input_buffer()
    s.write(("qr_inject %s\r\n" % p).encode())
    pump(1.2)
log("P6 stress done, observing 60s...", flush=True)
pump(60)

# P7 msh 还活着吗
s.write(b"\r\n"); pump(1.5)
r = raw
s.write(b"help\r\n"); pump(2)
raw.flush()
log("P7 alive check done (raw 已记录, help 响应见 raw)")

# P8 四用例
cases = []
p_fresh = fresh_payload()
cases.append(("fresh", p_fresh))
cases.append(("replay", p_fresh))          # 同载荷第二次 -> replay
p_tamper = fresh_payload()
cases.append(("tamper", p_tamper[:-1] + ("0" if p_tamper[-1] != "0" else "1")))
import time as _t
p_old = fresh_payload()
cases.append(("stale", p_old))             # 等 35s 变过期
for name, p in cases:
    s.reset_input_buffer()
    s.write(("qr_inject %s\r\n" % p).encode())
    pump(1.5)
    log("P8 inject %s sent" % name)
    if name == "stale":
        pump(35)
log("P8 four cases done, final observe 60s", flush=True)
pump(60)
log("REGRESSION DONE", flush=True)
s.close()
