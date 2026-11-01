/*
 * qr_gate.c — MC632X 二维码门禁杆控制模块
 *
 * 功能:
 *   1. CH1(864x480 NV12/SCAN) 取帧 -> zbar QR 解码 -> HMAC-SHA256 验签 -> 开/关门杆
 *   2. TCP 9000: "OPEN\n" 远程开门, "TIME <epoch>\n" 校时, "PING\n" 探活, "STATUS\n" 防尾随状态
 *   3. msh 命令: servo_init / servo_open / servo_close / qr_test / qr_start / qr_stop
 *   4. 扫码/TCP 开门都经 gate_request(): 防尾随打开时一次只放一人 (tg_gate.c / tailgate.c)
 *
 * 舵机: PWM_OUT2 = GPIO2_6 = 用户板 LCD_D12 跳线座 (PAD3_G1_FUNC_SEL=2)
 *       备用    PWM_OUT3 = GPIO2_5 = 用户板 LCD_D13 跳线座 (PAD2_G1_FUNC_SEL=2)
 * 验签: HMAC-SHA256(key, "uid|ts|nonce"), 30s 时间窗 + nonce 防重放
 *       QR_SKIP_TIME_WINDOW 宏可关闭时间窗(板时钟未校准时调试用)
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <netinet/in.h>      /* RT-Thread SAL socket */
#include <unistd.h>          /* close() */
#include <zbar.h>
#include "qr_verify.h"       /* 载荷校验(格式->HMAC->时间窗->nonce), PC 单测见 _build/qr_door_test */
#include "gate_hw.h"         /* 授权开门统一走 gate_request (tg_gate.c 防尾随) */

/* 本版 zbar.h 未提供 fourcc 辅助宏, 此处补上 (Y800 = 'Y','8','0','0') */
#ifndef zbar_fourcc
#define zbar_fourcc(a, b, c, d) \
    ((unsigned long)((unsigned char)(a) | ((unsigned char)(b) << 8) | \
                     ((unsigned char)(c) << 16) | ((unsigned long)(unsigned char)(d) << 24)))
#endif

#include "pwm.h"             /* lib/inc/bsp/pwm.h: SET_PWM_CONFIG 等 */
#include "vpu_chn_config.h"  /* lock_vpu_stream / FH_VIDEO_FRAME */

#define QR_GATE_TCP_PORT     9000
#define QR_GATE_HOLD_MS      5000                      /* 开门保持时间 */
#define QR_SCAN_W            CHN1_MAX_WIDTH            /* 864 */
#define QR_SCAN_H            CHN1_MAX_HEIGHT           /* 480 */
/* 扫码线程优先级要低于 lwIP(tcpip 100 / eth 126), 否则 zbar 连续占 CPU 时网络收不到包 */
#define QR_SCAN_PRIO         150
#define QR_SCAN_GAP_MS       20                        /* 每帧之间让出 CPU */
/* 密钥/时间窗/nonce 参数见 qr_verify.h; QR_SKIP_TIME_WINDOW 仍可用于调试 */

/* ---------------- 引脚复用寄存器 (CEN_PIN_REG 0x1020_0000 域) ---------------- */
#define PAD2_G1_FUNC_SEL_REG  (*(volatile unsigned int *)0x10200038u) /* GPIO2_5: 2=PWM_OUT3 */
#define PAD3_G1_FUNC_SEL_REG  (*(volatile unsigned int *)0x1020003Cu) /* GPIO2_6: 2=PWM_OUT2 */

/* ---------------- 状态 ---------------- */
static rt_device_t g_pwm_dev = RT_NULL;
static volatile rt_uint8_t g_servo_ready = 0;
static volatile rt_uint8_t g_scan_run = 0;
static volatile unsigned long g_time_epoch = 0;   /* TCP TIME 命令设置的板时钟 */
static volatile rt_tick_t g_time_tick = 0;        /* 设置时的 tick 基准 */

static FH_UINT8 g_y[QR_SCAN_W * QR_SCAN_H];       /* 连续 Y 平面缓冲 */

