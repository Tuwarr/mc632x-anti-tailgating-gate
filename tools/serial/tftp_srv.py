# -*- coding: utf-8 -*-
"""极简只读 TFTP 服务器（octet），给 uboot tftp 下载固件用。
用法: python tftp_srv.py <目录> [绑定IP]
全程复用绑定 :69 的单一 socket 收发；支持 blksize/tsize 选项；
从 ACK 包学习客户端数据端口（部分客户端 OACK 后换端口）；超时重传。
"""
import socket, sys, os, time, struct

ROOT = sys.argv[1] if len(sys.argv) > 1 else "."
BIND = sys.argv[2] if len(sys.argv) > 2 else "0.0.0.0"
PORT = 69

def log(msg):
    print("[%s] %s" % (time.strftime("%H:%M:%S"), msg), flush=True)

srv = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind((BIND, PORT))
log("tftp root=%s bind=%s:%d waiting..." % (os.path.abspath(ROOT), BIND, PORT))

def send_err(addr, code, msg):
    srv.sendto(struct.pack(b">HH", 5, code) + msg.encode() + b"\x00", addr)

def handle_rrq(rrq_addr, data):
    parts = data[2:].split(b"\x00")
    fname = parts[0].decode("utf-8", "replace")
    opts = {}
    i = 2
    while i + 1 < len(parts) and parts[i]:
        try:
            opts[parts[i].decode("ascii", "lower")] = parts[i + 1].decode("ascii")
        except Exception:
            pass
        i += 2
    blksize = 512
    if "blksize" in opts:
        try: blksize = min(max(int(opts["blksize"]), 8), 65464)
        except ValueError: pass
    path = os.path.abspath(os.path.join(ROOT, fname))
    if not path.startswith(os.path.abspath(ROOT)):
        send_err(rrq_addr, 2, "access violation"); return
    if not os.path.isfile(path):
        log("RRQ %s from %s -> NOT FOUND" % (fname, rrq_addr))
        send_err(rrq_addr, 1, "file not found"); return
    size = os.path.getsize(path)
    log("RRQ %s from %s opts=%s blksize=%d %d bytes" % (fname, rrq_addr, opts, blksize, size))

    # OACK：回显 blksize + tsize
    if opts:
        oack = struct.pack(b">H", 6)
        if "blksize" in opts:
            oack += b"blksize\x00" + str(blksize).encode() + b"\x00"
        if "tsize" in opts:
            oack += b"tsize\x00" + str(size).encode() + b"\x00"
        srv.sendto(oack, rrq_addr)

    # 等 ACK0，学习数据端口（客户端可能换端口）；收不到就按原地址直接推数据
    data_addr = rrq_addr
    t0 = time.time()
    got_ack0 = False
    while time.time() - t0 < 3:
        srv.settimeout(0.5)
        try:
            d, a = srv.recvfrom(65535)
        except socket.timeout:
            continue
        op = struct.unpack(b">H", d[:2])[0]
        if op == 4:
            data_addr = a
            got_ack0 = True
            log("ACK blk=%d from %s" % (struct.unpack(b">H", d[2:4])[0], a))
            break
        elif op == 1:  # 客户端重发 RRQ（没听懂 OACK），重发 OACK
            srv.sendto(oack, rrq_addr)
        # 其它包忽略
    if not got_ack0:
        log("no ACK0, falling back to rrq addr %s" % (rrq_addr,))

    with open(path, "rb") as f:
        blk = 1
        pending = f.read(blksize)
        retries = 0
        srv.settimeout(2)
        while True:
            srv.sendto(struct.pack(b">HH", 3, blk & 0xFFFF) + pending, data_addr)
            try:
                d, a = srv.recvfrom(65535)
                op, n = struct.unpack(b">HH", d[:4])
                if op == 4 and n == (blk & 0xFFFF):
                    if not pending:
                        break  # 空块确认，结束
                    data_addr = a
                    blk += 1
                    pending = f.read(blksize)
                    retries = 0
                else:
                    retries += 1
            except socket.timeout:
                retries += 1
                if retries > 12:
                    log("TIMEOUT blk %d -> give up" % blk); return
    log("DONE %s (%d blocks, last=%d bytes)" % (fname, blk, len(pending)))

while True:
    try:
        data, addr = srv.recvfrom(65535)
    except ConnectionResetError:
        # Windows: 客户端(探针/UBOOT)关端口后 ICMP 复位毒化 UDP socket,
        # 下一次 recvfrom 必抛 10054 —— 吞掉继续, 否则服务器直接死
        continue
    op = struct.unpack(b">H", data[:2])[0]
    if op == 1:
        srv.settimeout(None)
        try:
            handle_rrq(addr, data)
        except Exception as e:
            log("ERROR %r" % e)
            try: send_err(addr, 0, str(e))
            except Exception: pass
        srv.settimeout(None)
    else:
        log("ignore op=%d from %s" % (op, addr))
