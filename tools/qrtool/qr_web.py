# -*- coding: utf-8 -*-
"""
qr_web.py — 手机亮码 Web 服务 (配合 MC632X qr_gate 固件)

手机浏览器打开 http://<本机IP>:8000 即可显示动态二维码:
  - PC 每 20 秒换 nonce 并重新生成二维码(密钥不出PC)
  - 每轮自动向板端(192.168.137.100:9000)同步 TIME
  - 页面每 8 秒自动刷新二维码图
  - 页面带"远程开门"按钮和实时状态

依赖: qrcode, pillow (已随 qr_tool.py 安装)
用法: python qr_web.py [绑定端口默认8000]
"""
import io
import socket
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import qrcode

BOARD_IP = "192.168.137.100"
BOARD_PORT = 9000
DEMO_KEY = b"mc632x-demo-key-2026"   # !!! 量产必须改 !!!
DOOR_USER = "d1"
ROTATE_SEC = 8                       # nonce 轮换周期(与页面刷新同步, 防重放: 一次开门后等下一轮即可)
QR_PNG_PATH = "/qr.png"

import hmac
import hashlib

_state = {
    "png_bytes": b"",
    "payload": "",
    "status": "starting...",
    "tg": "TG (pending)",
    "lock": threading.Lock(),
}


def board_cmd(msg, timeout=3.0):
    """向板端 TCP 9000 发一条命令, 返回回复文本"""
    try:
        s = socket.create_connection((BOARD_IP, BOARD_PORT), timeout=timeout)
        s.sendall(msg.encode())
        time.sleep(0.2)
        try:
            reply = s.recv(128).decode(errors="replace").strip()
        except Exception:
            reply = "<no reply>"
        s.close()
        return reply
    except Exception as e:
        return "<board unreachable: %s>" % e


def make_qr_png(payload):
    # 低纠错(L)减少模块数, 摄像头远距解码更稳; border=4 保证静区;
    # box_size=26 让 PNG 原生约 1066px, 手机全屏显示时无需浏览器拉伸(拉伸会糊模块边缘)
    qr = qrcode.QRCode(error_correction=qrcode.constants.ERROR_CORRECT_L,
                       box_size=26, border=4)
    qr.add_data(payload)
    qr.make(fit=True)
    img = qr.make_image(fill_color="black", back_color="white")
    buf = io.BytesIO()
    img.save(buf, format="PNG")
    return buf.getvalue()


def rotate_loop():
    """每 ROTATE_SEC 秒: 新 nonce -> 生成二维码 -> 给板校时
    nonce 必须是 8 位小写 hex(协议 QR_NONCE_LEN=8, 板端 is_lower_hex 校验);
    hmac 截断为前 32 hex(128 位, 与板端 QR_HMAC_HEX_LEN=32 一致)"""
    import secrets
    n = 0
    while True:
        n += 1
        ts = int(time.time())
        nonce = secrets.token_hex(4)
        msg = "%s|%d|%s" % (DOOR_USER, ts, nonce)
        sig = hmac.new(DEMO_KEY, msg.encode("ascii"), hashlib.sha256).hexdigest().lower()[:32]
        payload = msg + "|" + sig
        png = make_qr_png(payload)
        reply = board_cmd("TIME %d\n" % ts)
        with _state["lock"]:
            _state["png_bytes"] = png
            _state["payload"] = payload
            _state["status"] = "payload #%d, board TIME sync: %s" % (n, reply)
        print("[rotate %d] %s | TIME sync: %s" % (n, payload, reply), flush=True)
        time.sleep(ROTATE_SEC)


def tg_poll_loop():
    """每 1.5s 从板端拉防尾随状态(TCP STATUS), 页面轮询 /status 时直接取缓存"""
    while True:
        reply = board_cmd("STATUS\n", timeout=2.0).strip()
        with _state["lock"]:
            _state["tg"] = reply if reply.startswith("TG") else "TG (board unreachable)"
        time.sleep(5.0)  # 5s: 降低lwIP连接churn(高频connect/close会耗尽TIME_WAIT pcb)


