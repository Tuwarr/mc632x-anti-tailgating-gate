# MC632X 双摄 AI 防尾随门禁系统

基于富瀚 MC632X (XC00) 智能视觉 SoC 的双摄像头防尾随门禁：双路 NNA 人形检测、动态签名二维码开门、双摄人数比对状态机、多端告警与远程控制。

## 系统组成

```
门外摄像头(A) ─ 人形检测 ─┐
门内摄像头(B) ─ 人形检测 ─┤→ 防尾随状态机 ─→ 舵机栏杆
手机二维码 ─── 摄像头扫码 ─┘        │
                                   ↓
PC/手机状态条(绿/黄/红+蜂鸣) ← TCP STATUS 轮询
手机控制页: 开门(保持) / 关门
```

- 一次授权只放行一个人：扫码开杆 → 人离开门外画面 → 杆在身后关闭 → 门内摄像头确认放行完成
- 两个人一起过 → 尾随告警（关杆 + 拒授权 + PC/手机红色蜂鸣）
- 二维码每 8 秒轮换，HMAC-SHA256 签名 + 30 秒时间窗 + nonce 防重放

## 仓库结构

```
board/     板端改动源码（覆盖到富瀚 SDK 对应路径后编译）
tools/     PC 端工具（串口烧录/二维码/低延迟查看/调试脚本）
firmware/  已验证固件（bin + img）
BUILD.md   ★ 完整构建与烧录流程（含六个关键坑）
```

## 板端源码说明

`board/` 只包含**相对 SDK 的改动文件**（保持原始相对路径），覆盖到
FH_RT_V3.4.0 SDK 的 `rt-thread/` 后按 BUILD.md 流程编译：

| 文件 | 改动内容 |
|---|---|
| `app/media_demo/sample_demo/qr_gate/` | 门禁主模块：扫码验签（HMAC-SHA256/时间窗/防重放）、防尾随状态机胶水、TCP 9000 服务（OPEN/OPENHOLD/CLOSE/TIME/STATUS）、舵机控制（含开杆宽限期） |
| `app/media_demo/sample_demo/nna_app/src/sample_nna_obj_detect.c` | 双路 NN：失败退避修复、每路独立 OSD 画布初始化（G1 画框） |
| `app/media_demo/.config` + `config.h` | 应用配置：双摄、稀疏协议参数、CH1 扫码通道 |
| `script/rtt/chip/MC632X/media.mk` | qr_gate 模块编入 |
| `platform/startup/board_startup.c` | 视频管线初始化（依赖顶层 FH_ENABLE_VIDEO） |
| `platform/mc632x/app_board/flash_layout.c` | NBG 模型分区（PART_NN_MODEL） |
| `top_config.readme.txt` | 顶层 `rt-thread/.config` 关键差异：`CONFIG_APP_CONFIG_MEDIA_DEMO=y` + `CONFIG_FH_ENABLE_VIDEO=y` |

## 快速使用（固件已验证版）

1. `firmware/media_demo.bin` 经 TFTP 加载到板卡 RAM `a0000000` 后 `go`（详见 BUILD.md）
2. 串口 msh：`ifconfig e0 192.168.137.100` → `vlcview -p 1234` → `qr_gate`
3. PC：`python tools/qrtool/qr_web.py 8000` → 手机打开 `http://<PC_IP>:8000/` 亮码扫码
4. 控制页：`http://<PC_IP>:8000/ctrl`（开门保持 / 关门）

## 硬件清单

- 富瀚 MC632X 开发板（XC00）
- CV2005 MIPI 摄像头 ×2（门外/门内）
- SG90/MG995 舵机（独立 5V 供电，与板共地；信号接 LCD_D12 = PWM_OUT2）
- PC（二维码生成 + 状态显示）+ 手机

## 许可

自研代码 MIT License；引用的富瀚 SDK/组件版权归原厂所有，遵循其许可。
