/*
 * tg_gate.c — 防尾随的 RT-Thread 胶水: 人形检测人数 -> tailgate.c 状态机 -> 开/关杆
 *
 * A 侧(门外)、B 侧(门内)各指定一个 video group + 一个矩形区域(百分比), 框中心在区域内且置信度够才计数.
 *   双摄(默认): A = grp0 全幅, B = grp1 全幅 (grp1 要在 appconfig 打开 VIDEO_GRP1 + NN_ENABLE_G1)
 *   单摄:      tg_roi a 0 0 0 50 100 ; tg_roi b 0 50 0 100 100   (画面左半门外, 右半门内)
 * 并发: NN 线程(喂人数) / 扫码、TCP、shell(授权) / tg_tick(超时) 都在 g_lock 下操作状态机和门杆.
 */
#include <rtthread.h>
#include <stdlib.h>
#include <string.h>
#include "tailgate.h"
#include "gate_hw.h"

#define TG_TICK_MS     50
#define TG_TICK_PRIO   140             /* 低于 lwIP(100~126) 之后, 高于扫码线程(150) */
#define TG_SIM_EVERY   4               /* 模拟人数每 4 个 tick 喂一次 = 200ms, 与 5fps 检测一致 */
#define TG_MAX_GRP     1               /* MC632X 两路: grp 0/1 */

typedef struct
{
    int grp;
    int x0, y0, x1, y1;                /* 百分比 0~100 */
} tg_src_t;

static tg_t            g_tg;
static struct rt_mutex g_lock;
static volatile int    g_ready = 0;
static volatile int    g_enable = 1;
static int             g_conf_pct = 60;
static tg_src_t        g_src[2] = { { 0, 0, 0, 100, 100 }, { 1, 0, 0, 100, 100 } };
static volatile int    g_sim = 0;      /* 1: 人数来自 tg_sim 命令, 不用检测结果 */
static int             g_sim_cnt[2] = { -1, -1 };
static int             g_verbose = 0;
static unsigned        g_pass_cnt = 0;
static unsigned        g_alarm_cnt = 0;
static tg_event_t      g_last_ev = TG_EV_NONE;

static uint32_t tg_now_ms(void)
{
    return (uint32_t)((rt_uint64_t)rt_tick_get() * 1000u / RT_TICK_PER_SECOND);
}

static int side_cnt(int s)
{
    return g_tg.side[s].valid ? g_tg.side[s].stable : -1;
}

/* 执行状态机输出; 调用者持有 g_lock */
static void tg_apply(tg_out_t o)
{
    if (o.act == TG_ACT_OPEN)
        gate_open();
    else if (o.act == TG_ACT_CLOSE)
        gate_close();
    if (o.ev == TG_EV_NONE)
        return;
    g_last_ev = o.ev;
    if (o.ev == TG_EV_PASS_OK || o.ev == TG_EV_B_FIRST)
        g_pass_cnt++;
    if (o.ev == TG_EV_TAILGATE)
    {
        g_alarm_cnt++;
        rt_kprintf("[tg] !!! TAILGATE ALARM: count jump %d (a=%d b=%d), arm closed, refuse auth %u ms\n",
                   o.delta, side_cnt(TG_SIDE_A), side_cnt(TG_SIDE_B), (unsigned)g_tg.cfg.alarm_ms);
        return;
    }
    /* 授权结果由 gate_request 打印 */
    if (!g_verbose && (o.ev == TG_EV_AUTH_OPEN || o.ev == TG_EV_AUTH_BUSY || o.ev == TG_EV_AUTH_ALARM))
        return;
    rt_kprintf("[tg] %s -> %s (a=%d b=%d)\n", tg_event_str(o.ev), tg_state_str(g_tg.state),
               side_cnt(TG_SIDE_A), side_cnt(TG_SIDE_B));
}