PAGE = """<!DOCTYPE html>
<html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>MC632X 门禁亮码</title>
<style>
body{font-family:sans-serif;text-align:center;background:#111;color:#eee;margin:0;padding:16px}
img{width:100vw;max-width:none;background:#fff;padding:0;border-radius:0;display:block}
a.ctrl{display:inline-block;font-size:16px;padding:10px 24px;margin:10px;border-radius:8px;background:#2d8cf0;color:#fff;text-decoration:none}
#st{font-size:13px;color:#9ad;margin-top:8px;word-break:break-all}
h3{margin:6px}
</style></head><body>
<h3>MC632X 门禁 · 动态二维码</h3>
<img id="qr" src="/qr.png?t=0">
<div id="tg" style="font-size:20px;font-weight:bold;padding:12px;margin:10px auto;max-width:92vw;border-radius:10px;background:#234;color:#cde">连接中…</div>
<div id="st">loading...</div>
<a class="ctrl" href="/ctrl">远程开关门控制 →</a>
<script>
function refresh(){document.getElementById('qr').src='/qr.png?t='+Date.now();}
setInterval(refresh, 8000);
var lastAlarm=false, audio=null;
function beep(){
  try{
    audio=audio||new (window.AudioContext||window.webkitAudioContext)();
    var o=audio.createOscillator(), g=audio.createGain();
    o.connect(g); g.connect(audio.destination); o.frequency.value=880;
    g.gain.setValueAtTime(0.3, audio.currentTime);
    o.start(); o.stop(audio.currentTime+0.25);
  }catch(e){}
}
function updateTG(t){
  var el=document.getElementById('tg');
  var m=t.match(/state=(\\w+)/);
  var a=(t.match(/a=(\\d+)/)||[0,'?'])[1], b=(t.match(/b=(\\d+)/)||[0,'?'])[1];
  var st=m?m[1]:'?';
  if(st==='ALARM'){el.style.background='#c0392b';el.style.color='#fff';
    el.textContent='⚠ 尾随告警：通道内多人！(门外 '+a+' 人 / 门内 '+b+' 人)';
    if(!lastAlarm)beep(); lastAlarm=true;}
  else if(st==='OPEN'||st==='PASSING'){el.style.background='#8a6d1a';el.style.color='#fff';
    el.textContent='放行通过中… (门外 '+a+' 人 / 门内 '+b+' 人)'; lastAlarm=false;}
  else if(st==='IDLE'){el.style.background='#1e5c2e';el.style.color='#fff';
    el.textContent='通道空闲 · 欢迎扫码 (门外 '+a+' 人 / 门内 '+b+' 人)'; lastAlarm=false;}
  else el.textContent=t;
}
setInterval(function(){if(lastAlarm)beep();},1200);
setInterval(function(){fetch('/status').then(r=>r.text()).then(t=>{
  document.getElementById('st').textContent=t.split('\\n')[0]; updateTG(t);}).catch(()=>{});},5000);
fetch('/status').then(r=>r.text()).then(t=>{updateTG(t);}).catch(()=>{});
</script>
</body></html>"""

