/*
 * tailgate.h — 双摄像头防尾随状态机 (纯 C, 不依赖 RT-Thread, PC 单测见 _build/qr_door_test)
 *
 * A 侧 = 门外排队区, B 侧 = 门内放行区. 每侧只关心"区域内人数".
 *
 *   IDLE  --授权(扫码/TCP OPEN)-->  OPEN   (开杆, 记下 A 侧峰值人数 / B 侧基准人数)
 *   OPEN  --A 侧人数比峰值少 1-->   PASSING(立即关杆, 等人出现在 B 侧)
 *   OPEN  --A 侧一次少 >=2-------->  ALARM  (关杆, 两人一起过 = 尾随)
 *   OPEN  --B 侧先多出 1 人------->  PASSING 后直接 PASS_OK (关杆)
 *   PASSING --B 侧多 1 人--------->  IDLE   (放行完成, 才接受下一个授权)
 *   PASSING --B 侧多 >=2 人------->  ALARM
 *   IDLE(放行后 watch 窗口内) --B 侧又多人--> ALARM (第二人贴身钻过)
 *   OPEN 超时 / PASSING 超时 / ALARM 保持到时 均回到安全状态(杆是关的)
 *
 * 某侧超过 stale_ms 没有数据 = 该侧没有摄像头/检测没跑, 只用另一侧 + 超时, 退化成原来的"开 N 秒自动关".
 */
#ifndef __TAILGATE_H__
#define __TAILGATE_H__

#include <stdint.h>

#define TG_SIDE_A 0   /* 门外 */
#define TG_SIDE_B 1   /* 门内 */

typedef enum
{
    TG_IDLE = 0,
    TG_OPEN,
    TG_PASSING,
    TG_ALARM,
} tg_state_t;

typedef enum
{
    TG_EV_NONE = 0,
    TG_EV_AUTH_OPEN,      /* 授权通过, 开杆 */
    TG_EV_AUTH_BUSY,      /* 上一个人还没放行完, 拒绝 */
    TG_EV_AUTH_ALARM,     /* 报警保持中, 拒绝 */
    TG_EV_A_LEFT,         /* A 侧少了 1 人, 关杆 */
    TG_EV_B_FIRST,        /* A 侧没看到少人但 B 侧先多 1 人, 关杆并算放行 */
    TG_EV_PASS_OK,        /* B 侧出现 1 人, 放行完成 */
    TG_EV_TAILGATE,       /* 尾随报警 */
    TG_EV_OPEN_TIMEOUT,   /* 开杆后没人过, 超时关杆 */
    TG_EV_PASS_TIMEOUT,   /* 关杆后 B 侧一直没见到人 */
    TG_EV_ALARM_CLEAR,    /* 报警保持结束 */
} tg_event_t;

typedef enum
{
    TG_ACT_NONE = 0,
    TG_ACT_OPEN,
    TG_ACT_CLOSE,
} tg_action_t;

typedef struct
{
    uint32_t debounce;        /* 人数连续几帧一致才算数 (5fps 下 2 帧 ≈ 0.4s) */
    uint32_t open_ms;         /* 开杆后最长等待 */
    uint32_t pass_ms;         /* 关杆后等 B 侧出现人的最长时间 */
    uint32_t watch_ms;        /* 放行完成后继续盯 B 侧的时间 */
    uint32_t alarm_ms;        /* 报警保持时间, 期间拒绝授权 */
    uint32_t stale_ms;        /* 某侧多久没数据算失效 */
    uint32_t min_open_ms;     /* 开杆宽限期: 期内 A 侧人数波动不触发关杆/告警 (0=关闭) */
} tg_cfg_t;

typedef struct
{
    int      valid;           /* 已有稳定人数且未失效 */
    int      stable;          /* 去抖后的人数 */
    int      cand;            /* 候选人数 */
    uint32_t hits;            /* 候选人数连续出现次数 */
    uint32_t last_ms;         /* 最近一次喂数据的时间 */
} tg_side_t;

typedef struct
{
    tg_cfg_t   cfg;
    tg_state_t state;
    tg_side_t  side[2];
    int        a_peak;        /* OPEN 期间 A 侧人数峰值 */
    int        b_ref;         /* B 侧基准(授权以来的最小值) */
    int        watch;         /* 放行后 watch 窗口是否有效 */
    uint32_t   t_state;       /* 进入当前状态的时间 */
    uint32_t   t_watch;       /* watch 窗口开始时间 */
    int        last_delta;    /* 最近一次判定用到的人数变化, 给日志用 */
} tg_t;

typedef struct
{
    tg_event_t  ev;
    tg_action_t act;
    int         delta;        /* 触发事件的人数变化 */
} tg_out_t;

void        tg_default_cfg(tg_cfg_t *cfg);
void        tg_init(tg_t *tg, const tg_cfg_t *cfg);
/* 某侧一帧检测结果 (区域内人数) */
tg_out_t    tg_feed(tg_t *tg, int side, int count, uint32_t now_ms);
/* 授权开门请求 */
tg_out_t    tg_auth(tg_t *tg, uint32_t now_ms);
/* 周期调用, 处理超时 */
tg_out_t    tg_tick(tg_t *tg, uint32_t now_ms);

const char *tg_state_str(tg_state_t s);
const char *tg_event_str(tg_event_t e);

#endif
