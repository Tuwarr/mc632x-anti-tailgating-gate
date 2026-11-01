#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
lowlat_view.py -- MC632X (RT-Thread) 摄像头板低延迟 RTSP 取流查看器

用法:
    python lowlat_view.py                                  # 默认 rtsp://192.168.137.100:1240
    python lowlat_view.py rtsp://192.168.137.100:1240      # 显式指定 URL
    python lowlat_view.py --tcp                            # UDP 不通时改走 TCP 交错传输
    python lowlat_view.py --scale 0.5                      # 大分辨率画面缩小显示

快捷键:
    q / ESC    退出
    s          截图, 保存到脚本同目录 shots\\ 下

低延迟要点:
    1. 通过环境变量 OPENCV_FFMPEG_CAPTURE_OPTIONS 把以下参数传给 FFmpeg:
         rtsp_transport;udp      RTP 走 UDP, 避免 TCP 交错传输的队头阻塞与重传延迟
         fflags;nobuffer         关闭 FFmpeg 输入缓冲
         flags;low_delay         启用低延迟标志
         max_delay;0             不允许封装层缓冲延迟
         reorder_queue_size;0    关闭 UDP 包重排队列(抖动缓冲)
         probesize;32768         减小流探测数据量, 加快打开速度
    2. 只显示最新帧: grab() 几乎瞬时返回, 说明取到的是解码队列里积压的旧帧,
       直接丢弃; 直到 grab() 花费明显时间(等待新帧到达)才 retrieve() 显示。
       这样显示端消费速度再慢, 延迟也不会随时间累积。
    3. 尝试设置 CAP_PROP_BUFFERSIZE=1 (仅部分后端支持, 不支持时静默忽略)。
    4. 连接前先用原生 socket 预检 RTSP 端口(约 4 秒超时), 秒级区分
       "端口拒绝连接(vlcview 未启动)"与"网络不可达(网线/IP 问题)",
       避免连接失败时在 FFmpeg 内部干等约 30 秒才报错。