CTRL = """<!DOCTYPE html>
<html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>MC632X 门禁 · 远程控制</title>
<style>
body{font-family:sans-serif;text-align:center;background:#111;color:#eee;margin:0;padding:16px}
button{font-size:22px;padding:18px 46px;margin:14px;border:0;border-radius:12px;color:#fff}
#open{background:#1e9e4a}#close{background:#c0392b}
a.back{color:#9ad;font-size:14px}
#st{font-size:13px;color:#9ad;margin-top:8px;word-break:break-all}
#tg{font-size:20px;font-weight:bold;padding:12px;margin:10px auto;max-width:92vw;border-radius:10px;background:#234;color:#cde}
h3{margin:6px}
</style></head><body>
<h3>MC632X 门禁 · 远程开关门</h3>
<div id="tg" style="">连接中…</div>
<div>
  <button id="open" onclick="cmd('/openh')">开门(保持)</button>
  <button id="close" onclick="cmd('/close')">关 门</button>
</div>
<div id="st">loading...</div>
<a class="back" href="/">← 返回亮码页</a>
<script>
var lastAlarm=false, audio=null;
function beep(){
  try{
    audio=audio||new (window.AudioContext||window.webkitAudioContext)();
    var o=audio.createOscillator(), g=audio.createGain();
    o.connect(g); g.connect(audio.destination); o.frequency.value=880;
    g.gain.setValueAtTime(0.3, audio.currentTime);
    o.start(); o.stop(audio.currentTime+0.25);
  }catch(e){}
}
function updateTG(t){
  var el=document.getElementById('tg');
  var m=t.match(/state=(\\w+)/);
  var a=(t.match(/a=(\\d+)/)||[0,'?'])[1], b=(t.match(/b=(\\d+)/)||[0,'?'])[1];
  var st=m?m[1]:'?';
  if(st==='ALARM'){el.style.background='#c0392b';el.style.color='#fff';
    el.textContent='⚠ 尾随告警：通道内多人！(门外 '+a+' 人 / 门内 '+b+' 人)';
    if(!lastAlarm)beep(); lastAlarm=true;}
  else if(st==='OPEN'||st==='PASSING'){el.style.background='#8a6d1a';el.style.color='#fff';
    el.textContent='放行通过中… (门外 '+a+' 人 / 门内 '+b+' 人)'; lastAlarm=false;}
  else if(st==='IDLE'){el.style.background='#1e5c2e';el.style.color='#fff';
    el.textContent='通道空闲 (门外 '+a+' 人 / 门内 '+b+' 人)'; lastAlarm=false;}
  else el.textContent=t;
}
function cmd(u){
  fetch(u).then(r=>r.text()).then(t=>{document.getElementById('st').textContent='板端: '+t;}).catch(()=>{});
}
setInterval(function(){if(lastAlarm)beep();},1200);
setInterval(function(){fetch('/status').then(r=>r.text()).then(t=>{
  document.getElementById('st').textContent=t.split('\\n')[0]; updateTG(t);}).catch(()=>{});},5000);
fetch('/status').then(r=>r.text()).then(t=>{updateTG(t);}).catch(()=>{});
</script>
</body></html>"""


class Handler(BaseHTTPRequestHandler):
    def _send(self, code, ctype, body):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path.startswith("/qr.png"):
            with _state["lock"]:
                png = _state["png_bytes"]
            self._send(200, "image/png", png)
        elif self.path.startswith("/status"):
            with _state["lock"]:
                st = _state["status"] + "\n" + _state.get("tg", "TG (pending)")
            self._send(200, "text/plain; charset=utf-8", st.encode("utf-8"))
        elif self.path.startswith("/ctrl"):
            self._send(200, "text/html; charset=utf-8", CTRL.encode("utf-8"))
        elif self.path.startswith("/openh"):
            reply = board_cmd("OPENHOLD\n")
            self._send(200, "text/plain; charset=utf-8", ("board: " + reply).encode("utf-8"))
        elif self.path.startswith("/close"):
            reply = board_cmd("CLOSE\n")
            self._send(200, "text/plain; charset=utf-8", ("board: " + reply).encode("utf-8"))
        elif self.path.startswith("/open"):
            reply = board_cmd("OPEN\n")
            self._send(200, "text/plain; charset=utf-8", ("board: " + reply).encode("utf-8"))
        else:
            self._send(200, "text/html; charset=utf-8", PAGE.encode("utf-8"))

    def log_message(self, fmt, *args):
        pass  # 静音访问日志


if __name__ == "__main__":
    import sys
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8000
    threading.Thread(target=rotate_loop, daemon=True).start()
    threading.Thread(target=tg_poll_loop, daemon=True).start()
    time.sleep(0.5)  # 等首轮二维码生成
    # 找本机局域网 IP, 打印手机可访问的地址
    ips = []
    try:
        import socket as _s
        hostname = _s.gethostname()
        for info in _s.getaddrinfo(hostname, None, _s.AF_INET):
            ip = info[4][0]
            if not ip.startswith("127.") and ip not in ips:
                ips.append(ip)
    except Exception:
        pass
    print("手机浏览器打开以下任一地址(与PC同一Wi-Fi/热点):", flush=True)
    for ip in ips:
        print("  http://%s:%d/" % (ip, port), flush=True)
    srv = ThreadingHTTPServer(("0.0.0.0", port), Handler)
    print("serving on 0.0.0.0:%d ..." % port, flush=True)
    srv.serve_forever()
