# 构建与烧录完整流程（含六大关键坑）

> 目标：从源码编出可运行的 `media_demo.bin/.img`，并烧进/拉起 MC632X 板卡。
> 本文每一条坑都是实测踩出来的，**跳过任何一步都会复现对应的故障**。

## 0. 环境

- 富瀚 SDK：FH_RT_V3.4.0（rt-thread/ 目录）
- WSL + 工具链 `arm-fullhanv2-eabi-`（gcc 6.3.1）
- PC 端 Python 3（opencv / qrcode / pillow / pyserial）

## 1. 两套配置必须同时正确（坑①：启动即崩的根因）

| 配置 | 位置 | 管什么 |
|---|---|---|
| 顶层 `rt-thread/.config` | 经 `cfg2hdr.py` 生成 `out/rtconfig/rtconfig.h` | 平台库（libplatform.a） |
| 应用 `app/media_demo/.config` | 经 `menu_anno.py` 生成 `config.h` | 应用代码 |

**关键**：顶层必须选 `CONFIG_APP_CONFIG_MEDIA_DEMO=y`（原厂 defconfig 默认是
HELLOWORLD！），它会 select 出 `FH_ENABLE_VIDEO`。否则 `board_startup.c` 的
`video_init()`（媒体/ISP/VENC 全部初始化）被编译剔除，链接器 --gc-sections
把媒体初始化整条链回收——**启动后媒体系统静默缺失**。

## 2. 构建必须走根目录完整流程（坑②：链接缺斤短两）

```bash
# WSL 内（工具链在 PATH）
cd $SDKROOT/rt-thread
make lib -j4     # 先编平台库（会重生成 rtconfig.h + appconfig.h + mem.ld）
make app         # 再编应用（SAMPLE_DIR 由顶层 APP_CONFIG 决定 → media_demo）
```

**不要**直接 `make -C app/media_demo`——会跳过 mem.ld/appconfig 再生，
链接产物缺 3 KB 左右且行为异常（实测：启动 prefetch abort / 管线缺失）。

## 3. bin 必须过 gengz（坑③：裸 bin 直接 go 必崩）

`make app` 末尾会调 `build/tools/gengz.sh`：objcopy 之后**就地改写 bin**
（0x82000 边界插入 8 字节魔数、4K 对齐、尾部追加标记，共 +3,248 字节）。
引导链靠这些标记定位 stage2——**没过 gengz 的裸 bin 直接 `go` = prefetch abort**。

自查方法：

```python
data = open("media_demo.bin","rb").read()
print(data[-8:].hex())          # 结尾应是 fdc93b7a
print(data[0x81ff0:0x82010].hex())  # 应含 9f5e7b3afdc93b7a
```

镜像 `.img` 同理由 gengz 生成，烧 Flash 用 img，RAM 启动用处理后的 bin。

## 4. 管线需要手动 vlcview 拉起（坑④：自动启动在本配置下不生效）

板子启动到 msh 后，媒体管线不会自动运行（qr_scan 会对着不存在的通道
每 200ms 报 lock 错误并最终拖死系统）。标准启动顺序：

```
msh /> ifconfig e0 192.168.137.100     # RAM 启动时必须手动配网
msh /> vlcview -p 1234                 # 先起媒体管线（Sensor/ISP/VENC/RTSP）
msh /> qr_gate                         # 再起门禁（扫码+TCP+舵机+防尾随）
```

顺序颠倒或漏掉 vlcview → NN/扫码线程空转 → 系统十几分钟后无声死亡。

## 5. 烧 Flash 前先 sf probe（坑⑤）

U-Boot 冷启动后 `sf` 未探测设备，直接 erase/write 会报
"No SPI flash selected"：

```
U-Boot> tftp 40000000 media_demo.img     # 核对 Bytes transferred
U-Boot> sf probe 0
U-Boot> sf erase 0x120000 0x300000       # 只动 app 分区
U-Boot> sf write 40000000 0x120000 0x300000
U-Boot> sf read 42000000 0x120000 0x300000
U-Boot> cmp.b 40000000 42000000 <img长度hex>   # 回读逐字节校验
```

- NBG 模型分区在 0x700000（人形检测模型），**严禁擦到**；
- 板卡热复位会进 ROM XMODEM 模式（板卡固有行为），重上电才是冷启动。

## 6. RAM 启动等 msh 就绪再发命令（坑⑥：启动竞态吃命令）

`go a0000000` 后系统启动约需 20~25 秒。过早发送 `ifconfig/vlcview/qr_gate`
会被吞掉（表现为命令无响应、管线没起）。正确做法：轮询串口直到出现
`msh` 提示符，再依次发命令（参考 `tools/debug/v5_regression.py`）。

## 7. 串口工具

| 工具 | 用途 |
|---|---|
| `ser_probe.py` | 探测串口/板卡状态 |
| `ser_toub.py` / `ser_powercatch.py` | 复位进 U-Boot / 上电瞬间自动打断 autoboot |
| `ser_cmd.py` | 批量发 U-Boot/msh 命令 |
| `tftp_srv.py` | PC 端 TFTP 服务器（用法：`python tftp_srv.py <目录>`） |
| `xmodem_send.py` | ROM XMODEM 救砖（ Flash 引导损坏时恢复 U-Boot） |

## 8. 已知遗留

- OSD gbox 在部分帧报 `0xa0c84002`（SDK OSD 显示层），不影响检测与推流；
- NN `now=` 时间戳显示值异常（判定逻辑正确，纯打印问题）；
- LCD_D12/D13 被 PWM 复用后 LCD 少两个颜色位（轻微色偏）。
