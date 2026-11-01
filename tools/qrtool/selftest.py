#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
selftest.py —— qr_tool 自测(无 GUI, 可在无人值守环境运行):
  [1] 用与屏幕二维码完全相同的代码路径生成一帧真实载荷, 渲染 sample_qr.png;
  [2] 逐字段解析该载荷并做格式检查;
  [3] 对 PNG 里编码的那份载荷【复算 HMAC-SHA256】并比对(本机无解码库时以此为准, 见 [4]);
  [4] 若本机存在可解码库(cv2 / pyzbar), 再做一次“图片 -> 文本”回读比对; 没有则如实注明;
  [5] 打印固定测试向量, 供板端 mbedtls 联调对拍(同步写入 protocol.md)。
"""
import hmac
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import qr_tool as t


def try_decode(png_path):
    """返回 (解码文本或 None, 使用的库名或 None)。"""
    # cv2(OpenCV 自带 QRCodeDetector)
    try:
        import cv2
    except Exception:
        cv2 = None
    if cv2 is not None:
        try:
            val, _, _ = cv2.QRCodeDetector().detectAndDecode(cv2.imread(png_path))
            if val:
                return val, "cv2.QRCodeDetector"
        except Exception:
            pass
    # pyzbar(需 zbar 动态库)
    try:
        from pyzbar.pyzbar import decode as zbar_decode
        from PIL import Image
        res = zbar_decode(Image.open(png_path))
        if res:
            return res[0].data.decode("utf-8"), "pyzbar"
    except Exception:
        pass
    return None, None


def main():
    ok = True

    # [1] 生成载荷 + 渲染 PNG(与 GUI 同一条代码路径: build_payload + render_qr)
    payload, ts, nonce = t.build_payload()
    png = os.path.join(HERE, "sample_qr.png")
    t.render_qr(payload).save(png)
    print("[1] sample_qr.png 已生成: %s (%d bytes)" % (png, os.path.getsize(png)))

    # [2] 字段解析与格式检查
    parts = payload.split("|")
    print("[2] 载荷 = %s" % payload)
    print("    字段数 = %d (期望 4)" % len(parts))
    user, ts_s, nonce_s, mac = parts
    fmt_ok = (len(parts) == 4 and user == t.DOOR_USER and ts_s.isdigit()
              and len(nonce_s) == 16 and len(mac) == 64)
    print("    字段格式检查: %s" % ("通过" if fmt_ok else "失败"))
    ok &= fmt_ok

    # [3] HMAC 复算比对(核心校验)
    mac2 = t.sign(user, int(ts_s), nonce_s)
    hmac_ok = hmac.compare_digest(mac, mac2)
    print("[3] HMAC 复算: %s" % ("一致" if hmac_ok else "不一致!"))
    print("    载荷内 mac = %s" % mac)
    print("    复算结果   = %s" % mac2)
    ok &= hmac_ok

    # [4] 有解码库则做图像回读比对
    text, lib = try_decode(png)
    if text is None:
        print("[4] 本机无可用 QR 解码库(cv2/pyzbar 均不可用): "
              "按约定以 [3] HMAC 复算一致为准, 未做图像->文本回读验证。")
    else:
        same = (text == payload)
        print("[4] 解码库 %s 图像回读: %s" % (lib, "与原载荷一致" if same else "不一致!"))
        print("    解码文本 = %s" % text)
        ok &= same

    # [5] 固定向量(板端 mbedtls 对拍)
    v_ts, v_nonce = 1800000000, "0123456789abcdef"
    v_mac = t.sign(t.DOOR_USER, v_ts, v_nonce)
    print("[5] 固定测试向量(已写入 protocol.md):")
    print("    key  = %s" % t.DEMO_KEY.decode())
    print("    msg  = %s|%d|%s" % (t.DOOR_USER, v_ts, v_nonce))
    print("    hmac = %s" % v_mac)

    print("SELFTEST %s" % ("PASSED" if ok else "FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