static void tg_tick_entry(void *param)
{
    unsigned n = 0;
    int s;

    (void)param;
    while (1)
    {
        rt_mutex_take(&g_lock, RT_WAITING_FOREVER);
        if (g_sim && (n++ % TG_SIM_EVERY) == 0)
        {
            for (s = 0; s < 2; s++)
                if (g_sim_cnt[s] >= 0)
                    tg_apply(tg_feed(&g_tg, s, g_sim_cnt[s], tg_now_ms()));
        }
        tg_apply(tg_tick(&g_tg, tg_now_ms()));
        rt_mutex_release(&g_lock);
        rt_thread_mdelay(TG_TICK_MS);
    }
}

/* 只从 shell 线程调用(qr_gate / tg 命令), 不需要防重入 */
int tg_start(void)
{
    rt_thread_t tid;

    if (g_ready)
        return 0;
    tg_init(&g_tg, RT_NULL);
    /* 扫码后等人通过: 人还在 G0 画面里(A>=峰值)杆保持不关,
     * 人一离开 G0 立即关杆(A_LEFT); 15s 无人通过才兜底关杆 */
    g_tg.cfg.open_ms = 15000;
    /* 开杆宽限 3s: 举手机扫码挡住人导致 A 侧人数闪动, 宽限期内不关杆不告警 */
    g_tg.cfg.min_open_ms = 3000;
    /* 去抖 5 帧(1s@5fps): 单帧检测丢失不再触发 a-left */
    g_tg.cfg.debounce = 5;
    rt_mutex_init(&g_lock, "tg", RT_IPC_FLAG_PRIO);
    tid = rt_thread_create("tg_tick", tg_tick_entry, RT_NULL, 8192, TG_TICK_PRIO, 10);
    if (tid == RT_NULL)
    {
        rt_mutex_detach(&g_lock);
        rt_kprintf("[tg] create tick thread failed\n");
        return -1;
    }
    g_ready = 1;
    rt_thread_startup(tid);
    rt_kprintf("[tg] anti-tailgate %s, A=grp%d B=grp%d conf>=%d%%\n",
               g_enable ? "on" : "off", g_src[0].grp, g_src[1].grp, g_conf_pct);
    return 0;
}

/* 人形检测线程每帧调用; 覆盖 sample_nna_obj_detect.c 里的弱符号 */
void qr_gate_nn_report(int grp_id, int n, const float *cx, const float *cy, const float *conf)
{
    int cnt[2] = { 0, 0 };
    int s, i, used = 0;

    if (!g_ready || g_sim)
        return;
    for (s = 0; s < 2; s++)
    {
        const tg_src_t *src = &g_src[s];
        if (src->grp != grp_id)
            continue;
        used |= 1 << s;
        for (i = 0; i < n; i++)
        {
            int x = (int)(cx[i] * 100.0f), y = (int)(cy[i] * 100.0f);
            if ((int)(conf[i] * 100.0f) < g_conf_pct)
                continue;
            if (x >= src->x0 && x <= src->x1 && y >= src->y0 && y <= src->y1)
                cnt[s]++;
        }
    }
    if (!used)
        return;
    rt_mutex_take(&g_lock, RT_WAITING_FOREVER);
    for (s = 0; s < 2; s++)
        if (used & (1 << s))
            tg_apply(tg_feed(&g_tg, s, cnt[s], tg_now_ms()));
    rt_mutex_release(&g_lock);
    if (g_verbose > 1)
        rt_kprintf("[tg] nn grp%d raw=%d a=%d b=%d\n", grp_id, n,
                   (used & 1) ? cnt[0] : -1, (used & 2) ? cnt[1] : -1);
}

