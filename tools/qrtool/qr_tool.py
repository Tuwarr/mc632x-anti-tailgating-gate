#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
qr_tool.py —— MC632X 动态二维码开门 PC 端工具(Python 3.9 + tkinter)

功能:
  1. 每 25 秒自动刷新动态二维码(换 nonce 重新签名):
        载荷 = "dooruser1|<epoch秒>|<nonce>|<hmac_hex>"
        hmac = HMAC-SHA256(DEMO_KEY, "dooruser1|<epoch秒>|<nonce>") 的 64 位小写 hex
  2. 按钮“校时”   : TCP 连 192.168.137.100:9000, 发送 "TIME <epoch秒>\n"
  3. 按钮“远程开门”: 同端口发送 "OPEN\n"
  4. 所有网络操作都在工作线程执行且带 3 秒超时:
        连接失败只弹提示(messagebox), 绝不崩溃、绝不卡界面(无阻塞)。

运行:  python qr_tool.py
依赖:  python -m pip install --cache-dir E:/Qqqqqrs/qianrsRTT/MC632X/_build/qrpip qrcode pillow
协议:  见同目录 protocol.md(字段格式 / HMAC / 30s 时间窗 / nonce 防重放 / TIME·OPEN 命令)
"""

import hashlib
import hmac
import re
import secrets
import socket
import threading
import time

import tkinter as tk
from tkinter import messagebox

import qrcode
from qrcode.constants import ERROR_CORRECT_M
from PIL import ImageTk

# --------------------------------------------------------------- 关键参数 ----
DEMO_KEY = b"mc632x-demo-key-2026"  # !!! 量产必须改: 演示专用固定密钥, 出厂前必须更换(每台设备唯一并安全存放) !!!
DOOR_USER = "d1"                    # 载荷第一段(用户标识), 同时参与签名; 短 uid 让二维码更稀疏易扫
BOARD_IP = "192.168.137.100"        # 开发板 IP(网线直连)
BOARD_PORT = 9000                   # 板端 TCP 服务端口(TIME/OPEN 命令通道)
REFRESH_INTERVAL_S = 8              # 二维码刷新周期(秒), 必须小于板端 30s 时间窗
NET_TIMEOUT_S = 3                   # TCP 连接/收发超时(秒), 防止界面卡死


# ------------------------------------------------------------- 载荷 / HMAC ----
def make_nonce():
    """随机 nonce: 4 字节随机数 -> 8 个 hex 字符(协议 QR_NONCE_LEN=8), 每次刷新更换。"""
    return secrets.token_hex(4)


def sign(user, ts, nonce):
    """HMAC-SHA256(DEMO_KEY, "user|ts|nonce") -> 前 32 字符小写 hex(128 位截断)。

    注意: 被签名内容是【前三段原样拼接】(含分隔符 '|'),
          不包含第 4 段 hmac 本身 —— 与板端 mbedtls 校验严格一致;
          板端 QR_HMAC_HEX_LEN=32 做常量时间前缀比较。
    """
    msg = "%s|%d|%s" % (user, int(ts), nonce)
    return hmac.new(DEMO_KEY, msg.encode("ascii"), hashlib.sha256).hexdigest().lower()[:32]


def build_payload(now=None):
    """生成一帧二维码载荷 "dooruser1|<ts>|<nonce>|<hmac_hex>"。

    返回 (载荷文本, ts, nonce); now 参数仅供测试注入固定时间。
    """
    ts = int(time.time() if now is None else now)
    nonce = make_nonce()
    return "%s|%d|%s|%s" % (DOOR_USER, ts, nonce, sign(DOOR_USER, ts, nonce)), ts, nonce


def render_qr(text):
    """把文本渲染成二维码 PIL 图片(白底黑码, 纠错级别 L —— 模块最少, 摄像头远距更易解)。"""
    qr = qrcode.QRCode(error_correction=ERROR_CORRECT_L, box_size=7, border=4)
    qr.add_data(text)
    qr.make(fit=True)
    img = qr.make_image(fill_color="black", back_color="white")
    pil = img.get_image() if hasattr(img, "get_image") else img._img
    return pil.convert("RGB")


# ------------------------------------------------------------------ GUI ----
class QrApp(object):
    def __init__(self, root):
        self.root = root
        root.title("MC632X 动态二维码开门 · PC 端 (demo)")
        root.resizable(False, False)
        self.photo = None        # 持有 PhotoImage 引用, 防被 GC 后界面变白
        self.deadline = 0.0      # 当前二维码过期时刻(本地单调比较用本地时钟)
        self.closed = False
        self._build()
        self._refresh()
        self._tick()

    # ---------- 布局 ----------
    def _build(self):
        pad = dict(padx=10, pady=6)
        self.qr_label = tk.Label(self.root, bg="white")
        self.qr_label.grid(row=0, column=0, columnspan=3, **pad)

        self.payload_var = tk.StringVar(value="")
        tk.Label(self.root, textvariable=self.payload_var, wraplength=520,
                 justify="center", fg="#333333",
                 font=("Consolas", 9)).grid(row=1, column=0, columnspan=3, **pad)

        self.count_var = tk.StringVar(value="")
        tk.Label(self.root, textvariable=self.count_var,
                 fg="#007a4d").grid(row=2, column=0, columnspan=3, **pad)

        tk.Button(self.root, text="校时", width=14,
                  command=self.on_time).grid(row=3, column=0, **pad)
        tk.Button(self.root, text="远程开门", width=14,
                  command=self.on_open).grid(row=3, column=1, **pad)
        tk.Button(self.root, text="退出", width=14,
                  command=self.on_quit).grid(row=3, column=2, **pad)

        self.tg_var = tk.StringVar(value="防尾随状态: 连接中…")
        self.tg_label = tk.Label(self.root, textvariable=self.tg_var,
                                 fg="white", bg="#555555", width=58, pady=4)
        self.tg_label.grid(row=4, column=0, columnspan=3, **pad)
        self._tg_alarm = False
        self._tg_poll()

    # ---------- 二维码刷新(25s 换 nonce) ----------
    def _refresh(self):
        payload, ts, nonce = build_payload()
        self.deadline = time.time() + REFRESH_INTERVAL_S
        pil = render_qr(payload)
        self.photo = ImageTk.PhotoImage(pil)
        self.qr_label.config(image=self.photo)
        self.payload_var.set(payload)

    def _tick(self):
        if self.closed:
            return
        left = int(self.deadline - time.time())
        if left <= 0:
            self._refresh()
            left = REFRESH_INTERVAL_S
        self.count_var.set("下次刷新: %2d s   (板端有效期 30 s 时间窗)" % left)
        self.root.after(500, self._tick)

    # ---------- 防尾随状态轮询(TCP STATUS -> 标签变色 + 告警蜂鸣) ----------
    def _tg_poll(self):
        if self.closed:
            return
        threading.Thread(target=self._tg_fetch, daemon=True).start()
        self.root.after(5000, self._tg_poll)

    def _tg_fetch(self):
        line = ""
        try:
            s = socket.create_connection((BOARD_IP, BOARD_PORT), timeout=1.5)
            s.sendall(b"STATUS\n")
            time.sleep(0.2)
            line = s.recv(256).decode(errors="replace").strip()
            s.close()
        except Exception:
            line = ""
        self.root.after(0, lambda: self._tg_show(line))

    def _tg_show(self, line):
        if not line.startswith("TG"):
            self.tg_var.set("防尾随状态: 板端未连接(qr_gate 未启动或掉线)")
            self.tg_label.config(bg="#555555")
            self._tg_alarm = False
            return
        self.tg_var.set("防尾随: " + line)
        m = re.search(r"state=(\w+)", line)
        st = m.group(1) if m else "?"
        alarm = (st == "ALARM")
        if alarm:
            self.tg_label.config(bg="#c0392b")
            if not self._tg_alarm:
                self._tg_beep()
        elif st in ("OPEN", "PASSING"):
            self.tg_label.config(bg="#8a6d1a")
        else:
            self.tg_label.config(bg="#1e5c2e")
        self._tg_alarm = alarm

    def _tg_beep(self):
        try:
            import winsound
            winsound.Beep(1000, 350)
            winsound.Beep(1300, 350)
        except Exception:
            self.root.bell()

    # ---------- TCP 命令(工作线程执行, 不阻塞 UI) ----------
    def on_time(self):
        self._send_async("TIME %d\n" % int(time.time()), "校时")

    def on_open(self):
        self._send_async("OPEN\n", "远程开门")

    def _send_async(self, line, title):
        threading.Thread(target=self._net, args=(line, title), daemon=True).start()

    def _net(self, line, title):
        try:
            sock = socket.create_connection((BOARD_IP, BOARD_PORT),
                                            timeout=NET_TIMEOUT_S)
        except Exception as e:
            self._msg("warning", title + " 失败",
                      "无法连接 %s:%d\n\n%s\n\n请检查网线连通性, 以及板端 TCP 服务(端口 %d)是否已启动。"
                      % (BOARD_IP, BOARD_PORT, e, BOARD_PORT))
            return
        try:
            sock.settimeout(NET_TIMEOUT_S)
            sock.sendall(line.encode("ascii"))
            reply = sock.recv(256).decode("ascii", "replace").strip()
            if reply:
                self._msg("info", title + " 已发送",
                          "命令: %s\n板端回复: %s" % (line.strip(), reply))
            else:
                self._msg("info", title + " 已发送",
                          "命令: %s\n板端未回复(连接已关闭或无回包)。" % line.strip())
        except Exception as e:
            self._msg("warning", title + " 失败", "发送/接收异常: %s" % e)
        finally:
            try:
                sock.close()
            except Exception:
                pass

    def _msg(self, kind, title, text):
        """线程安全弹窗: 统一切回主线程; 窗口已关闭则静默丢弃。"""
        if self.closed:
            return

        def show():
            if self.closed:
                return
            if kind == "info":
                messagebox.showinfo(title, text)
            else:
                messagebox.showwarning(title, text)

        try:
            self.root.after(0, show)
        except RuntimeError:
            pass

    # ---------- 退出 ----------
    def on_quit(self):
        self.on_close()

    def on_close(self):
        self.closed = True
        try:
            self.root.destroy()
        except Exception:
            pass


def main():
    root = tk.Tk()
    app = QrApp(root)
    root.protocol("WM_DELETE_WINDOW", app.on_close)
    root.mainloop()


if __name__ == "__main__":
    main()