static qr_nonce_ring_t g_ring;                    /* nonce 防重放表, 只在扫码线程里访问 */
static struct rt_timer g_close_timer;             /* 开门后自动关, 不阻塞调用线程 */
static volatile rt_uint8_t g_close_timer_ready = 0;

/* ---------------- 工具 ---------------- */
static unsigned long qr_now_sec(void)
{
    if (g_time_epoch == 0)
        return 0;
    /* tick 差 / 每秒 tick 数 = 秒 (原写法多乘了 1000, 校时后时钟快 1000 倍, 所有码都判过期) */
    return g_time_epoch + (unsigned long)((rt_tick_get() - g_time_tick) / RT_TICK_PER_SECOND);
}

/* ---------------- 舵机 ---------------- */
int servo_hw_init(void)
{
    rt_device_t dev;

    if (g_servo_ready)
        return 0;
    /* 不再调用 rt_hw_pwm_init(): board_startup.c 已调过; 再调会重新 probe,
     * 重新 malloc 驱动结构、重装 PWM 中断并重复注册 "pwm" 设备 */
    dev = rt_device_find("pwm");
    if (dev == RT_NULL)
    {
        rt_kprintf("[qr_gate] pwm device not found\n");
        return -1;
    }
    if (rt_device_open(dev, RT_DEVICE_OFLAG_RDWR) != RT_EOK)
    {
        rt_kprintf("[qr_gate] pwm device open failed\n");
        return -1;
    }
    g_pwm_dev = dev;
    /* 引脚复用: GPIO2_6->PWM_OUT2(主用), GPIO2_5->PWM_OUT3(备用) */
    PAD3_G1_FUNC_SEL_REG = (PAD3_G1_FUNC_SEL_REG & ~0x7u) | 0x2u;
    PAD2_G1_FUNC_SEL_REG = (PAD2_G1_FUNC_SEL_REG & ~0x7u) | 0x2u;
    rt_kprintf("[qr_gate] pinmux: PAD3_G1=2(PWM_OUT2/LCD_D12), PAD2_G1=2(PWM_OUT3/LCD_D13)\n");
    g_servo_ready = 1;
    return 0;
}

static int servo_set(unsigned int duty_ns)
{
    struct fh_pwm_chip_data chip;

    if (!g_servo_ready)
        return -1;
    rt_memset(&chip, 0, sizeof(chip));
    chip.id = 2;                              /* PWM_OUT2 -> GPIO2_6 */
    chip.config.period_ns = 20000000u;        /* 50Hz */
    chip.config.duty_ns = duty_ns;            /* 0.5ms~2.0ms */
    chip.config.pulses = FH_PWM_PULSE_NOLIMIT;
    chip.config.stop_module = FH_PWM_STOPLVL_KEEP;
    if (rt_device_control(g_pwm_dev, SET_PWM_CONFIG, &chip) != RT_EOK)
    {
        rt_kprintf("[qr_gate] SET_PWM_CONFIG failed\n");
        return -1;
    }
    if (rt_device_control(g_pwm_dev, SET_PWM_ENABLE, &chip) != RT_EOK)
    {
        rt_kprintf("[qr_gate] SET_PWM_ENABLE failed\n");
        return -1;
    }
    rt_kprintf("[qr_gate] pwm ch%u period=%u duty=%u\n",
               (unsigned)chip.id, chip.config.period_ns, chip.config.duty_ns);
    return 0;
}

/* MG995 安全脉宽 1.0~2.0ms(约90°); 0.5ms 属 180° 扩展范围, 多数国产 MG995 不响应 */
void gate_close(void)
{
    if (servo_set(1000000u) == 0)
        rt_kprintf("[qr_gate] arm CLOSED (1.0ms)\n");
}

void gate_open(void)
{
    if (servo_set(2000000u) == 0)
        rt_kprintf("[qr_gate] arm OPEN (2.0ms)\n");
}

static void gate_close_timeout(void *param)
{
    (void)param;
    gate_close();
}

