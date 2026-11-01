# -*- coding: utf-8 -*-
"""最小 XMODEM(128B+checksum) 发送器 v2:
- 只在开头等一次接收方轮询(NAK/C), 之后 ACK 后立即发下一块
- NAK 重发同一块, CAN 中止
用法: python xmodem_send.py <img路径> [总超时秒]
"""
import sys, time
import serial

PORT = "COM24"
BAUD = 115200
SOH, EOT, ACK, NAK, CAN = 0x01, 0x04, 0x06, 0x15, 0x18

path = sys.argv[1]
total_tmo = float(sys.argv[2]) if len(sys.argv) > 2 else 600
data = open(path, "rb").read()
if len(data) % 128:
    data += b"\x1a" * (128 - len(data) % 128)
blocks = [data[i:i+128] for i in range(0, len(data), 128)]
print("file=%s bytes=%d blocks=%d" % (path, len(data), len(blocks)))

ser = serial.Serial(PORT, BAUD, timeout=0.5, rtscts=False, dsrdtr=False)
ser.reset_input_buffer()

def wait_poll(sec):
    t0 = time.time()
    while time.time() - t0 < sec:
        c = ser.read(1)
        if c in (b"\x15", b"C"):
            return c
    return None

def send_block(num, payload):
    cks = sum(payload) & 0xFF
    pkt = bytes([SOH, num & 0xFF, (~num) & 0xFF]) + payload + bytes([cks])
    for attempt in range(15):
        ser.write(pkt)
        t0 = time.time()
        while time.time() - t0 < 5:
            c = ser.read(1)
            if c == b"\x06":
                return True
            if c in (b"\x15", b"\x18"):
                break            # NAK/CAN -> 重发/中止
        else:
            pass                 # 超时也重发
    return False

t0 = time.time()
poll = wait_poll(30)
if poll is None:
    print("no xmodem poll (NAK/C) seen in 30s, abort")
    sys.exit(1)
print("poll=%r, sending..." % (poll,))

ok = True
for i, payload in enumerate(blocks):
    if not send_block(i + 1, payload):
        print("block %d failed after retries" % (i + 1))
        ok = False
        break
    if (i + 1) % 300 == 0 or i + 1 == len(blocks):
        print("block %d/%d (%.0fs)" % (i + 1, len(blocks), time.time() - t0))

if not ok:
    sys.exit(1)

# EOT: 常规流程 NAK -> EOT -> ACK
for round_ in range(6):
    ser.write(bytes([EOT]))
    t0 = time.time()
    r = None
    while time.time() - t0 < 5:
        c = ser.read(1)
        if c:
            r = c
            break
    if r == b"\x06":
        print("EOT acked directly")
        break
    if r == b"\x15":
        continue
    # 超时继续
print("TRANSFER DONE in %.0fs" % (time.time() - t0,))

time.sleep(2)
out = ser.read(65536)
sys.stdout.buffer.write(out[-4000:])
print()
ser.close()