int gate_request(const char *who)
{
    tg_out_t o;
    int r;

    if (servo_hw_init() != 0)
        return GATE_REQ_NOHW;
    if (!g_ready || !g_enable)
    {
        gate_pulse(who);               /* 防尾随没开: 原来的开 5s 自动关 */
        return GATE_REQ_OPENED;
    }
    rt_mutex_take(&g_lock, RT_WAITING_FOREVER);
    o = tg_auth(&g_tg, tg_now_ms());
    tg_apply(o);
    rt_mutex_release(&g_lock);
    r = (o.ev == TG_EV_AUTH_OPEN) ? GATE_REQ_OPENED :
        (o.ev == TG_EV_AUTH_ALARM) ? GATE_REQ_ALARM : GATE_REQ_BUSY;
    rt_kprintf("[tg] auth by %s: %s (a=%d b=%d)\n", who,
               r == GATE_REQ_OPENED ? "OPEN, close when one person passes" :
               r == GATE_REQ_ALARM ? "refused, tailgate alarm active" : "refused, previous pass not finished",
               side_cnt(TG_SIDE_A), side_cnt(TG_SIDE_B));
    return r;
}

/* 不加锁的预判(读一个枚举), 真正的判定在 gate_request 里 */
int gate_can_open(void)
{
    if (!g_ready || !g_enable)
        return GATE_REQ_OPENED;
    if (g_tg.state == TG_ALARM)
        return GATE_REQ_ALARM;
    return (g_tg.state == TG_IDLE) ? GATE_REQ_OPENED : GATE_REQ_BUSY;
}

int tg_status_line(char *buf, int size)
{
    if (!g_ready)
        return rt_snprintf(buf, size, "TG not-started\n");
    return rt_snprintf(buf, size, "TG %s state=%s a=%d b=%d pass=%u alarm=%u last=%s%s\n",
                       g_enable ? "on" : "off", tg_state_str(g_tg.state),
                       side_cnt(TG_SIDE_A), side_cnt(TG_SIDE_B), g_pass_cnt, g_alarm_cnt,
                       tg_event_str(g_last_ev), g_sim ? " (sim)" : "");
}

/* ---------------- msh 命令 ---------------- */
static int tg(int argc, char **argv)
{
    char line[128];

    if (tg_start() != 0)
        return -1;
    if (argc == 2 && (strcmp(argv[1], "on") == 0 || strcmp(argv[1], "off") == 0))
    {
        tg_cfg_t cfg;
        rt_mutex_take(&g_lock, RT_WAITING_FOREVER);
        g_enable = (argv[1][1] == 'n');
        cfg = g_tg.cfg;
        tg_init(&g_tg, &cfg);          /* 切换时回到 IDLE, 杆关上 */
        gate_close();
        rt_mutex_release(&g_lock);
    }
    else if (argc != 1)
    {
        rt_kprintf("usage: tg [on|off]\n");
        return -1;
    }
    tg_status_line(line, sizeof(line));
    rt_kprintf("%s", line);
    return 0;
}
MSH_CMD_EXPORT(tg, anti-tailgate status / tg on|off);

static int tg_cfg(int argc, char **argv)
{
    tg_cfg_t *c = &g_tg.cfg;
    int v;

    if (tg_start() != 0)
        return -1;
    if (argc == 3)
    {
        v = atoi(argv[2]);
        rt_mutex_take(&g_lock, RT_WAITING_FOREVER);
        if (strcmp(argv[1], "debounce") == 0 && v >= 1 && v <= 10) c->debounce = v;
        else if (strcmp(argv[1], "open") == 0 && v >= 500)          c->open_ms = v;
        else if (strcmp(argv[1], "pass") == 0 && v >= 500)          c->pass_ms = v;
        else if (strcmp(argv[1], "watch") == 0 && v >= 0)           c->watch_ms = v;
        else if (strcmp(argv[1], "alarm") == 0 && v >= 0)           c->alarm_ms = v;
        else if (strcmp(argv[1], "stale") == 0 && v >= 300)         c->stale_ms = v;
        else if (strcmp(argv[1], "conf") == 0 && v >= 1 && v <= 99) g_conf_pct = v;
        else if (strcmp(argv[1], "verbose") == 0 && v >= 0)         g_verbose = v;
        else argc = 0;
        rt_mutex_release(&g_lock);
    }
    if (argc != 1 && argc != 3)
    {
        rt_kprintf("usage: tg_cfg [debounce|open|pass|watch|alarm|stale|conf|verbose <n>]\n");
        return -1;
    }
    rt_kprintf("debounce=%u open=%u pass=%u watch=%u alarm=%u stale=%u (ms) conf=%d%% verbose=%d\n",
               (unsigned)c->debounce, (unsigned)c->open_ms, (unsigned)c->pass_ms, (unsigned)c->watch_ms,
               (unsigned)c->alarm_ms, (unsigned)c->stale_ms, g_conf_pct, g_verbose);
    rt_kprintf("A: grp%d roi %d,%d-%d,%d   B: grp%d roi %d,%d-%d,%d (%%)\n",
               g_src[0].grp, g_src[0].x0, g_src[0].y0, g_src[0].x1, g_src[0].y1,
               g_src[1].grp, g_src[1].x0, g_src[1].y0, g_src[1].x1, g_src[1].y1);
    return 0;
}
MSH_CMD_EXPORT(tg_cfg, anti-tailgate config: tg_cfg [key value]);