/* 开门并在 QR_GATE_HOLD_MS 后自动关; 不阻塞调用线程.
 * 扫码和 TCP 两路都走这里, 保持期间再次开门会重新计时, 不会被另一路提前关杆 */
void gate_pulse(const char *who)
{
    /* 扫码线程和 TCP 线程都可能首次进来; 两者都是线程上下文, 锁调度器即可 */
    rt_enter_critical();
    if (!g_close_timer_ready)
    {
        rt_timer_init(&g_close_timer, "qr_close", gate_close_timeout, RT_NULL,
                      rt_tick_from_millisecond(QR_GATE_HOLD_MS),
                      RT_TIMER_FLAG_ONE_SHOT | RT_TIMER_FLAG_SOFT_TIMER);
        g_close_timer_ready = 1;
    }
    rt_exit_critical();

    rt_kprintf("[qr_gate] open by %s, auto close in %d ms\n", who, QR_GATE_HOLD_MS);
    gate_open();
    rt_timer_stop(&g_close_timer);
    rt_timer_start(&g_close_timer);
}

/* 远程保持开门: 开杆并取消自动回杆定时器, 由 CLOSE 命令(或防尾随状态机)关杆 */
void gate_hold_open(void)
{
    rt_enter_critical();
    if (g_close_timer_ready)
        rt_timer_stop(&g_close_timer);
    rt_exit_critical();
    rt_kprintf("[qr_gate] open by hold (no auto close)\n");
    gate_open();
}

/* 远程关门: 取消定时器 + 立即关杆 */
void gate_close_now(void)
{
    rt_enter_critical();
    if (g_close_timer_ready)
        rt_timer_stop(&g_close_timer);
    rt_exit_critical();
    rt_kprintf("[qr_gate] close by command\n");
    gate_close();
}

/* ---------------- 验签 + 开门 ---------------- */
static void qr_handle_payload(const char *data, unsigned int len)
{
    static qr_result_t last_r = QR_OK;
    static int last_busy = GATE_REQ_OPENED;
    qr_result_t r;
    unsigned long now;
    int busy;

    /* 上一个人还没放行完/报警中: 先不验签, 码的 nonce 不被消耗, 等放行完继续对着摄像头就能开 */
    busy = gate_can_open();
    if (busy != GATE_REQ_OPENED)
    {
        if (busy != last_busy)
            rt_kprintf("[qr_gate] code seen, wait: %s\n",
                       busy == GATE_REQ_ALARM ? "tailgate alarm" : "previous person passing");
        last_busy = busy;
        return;
    }
    last_busy = busy;

#if QR_EN_TIME_WINDOW
    if (g_time_epoch == 0)
    {
        rt_kprintf("[qr_gate] board time not synced, use PC tool '校时' first\n");
        return;
    }
#endif
    now = qr_now_sec();
    /* 扫码线程和 shell(qr_inject) 都会进来, g_ring 要互斥; 一次 HMAC 很短, 锁调度器即可 */
    rt_enter_critical();
    r = qr_verify(data, len, (uint32_t)now, &g_ring);
    rt_exit_critical();
    if (r == QR_OK)
    {
        rt_kprintf("[qr_gate] access GRANTED (now=%lu)\n", now);
        if (gate_request("qr") == GATE_REQ_NOHW)
            rt_kprintf("[qr_gate] pwm not available, arm not moved\n");
    }
    else if (r != QR_ERR_REPLAY || last_r != QR_ERR_REPLAY)
    {
        /* 同一张码每帧都会被解出来, 连续的 replay 只打一次 */
        rt_kprintf("[qr_gate] reject: %s (now=%lu)\n", qr_result_str(r), now);
    }
    last_r = r;
}