"""

import argparse
import os
import socket
import sys
import time
from collections import deque
from datetime import datetime
from pathlib import Path
from urllib.parse import urlparse

SCRIPT_DIR = Path(__file__).resolve().parent
SHOTS_DIR = SCRIPT_DIR / "shots"

DEFAULT_URL = "rtsp://192.168.137.100:1240"

# 除 rtsp_transport 外共用的低延迟参数
FFMPEG_OPTS_COMMON = (
    "fflags;nobuffer|flags;low_delay|max_delay;0"
    "|reorder_queue_size;0|probesize;32768"
)

# 必须在打开 VideoCapture 之前设置好默认值(UDP); opencv 在每次打开时读取该环境变量
os.environ["OPENCV_FFMPEG_CAPTURE_OPTIONS"] = "rtsp_transport;tcp|" + FFMPEG_OPTS_COMMON

try:
    import cv2
except ImportError:
    cv2 = None

WIN_NAME = "MC632X lowlat"
MAX_OPEN_TRIES = 3          # 每轮"连不上/断流"的最大尝试次数
RETRY_DELAY = 1.0           # 重试间隔(秒)
STALE_GRAB_SEC = 0.003      # grab() 耗时低于该值 => 取到的是积压旧帧, 应丢弃
MAX_CONSECUTIVE_SKIP = 120  # 连续丢弃旧帧的上限, 防止极端情况下画面长时间不刷新
READ_FAIL_LIMIT = 15        # 连续 grab 失败达到该次数 => 判定断流
STUCK_GRAB_SEC = 5.0        # grab() 阻塞超过该秒数后失败 => 等不到数据, 直接判定断流
FPS_WINDOW = 2.0            # FPS 统计滑动窗口(秒)
PREFLIGHT_TIMEOUT = 4.0     # 连接前 socket 预检超时(秒)


def ffmpeg_opts(use_tcp):
    """根据传输方式拼出完整的 OPENCV_FFMPEG_CAPTURE_OPTIONS 值。"""
    transport = "tcp" if use_tcp else "udp"
    return "rtsp_transport;%s|%s" % (transport, FFMPEG_OPTS_COMMON)


def preflight_tcp(url):
    """连接前用原生 socket 快速探测 RTSP 控制端口。

    返回 (state, host, port), state 取值:
      'ok'          TCP 可连, 继续走 cv2/FFmpeg 打开流程
      'refused'     端口拒绝连接: 板子在线但 RTSP 服务(vlcview)可能未启动
      'unreachable' 网络不可达/超时: 检查网线和 IP
      'skip'        URL 无法解析出主机名, 跳过预检
    """
    try:
        parts = urlparse(url)
        host = parts.hostname
        port = parts.port or 554
    except Exception:
        return "skip", None, None
    if not host:
        return "skip", None, None
    try:
        sock = socket.create_connection((host, port), timeout=PREFLIGHT_TIMEOUT)
        sock.close()
        return "ok", host, port
    except ConnectionRefusedError:
        return "refused", host, port
    except (socket.timeout, OSError):
        return "unreachable", host, port


def open_capture(url, use_tcp):
    """打开 RTSP 流。返回 (capture, fail_reason, host, port)。

    预检失败时 capture 为 None, fail_reason 为 'refused'/'unreachable';
    否则 fail_reason 为 None, 是否成功由调用方用 isOpened() 判断。
    """
    state, host, port = preflight_tcp(url)
    if state in ("refused", "unreachable"):
        return None, state, host, port
    os.environ["OPENCV_FFMPEG_CAPTURE_OPTIONS"] = ffmpeg_opts(use_tcp)
    cap = cv2.VideoCapture(url, cv2.CAP_FFMPEG)
    if cap.isOpened():
        try:
            # FFMPEG 后端可能不支持该属性, 失败则忽略(靠 grab 跳帧兜底)
            cap.set(cv2.CAP_PROP_BUFFERSIZE, 1)
        except Exception:
            pass
    return cap, None, host, port


def print_connect_help(url):
    """连接失败时打印中文排查指引。"""
    print("")
    print("[错误] 无法连接 RTSP 流: %s" % url)
    print("请按以下顺序排查:")
    print("  1. 检查网线是否插好, 板子与 PC 是否直连")
    print("  2. 确认 PC 网口 IP 已固定为 192.168.137.1 (子网掩码 255.255.255.0)")
    print("  3. 在 cmd 里执行: ping 192.168.137.100   确认板子在线")
    print("  4. 确认板端 RTSP 服务(vlcview)已启动, 端口 1240")
    print("  5. 若 VLC 能打开而本工具打不开, 尝试加参数 --tcp (RTP 改走 TCP)")
    print("")


def save_snapshot(frame):
    """截图保存到脚本同目录 shots/ 下, 返回保存路径或 None。"""
    try:
        SHOTS_DIR.mkdir(parents=True, exist_ok=True)
        stamp = datetime.now().strftime("%Y%m%d_%H%M%S_%f")[:-3]
        path = SHOTS_DIR / ("shot_%s.png" % stamp)
        ok = cv2.imwrite(str(path), frame)
    except Exception as e:
        print("[截图] 保存失败: %s: %s" % (type(e).__name__, e))
        return None
    if ok:
        return str(path)
    print("[截图] 保存失败: cv2.imwrite 返回 False, 请检查 shots 目录权限")
    return None


def stream_loop(cap, args):
    """取流 + 显示主循环。返回 'quit' | 'timeout' | 'lost'。"""
    disp_times = deque()        # 用于统计显示 FPS 的滑动窗口
    read_fails = 0
    skips = 0
    last_frame = None
    last_title = ""
    last_title_t = 0.0
    first_frame_printed = False
    t_start = time.perf_counter()

    while True:
        t0 = time.perf_counter()
        ok = cap.grab()
        grab_dt = time.perf_counter() - t0
        if not ok:
            if grab_dt > STUCK_GRAB_SEC:
                # 长时间阻塞后才失败: 典型的等不到数据(OpenCV 约 30s 超时中断),
                # 说明流已断或无数据, 不必再累计失败次数
                print("[警告] 取流阻塞 %.1f 秒后失败, 判定断流"
                      " (若 UDP 被网络环境拦截, 请加 --tcp 重试)" % grab_dt)
                return "lost"
            read_fails += 1
            if read_fails >= READ_FAIL_LIMIT:
                print("[警告] 连续 %d 次取流失败, 判定断流" % READ_FAIL_LIMIT)
                return "lost"
            continue
        read_fails = 0

        # grab() 几乎瞬时返回 => 该帧是内部积压的旧帧, 丢弃, 保证只显示最新帧
        if grab_dt < STALE_GRAB_SEC and skips < MAX_CONSECUTIVE_SKIP:
            skips += 1
            continue
        skips = 0

        ok, frame = cap.retrieve()
        if not ok or frame is None:
            continue
        last_frame = frame

        if not first_frame_printed:
            first_frame_printed = True
            print("[OK] 收到首帧: %dx%d" % (frame.shape[1], frame.shape[0]))

        now = time.perf_counter()
        disp_times.append(now)
        while disp_times and (now - disp_times[0]) > FPS_WINDOW:
            disp_times.popleft()
        if len(disp_times) > 1:
            fps = (len(disp_times) - 1) / (disp_times[-1] - disp_times[0])
        else:
            fps = 0.0

        shown = frame
        if args.scale != 1.0:
            interp = cv2.INTER_AREA if args.scale < 1.0 else cv2.INTER_LINEAR
            shown = cv2.resize(frame, None, fx=args.scale, fy=args.scale,
                               interpolation=interp)
        cv2.imshow(WIN_NAME, shown)

        # 窗口标题显示实际 FPS, 限频刷新(0.25s 一次)
        title = "MC632X  %.1f FPS  %dx%d  [q:quit  s:snap]" % (
            fps, frame.shape[1], frame.shape[0])
        if (hasattr(cv2, "setWindowTitle") and title != last_title
                and (now - last_title_t) > 0.25):
            cv2.setWindowTitle(WIN_NAME, title)
            last_title = title
            last_title_t = now

        key = cv2.waitKey(1) & 0xFF
        if key in (ord('q'), ord('Q'), 27):     # q / ESC 退出
            return "quit"
        if key in (ord('s'), ord('S')) and last_frame is not None:
            path = save_snapshot(last_frame)
            if path:
                print("[截图] 已保存: %s" % path)

        if args.max_seconds > 0 and (now - t_start) >= args.max_seconds:
            return "timeout"


def main():
    parser = argparse.ArgumentParser(
        description="MC632X (RT-Thread) 摄像头板低延迟 RTSP 查看器",
        epilog=(
            "示例:\n"
            "  python lowlat_view.py\n"
            "  python lowlat_view.py rtsp://192.168.137.100:1240\n"
            "  python lowlat_view.py --tcp          # UDP 不通/延迟异常时改走 TCP\n"
            "  python lowlat_view.py --scale 0.5    # 大分辨率缩小显示\n"
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("url", nargs="?", default=DEFAULT_URL,
                        help="RTSP 地址 (默认: %(default)s)")
    parser.add_argument("--tcp", action="store_true", default=True,
                        help="RTP 用 TCP 交错传输 (现在默认 TCP)")
    parser.add_argument("--udp", dest="tcp", action="store_false",
                        help="强制 RTP 走 UDP (默认已改为 TCP)")
    parser.add_argument("--scale", type=float, default=1.0,
                        help="显示缩放倍数, 如 0.5 (默认 1.0)")
    parser.add_argument("--max-seconds", type=float, default=0.0,
                        help="运行指定秒数后自动退出, 0 表示一直运行 (默认 0)")
    args = parser.parse_args()

    if args.scale <= 0:
        print("[错误] --scale 必须大于 0")
        return 2

    if cv2 is None:
        print("[错误] 未找到 OpenCV (cv2), 请先安装:")
        print("  pip install opencv-python")
        return 2

    print("[连接] %s  (RTP 传输: %s)" % (args.url, "TCP" if args.tcp else "UDP"))
    window_ready = False
    try_count = 0
    while True:
        try_count += 1
        cap, fail_reason, host, port = open_capture(args.url, args.tcp)
        if cap is None or not cap.isOpened():
            if cap is not None:
                try:
                    cap.release()
                except Exception:
                    pass
            if fail_reason == "refused":
                print("[警告] %s 端口 %s 拒绝连接, 板子在线但 RTSP 服务(vlcview)可能未启动"
                      % (host, port))
            elif fail_reason == "unreachable":
                print("[警告] 无法建立到 %s:%s 的 TCP 连接" % (host, port))
            else:
                print("[警告] RTSP 初始化失败 (TCP 可连, 握手/取流阶段出错)")
            if try_count < MAX_OPEN_TRIES:
                print("[警告] 连接失败, %.0f 秒后重试 (%d/%d)"
                      % (RETRY_DELAY, try_count, MAX_OPEN_TRIES))
                time.sleep(RETRY_DELAY)
                continue
            print_connect_help(args.url)
            return 1

        try_count = 0
        if not window_ready:
            cv2.namedWindow(WIN_NAME, cv2.WINDOW_AUTOSIZE)
            window_ready = True
        print("[OK] 已连接, 取流中...  (q=退出  s=截图)")

        result = stream_loop(cap, args)
        try:
            cap.release()
        except Exception:
            pass

        if result == "quit":
            return 0
        if result == "timeout":
            print("[完成] 已运行 %.1f 秒, 自动退出" % args.max_seconds)
            return 0
        # 断流: 尝试重连(重连次数同样受 MAX_OPEN_TRIES 限制)
        print("[提示] 正在尝试重新连接...")
        time.sleep(RETRY_DELAY)


if __name__ == "__main__":
    try:
        _code = main()
    except KeyboardInterrupt:
        print("")
        print("[退出] Ctrl+C, 已手动退出")
        _code = 130
    except Exception as e:  # 兜底: 任何未预期异常都以中文提示结束, 不抛裸 traceback
        print("[错误] 发生未预期的异常: %s: %s" % (type(e).__name__, e))
        _code = 2
    finally:
        if cv2 is not None:
            try:
                cv2.destroyAllWindows()
            except Exception:
                pass
    sys.exit(_code)
