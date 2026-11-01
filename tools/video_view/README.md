# lowlat_view — MC632X 低延迟 RTSP 查看工具

PC 端 Python 脚本，用于直连查看富瀚 MC632X（RT-Thread）摄像头板通过 RTSP 推出的 H.264 流，
目标是把端到端延迟压到 **300ms 以内**（VLC 默认打开同一路流约有 1~2 秒延迟）。

- 默认地址：`rtsp://192.168.137.100:1240`（板子 `192.168.137.100`，PC 网口 `192.168.137.1`，网线直连）

## 环境

- Windows + Python 3（miniconda：`D:\QQQQianrushi\python\miniconda\python.exe`，直接用 `python` 命令即可）
- 依赖：`opencv-python`（本机已装 4.14.0；若缺失执行 `pip install opencv-python`）

## 用法

```bat
cd /d E:\Qqqqqrs\qianrsRTT\MC632X\_build\video_view

python lowlat_view.py                      :: 默认连 rtsp://192.168.137.100:1240
python lowlat_view.py --tcp                :: RTP 改走 TCP（UDP 被拦/异常时用）
python lowlat_view.py --scale 0.5          :: 大分辨率画面缩小一半显示
python lowlat_view.py rtsp://192.168.137.100:1240   :: 显式指定 URL
```

### 参数

| 参数 | 说明 |
| --- | --- |
| `url`（可选位置参数） | RTSP 地址，默认 `rtsp://192.168.137.100:1240` |
| `--tcp` | RTP 用 TCP 交错传输（默认 UDP） |
| `--scale 0.5` | 显示缩放倍数（只影响显示，不影响取流） |
| `--max-seconds 10` | 运行 N 秒后自动退出（0 = 一直运行），主要用于测试 |

### 快捷键

| 按键 | 功能 |
| --- | --- |
| `q` 或 `ESC` | 退出 |
| `s` | 截图，保存到脚本同目录 `shots\` 下（PNG，文件名含时间戳） |

窗口标题实时显示**实际 FPS** 和分辨率。

## 低延迟是怎么压下来的

1. **FFmpeg 低延迟参数**：通过环境变量 `OPENCV_FFMPEG_CAPTURE_OPTIONS` 传给 OpenCV 的 FFmpeg 后端：

   ```
   rtsp_transport;udp|fflags;nobuffer|flags;low_delay|max_delay;0|reorder_queue_size;0|probesize;32768
   ```

   | 参数 | 作用 |
   | --- | --- |
   | `rtsp_transport;udp` | RTP 走 UDP，避免 TCP 交错传输的队头阻塞和重传延迟 |
   | `fflags;nobuffer` | 关闭 FFmpeg 输入缓冲 |
   | `flags;low_delay` | 启用低延迟标志 |
   | `max_delay;0` | 不允许封装层缓冲延迟 |
   | `reorder_queue_size;0` | 关闭 UDP 包重排队列（抖动缓冲） |
   | `probesize;32768` | 减小流探测数据量，加快打开速度 |

2. **只取最新帧**：主循环里先用 `grab()` 解码并检查耗时——若几乎瞬时返回，说明取到的是
   解码队列里积压的旧帧，直接丢弃；直到 `grab()` 等待了一段时间（说明等到了新到的帧）才
   `retrieve()` 显示。因此显示端再卡，延迟也不会随时间累积（另尝试设置 `CAP_PROP_BUFFERSIZE=1`，
   后端不支持时自动忽略）。

3. **断流自动重连**：连续 15 次取流失败判定断流，自动重连最多 3 次；彻底失败时给出中文排查提示。

## 连不上 / 延迟仍大时的排查

**连不上：**
1. 检查网线，确认板子与 PC 直连；
2. 确认 PC 网口 IP 为 `192.168.137.1`（掩码 255.255.255.0）；
3. `ping 192.168.137.100` 确认板子在线；
4. 确认板端 RTSP 服务（vlcview）已启动、端口 1240；
5. VLC 能打开而本工具打不开时，加 `--tcp` 试试（部分网络环境会拦 UDP）。

**延迟仍大（>300ms）：**
- 确认画面里没有 B 帧：板端编码器建议关闭 B 帧、GOP/I 帧间隔适当调小；
- UDP 受防火墙影响时会出现丢包→画面跳变，可试 `--tcp`（延迟略高但稳）；
- 用本工具与 VLC 对比同一画面（如拍秒表），确认板端编码/发送侧本身没有缓冲。

## 文件说明

- `lowlat_view.py` — 主脚本
- `shots\` — 按 `s` 截图时自动创建的保存目录