/* ---------------- 扫码线程 ---------------- */
static void qr_scan_entry(void *param)
{
    zbar_image_scanner_t *scn;
    zbar_image_t *img;

    (void)param;
    scn = zbar_image_scanner_create();
    img = zbar_image_create();
    if (scn == RT_NULL || img == RT_NULL)
    {
        rt_kprintf("[qr_gate] zbar create failed\n");
        if (scn) zbar_image_scanner_destroy(scn);
        if (img) zbar_image_destroy(img);
        return;
    }
    zbar_image_scanner_set_config(scn, ZBAR_QRCODE, ZBAR_CFG_ENABLE, 1);
    zbar_image_set_format(img, zbar_fourcc('Y', '8', '0', '0'));

    while (g_scan_run)
    {
        FH_VIDEO_FRAME frm;
        FH_UINT32 lock = 0;
        FH_UINT32 w, h, stride, row;
        FH_UINT8 *src;

        if (lock_vpu_stream(0, 1, &frm, 500, &lock, 1) != 0)
        {
            rt_thread_mdelay(200);   /* 视频管线没起来时别空转刷屏 */
            continue;
        }
        w = frm.size.u32Width;
        h = frm.size.u32Height;
        stride = frm.frm_scan.luma.stride;
        src = (FH_UINT8 *)frm.frm_scan.luma.data.vbase;
        if (w != QR_SCAN_W || h != QR_SCAN_H || w * h > sizeof(g_y))
        {
            unlock_vpu_stream(0, 1, lock, &frm);
            continue;
        }
        /* 注: SDK 自身取帧路径不做 D-Cache 失效处理(缓冲在一致域);
         * 若实测画面花屏/内容不更新, 再在此处补 cache invalidate(见 Claude 调试建议) */
        for (row = 0; row < h; row++)
            memcpy(g_y + row * w, src + row * stride, w);
        unlock_vpu_stream(0, 1, lock, &frm);

        zbar_image_set_size(img, w, h);
        zbar_image_set_data(img, g_y, w * h, RT_NULL);
        if (zbar_scan_image(scn, img) > 0)
        {
            const zbar_symbol_t *sym = zbar_image_first_symbol(img);
            for (; sym != RT_NULL; sym = zbar_symbol_next(sym))
                qr_handle_payload(zbar_symbol_get_data(sym), zbar_symbol_get_data_length(sym));
        }
        rt_thread_mdelay(QR_SCAN_GAP_MS);
    }
    zbar_image_destroy(img);
    zbar_image_scanner_destroy(scn);
    rt_kprintf("[qr_gate] scan thread exit\n");
}

/* ---------------- TCP 9000: OPEN / TIME / PING ---------------- */
static void qr_tcp_entry(void *param)
{
    int srv, cli;
    struct sockaddr_in addr;
    socklen_t len;

    (void)param;
    srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0)
    {
        rt_kprintf("[qr_gate] tcp socket failed\n");
        return;
    }
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(QR_GATE_TCP_PORT);
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(srv, 2) < 0)
    {
        rt_kprintf("[qr_gate] tcp bind/listen failed\n");
        close(srv);
        return;
    }
    rt_kprintf("[qr_gate] tcp server on port %d\n", QR_GATE_TCP_PORT);
    while (1)
    {
        char buf[256];
        int n;
        len = sizeof(struct sockaddr_in);
        cli = accept(srv, (struct sockaddr *)&addr, &len);
        if (cli < 0)
            continue;
        n = recv(cli, buf, sizeof(buf) - 1, 0);
        if (n > 0)
        {
            char *endp;
            unsigned long v;
            buf[n] = '\0';
            while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
                buf[--n] = '\0';
            if (strncmp(buf, "OPENHOLD", 8) == 0)
            {
                /* 远程保持开门: 开杆且不启动自动回杆定时器, 等 CLOSE 命令 */
                int r = gate_request("tcp-hold");
                const char *rep = (r == GATE_REQ_OPENED) ? "OPENED-HOLD\n" :
                                  (r == GATE_REQ_BUSY) ? "BUSY\n" :
                                  (r == GATE_REQ_ALARM) ? "ALARM\n" : "ERR pwm\n";
                if (r == GATE_REQ_OPENED)
                    gate_hold_open();
                send(cli, rep, strlen(rep), 0);
            }
            else if (strncmp(buf, "CLOSE", 5) == 0)
            {
                gate_close_now();
                send(cli, "CLOSED\n", 7, 0);
            }
            else if (strncmp(buf, "OPEN", 4) == 0)
            {
                int r = gate_request("tcp");
                const char *rep = (r == GATE_REQ_OPENED) ? "OPENED\n" :
                                  (r == GATE_REQ_BUSY) ? "BUSY\n" :
                                  (r == GATE_REQ_ALARM) ? "ALARM\n" : "ERR pwm\n";
                send(cli, rep, strlen(rep), 0);
            }
            else if (strncmp(buf, "STATUS", 6) == 0)
            {
                char line[128];
                int m = tg_status_line(line, sizeof(line));
                if (m > (int)sizeof(line) - 1)
                    m = sizeof(line) - 1;
                send(cli, line, m, 0);
            }
            else if (strncmp(buf, "TIME ", 5) == 0)
            {
                v = strtoul(buf + 5, &endp, 10);
                if (*endp == '\0' && v != 0)
                {
                    g_time_epoch = v;
                    g_time_tick = rt_tick_get();
                    send(cli, "TIME-SET\n", 9, 0);
                    rt_kprintf("[qr_gate] time synced: %u\n", (unsigned)v);
                }
                else
                    send(cli, "BAD-TIME\n", 9, 0);
            }
            else if (strncmp(buf, "PING", 4) == 0)
                send(cli, "PONG\n", 5, 0);
            else
                send(cli, "UNKNOWN\n", 8, 0);
        }
        close(cli);
    }
}