static int tg_roi(int argc, char **argv)
{
    int s, g, x0, y0, x1, y1;

    if (argc != 7 || (argv[1][0] != 'a' && argv[1][0] != 'b'))
    {
        rt_kprintf("usage: tg_roi a|b <grp> <x0> <y0> <x1> <y1>   (percent 0..100)\n");
        return -1;
    }
    s = (argv[1][0] == 'a') ? TG_SIDE_A : TG_SIDE_B;
    g = atoi(argv[2]);
    x0 = atoi(argv[3]); y0 = atoi(argv[4]); x1 = atoi(argv[5]); y1 = atoi(argv[6]);
    if (g < 0 || g > TG_MAX_GRP || x0 < 0 || y0 < 0 || x1 > 100 || y1 > 100 || x0 >= x1 || y0 >= y1)
    {
        rt_kprintf("[tg] bad roi\n");
        return -1;
    }
    if (tg_start() != 0)
        return -1;
    rt_mutex_take(&g_lock, RT_WAITING_FOREVER);
    g_src[s].grp = g;
    g_src[s].x0 = x0; g_src[s].y0 = y0; g_src[s].x1 = x1; g_src[s].y1 = y1;
    g_tg.side[s].valid = 0;            /* 区域变了, 该侧重新建立基准 */
    g_tg.side[s].hits = 0;
    rt_mutex_release(&g_lock);
    return tg_cfg(1, argv);
}
MSH_CMD_EXPORT(tg_roi, set counting region: tg_roi a|b grp x0 y0 x1 y1);

/* 联调用: 不用摄像头, 手动给两侧人数 (-1 = 该侧无数据) */
static int tg_sim(int argc, char **argv)
{
    if (tg_start() != 0)
        return -1;
    if (argc == 2 && strcmp(argv[1], "off") == 0)
    {
        g_sim = 0;
        rt_kprintf("[tg] sim off, counts from person detection\n");
        return 0;
    }
    if (argc != 3)
    {
        rt_kprintf("usage: tg_sim <a> <b> | tg_sim off\n");
        return -1;
    }
    rt_mutex_take(&g_lock, RT_WAITING_FOREVER);
    g_sim_cnt[TG_SIDE_A] = atoi(argv[1]);
    g_sim_cnt[TG_SIDE_B] = atoi(argv[2]);
    g_sim = 1;
    rt_mutex_release(&g_lock);
    return 0;
}
MSH_CMD_EXPORT(tg_sim, simulate people count: tg_sim a b | tg_sim off);

static int tg_open(void)
{
    int r;
    if (tg_start() != 0)
        return -1;
    r = gate_request("shell");
    return (r == GATE_REQ_OPENED) ? 0 : -1;
}
MSH_CMD_EXPORT(tg_open, authorized open request (same path as qr / tcp OPEN));