/* ---------------- msh 命令 ---------------- */
static int servo_init(void)
{
    return servo_hw_init();
}
MSH_CMD_EXPORT(servo_init, init pwm device and pinmux for gate servo);

static int servo_open(void)
{
    if (servo_hw_init() != 0) return -1;
    gate_open();
    return 0;
}
MSH_CMD_EXPORT(servo_open, gate arm open (servo 2.0ms));

static int servo_close(void)
{
    if (servo_hw_init() != 0) return -1;
    gate_close();
    return 0;
}
MSH_CMD_EXPORT(servo_close, gate arm close (servo 1.0ms));

static int servo_mid(void)
{
    if (servo_hw_init() != 0) return -1;
    if (servo_set(1500000u) == 0)
        rt_kprintf("[qr_gate] arm MID (1.5ms)\n");
    return 0;
}
MSH_CMD_EXPORT(servo_mid, gate arm to middle (servo 1.5ms));

/* 联调用: 直接指定脉宽微秒数, 扫舵机实际响应范围, 如 servo_us 1800 */
static int servo_us(int argc, char **argv)
{
    long us;
    if (argc != 2)
    {
        rt_kprintf("usage: servo_us <500..2500 (us)>\n");
        return -1;
    }
    us = strtol(argv[1], RT_NULL, 10);
    if (us < 500 || us > 2500)
    {
        rt_kprintf("out of range 500..2500 us\n");
        return -1;
    }
    if (servo_hw_init() != 0) return -1;
    if (servo_set((unsigned int)us * 1000u) == 0)
        rt_kprintf("[qr_gate] arm pulse %ld us\n", us);
    return 0;
}
MSH_CMD_EXPORT(servo_us, set servo pulse width in microseconds (debug));

static int qr_test(void)
{
    zbar_image_scanner_t *scn;
    zbar_image_t *img;
    FH_VIDEO_FRAME frm;
    FH_UINT32 lock = 0, w, h, stride, row;
    FH_UINT8 *src;
    int n;

    scn = zbar_image_scanner_create();
    img = zbar_image_create();
    zbar_image_scanner_set_config(scn, ZBAR_QRCODE, ZBAR_CFG_ENABLE, 1);
    zbar_image_set_format(img, zbar_fourcc('Y', '8', '0', '0'));

    if (lock_vpu_stream(0, 1, &frm, 1000, &lock, 1) != 0)
    {
        rt_kprintf("[qr_test] lock CH1 failed (先运行 vlcview 建立视频通道)\n");
        zbar_image_destroy(img);
        zbar_image_scanner_destroy(scn);
        return -1;
    }
    w = frm.size.u32Width;
    h = frm.size.u32Height;
    stride = frm.frm_scan.luma.stride;
    src = (FH_UINT8 *)frm.frm_scan.luma.data.vbase;
    rt_kprintf("[qr_test] frame %ux%u stride=%u\n", (unsigned)w, (unsigned)h, (unsigned)stride);
    if (w <= QR_SCAN_W && h <= QR_SCAN_H)
    {
        for (row = 0; row < h; row++)
            memcpy(g_y + row * w, src + row * stride, w);
        zbar_image_set_size(img, w, h);
        zbar_image_set_data(img, g_y, w * h, RT_NULL);
        n = zbar_scan_image(scn, img);
        if (n > 0)
        {
            const zbar_symbol_t *sym = zbar_image_first_symbol(img);
            for (; sym != RT_NULL; sym = zbar_symbol_next(sym))
                rt_kprintf("[qr_test] QR: %s\n", zbar_symbol_get_data(sym));
        }
        else
            rt_kprintf("[qr_test] no qr decoded (无码/画面无二维码为正常现象)\n");
    }
    else
        rt_kprintf("[qr_test] frame too large for buffer\n");
    unlock_vpu_stream(0, 1, lock, &frm);
    zbar_image_destroy(img);
    zbar_image_scanner_destroy(scn);
    return 0;
}
MSH_CMD_EXPORT(qr_test, grab one CH1 frame and try zbar decode);

/* 联调用: 把载荷字符串直接送进验签, 不经摄像头 */
static int qr_inject(int argc, char **argv)
{
    if (argc != 2)
    {
        rt_kprintf("usage: qr_inject dooruser1|<ts>|<nonce>|<hmac>\n");
        return -1;
    }
    qr_handle_payload(argv[1], rt_strlen(argv[1]));
    return 0;
}
MSH_CMD_EXPORT(qr_inject, feed a qr payload string to verifier (debug));

static int qr_start(void)
{
    rt_thread_t tid;
    if (g_scan_run)
    {
        rt_kprintf("[qr_gate] already running\n");
        return 0;
    }
    /* g_ring 是静态变量已清零; 这里不再重置, 否则 qr_stop/qr_start 后 30s 内可重放 */
    g_scan_run = 1;
    tid = rt_thread_create("qr_scan", qr_scan_entry, RT_NULL,
                           256 * 1024, QR_SCAN_PRIO, 10);
    if (tid == RT_NULL)
    {
        g_scan_run = 0;
        rt_kprintf("[qr_gate] create scan thread failed\n");
        return -1;
    }
    rt_thread_startup(tid);
    rt_kprintf("[qr_gate] scan thread started\n");
    return 0;
}
MSH_CMD_EXPORT(qr_start, start background qr scan thread);

static int qr_stop(void)
{
    g_scan_run = 0;
    return 0;
}
MSH_CMD_EXPORT(qr_stop, stop background qr scan thread);

static int qr_tcp(void)
{
    rt_thread_t tid = rt_thread_create("qr_tcp", qr_tcp_entry, RT_NULL,
                                       16 * 1024, 20, 10);
    if (tid == RT_NULL)
        return -1;
    rt_thread_startup(tid);
    return 0;
}
MSH_CMD_EXPORT(qr_tcp, start tcp server port 9000 (OPEN/TIME/PING));

static int qr_gate(void)
{
    int r = 0;
    if (servo_hw_init() != 0) r = -1;
    if (qr_start() != 0) r = -1;
    if (tg_start() != 0) r = -1;
    if (qr_tcp() != 0) r = -1;
    rt_kprintf("[qr_gate] %s (scan + tcp + servo + anti-tailgate)\n", (r == 0) ? "started" : "start failed");
    return r;
}
MSH_CMD_EXPORT(qr_gate, one-shot start: servo + qr scan + tcp server);
