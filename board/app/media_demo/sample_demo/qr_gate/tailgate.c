/*
 * tailgate.c — 双摄像头防尾随状态机, 规则见 tailgate.h
 * 时间都是 32 位毫秒, 用无符号差值比较, tick 回绕不出错.
 */
#include <string.h>
#include "tailgate.h"

static tg_out_t mk(tg_event_t ev, tg_action_t act, int delta)
{
    tg_out_t o;
    o.ev = ev;
    o.act = act;
    o.delta = delta;
    return o;
}

#define NOTHING mk(TG_EV_NONE, TG_ACT_NONE, 0)

static uint32_t since(uint32_t now, uint32_t t)
{
    return now - t;
}

static void enter(tg_t *tg, tg_state_t s, uint32_t now)
{
    tg->state = s;
    tg->t_state = now;
}

static void check_stale(tg_t *tg, int side, uint32_t now)
{
    tg_side_t *sd = &tg->side[side];
    if (sd->valid && since(now, sd->last_ms) > tg->cfg.stale_ms)
    {
        sd->valid = 0;   /* 断流: 恢复后重新建立基准, 不把断流前后的差当成人数变化 */
        sd->hits = 0;
    }
}

static int side_ok(tg_t *tg, int side, uint32_t now)
{
    check_stale(tg, side, now);
    return tg->side[side].valid;
}

/* 杆已关, 等人出现在 B 侧; B 侧没有摄像头就没法确认, 直接回 IDLE */
static void enter_passing(tg_t *tg, uint32_t now)
{
    if (side_ok(tg, TG_SIDE_B, now))
        enter(tg, TG_PASSING, now);
    else
        enter(tg, TG_IDLE, now);
}

static void start_watch(tg_t *tg, int b_now, uint32_t now)
{
    tg->watch = 1;
    tg->t_watch = now;
    tg->b_ref = b_now;   /* watch 期间基准固定为放行时的人数, 见 on_change_b */
    enter(tg, TG_IDLE, now);
}

void tg_default_cfg(tg_cfg_t *cfg)
{
    cfg->debounce = 2;
    cfg->open_ms  = 6000;
    cfg->pass_ms  = 4000;
    cfg->watch_ms = 3000;
    cfg->alarm_ms = 5000;
    cfg->stale_ms = 1500;
    cfg->min_open_ms = 0;    /* 默认关闭; 板端 tg_start 里按需开启(举手机宽限) */
}

void tg_init(tg_t *tg, const tg_cfg_t *cfg)
{
    memset(tg, 0, sizeof(*tg));
    if (cfg)
        tg->cfg = *cfg;
    else
        tg_default_cfg(&tg->cfg);
    if (tg->cfg.debounce == 0)
        tg->cfg.debounce = 1;
    tg->state = TG_IDLE;
    tg->b_ref = -1;
}

/* 某侧第一次(或断流恢复后)拿到稳定人数: 只建立基准, 旧基准作废 */
static void on_baseline(tg_t *tg, int side, int cnt)
{
    if (side == TG_SIDE_A)
        tg->a_peak = cnt;
    else
        tg->b_ref = cnt;
}

static tg_out_t on_change_a(tg_t *tg, int cnt, uint32_t now)
{
    int d;

    if (tg->state != TG_OPEN)
        return NOTHING;              /* 杆关着时排队区人数怎么变都不影响 */
    if (cnt >= tg->a_peak)
    {
        tg->a_peak = cnt;            /* 又有人来排队 */
        return NOTHING;
    }
    if (tg->cfg.min_open_ms && now - tg->t_state < tg->cfg.min_open_ms)
        return NOTHING;              /* 开杆宽限期: 举手机/走动造成的 A 侧人数闪动不关杆不告警 */
    d = tg->a_peak - cnt;
    tg->last_delta = d;
    if (d >= 2)
    {
        enter(tg, TG_ALARM, now);    /* 一次走掉两个人 = 贴身跟进 */
        return mk(TG_EV_TAILGATE, TG_ACT_CLOSE, d);
    }
    enter_passing(tg, now);          /* 走掉一个: 立即关杆 */
    return mk(TG_EV_A_LEFT, TG_ACT_CLOSE, d);
}

static tg_out_t on_change_b(tg_t *tg, int old, int cnt, uint32_t now)
{
    int d;
    int in_watch = (tg->state == TG_IDLE && tg->watch);

    if (tg->b_ref < 0)
        tg->b_ref = old;
    if (cnt <= tg->b_ref)
    {
        /* 门内有人走开: 基准跟着降. watch 期间不降, 否则刚放行的人被遮挡一下又出现会误报 */
        if (!in_watch)
            tg->b_ref = cnt;
        return NOTHING;
    }
    d = cnt - tg->b_ref;
    tg->last_delta = d;

    switch (tg->state)
    {
    case TG_OPEN:
        if (d >= 2)
        {
            enter(tg, TG_ALARM, now);
            return mk(TG_EV_TAILGATE, TG_ACT_CLOSE, d);
        }
        start_watch(tg, cnt, now);   /* A 侧没来得及判到, B 侧已看到人: 关杆并算放行完成 */
        return mk(TG_EV_B_FIRST, TG_ACT_CLOSE, d);

    case TG_PASSING:
        if (d >= 2)
        {
            enter(tg, TG_ALARM, now);
            return mk(TG_EV_TAILGATE, TG_ACT_CLOSE, d);
        }
        start_watch(tg, cnt, now);
        return mk(TG_EV_PASS_OK, TG_ACT_NONE, d);

    case TG_IDLE:
        if (in_watch && since(now, tg->t_watch) <= tg->cfg.watch_ms)
        {
            tg->watch = 0;
            enter(tg, TG_ALARM, now);   /* 放行刚完成, 门内又多出人: 从杆下钻过来的 */
            return mk(TG_EV_TAILGATE, TG_ACT_CLOSE, d);
        }
        tg->b_ref = cnt;
        return NOTHING;

    default:
        tg->b_ref = cnt;
        return NOTHING;
    }
}

tg_out_t tg_feed(tg_t *tg, int side, int count, uint32_t now_ms)
{
    tg_side_t *sd;
    int was_valid, old;

    if ((side != TG_SIDE_A && side != TG_SIDE_B) || count < 0)
        return NOTHING;
    sd = &tg->side[side];
    check_stale(tg, side, now_ms);
    sd->last_ms = now_ms;

    if (sd->hits > 0 && count == sd->cand)
        sd->hits++;
    else
    {
        sd->cand = count;
        sd->hits = 1;
    }
    if (sd->hits < tg->cfg.debounce)
        return NOTHING;

    was_valid = sd->valid;
    old = sd->stable;
    sd->stable = count;
    sd->valid = 1;
    if (!was_valid)
    {
        on_baseline(tg, side, count);
        return NOTHING;
    }
    if (count == old)
        return NOTHING;
    if (side == TG_SIDE_A)
        return on_change_a(tg, count, now_ms);
    return on_change_b(tg, old, count, now_ms);
}

tg_out_t tg_auth(tg_t *tg, uint32_t now_ms)
{
    if (tg->state == TG_ALARM)
        return mk(TG_EV_AUTH_ALARM, TG_ACT_NONE, 0);
    if (tg->state != TG_IDLE)
        return mk(TG_EV_AUTH_BUSY, TG_ACT_NONE, 0);
    tg->watch = 0;
    tg->a_peak = side_ok(tg, TG_SIDE_A, now_ms) ? tg->side[TG_SIDE_A].stable : 0;
    tg->b_ref = side_ok(tg, TG_SIDE_B, now_ms) ? tg->side[TG_SIDE_B].stable : -1;
    enter(tg, TG_OPEN, now_ms);
    return mk(TG_EV_AUTH_OPEN, TG_ACT_OPEN, 0);
}

tg_out_t tg_tick(tg_t *tg, uint32_t now_ms)
{
    uint32_t el;

    check_stale(tg, TG_SIDE_A, now_ms);
    check_stale(tg, TG_SIDE_B, now_ms);
    el = since(now_ms, tg->t_state);

    switch (tg->state)
    {
    case TG_OPEN:
        if (el >= tg->cfg.open_ms)
        {
            enter_passing(tg, now_ms);   /* 可能有人在最后一刻过去, 再给 B 侧一段时间 */
            return mk(TG_EV_OPEN_TIMEOUT, TG_ACT_CLOSE, 0);
        }
        break;
    case TG_PASSING:
        if (el >= tg->cfg.pass_ms || !tg->side[TG_SIDE_B].valid)
        {
            enter(tg, TG_IDLE, now_ms);
            return mk(TG_EV_PASS_TIMEOUT, TG_ACT_NONE, 0);
        }
        break;
    case TG_ALARM:
        if (el >= tg->cfg.alarm_ms)
        {
            enter(tg, TG_IDLE, now_ms);
            tg->b_ref = tg->side[TG_SIDE_B].valid ? tg->side[TG_SIDE_B].stable : -1;
            return mk(TG_EV_ALARM_CLEAR, TG_ACT_NONE, 0);
        }
        break;
    case TG_IDLE:
        if (tg->watch && since(now_ms, tg->t_watch) > tg->cfg.watch_ms)
        {
            tg->watch = 0;
            tg->b_ref = tg->side[TG_SIDE_B].valid ? tg->side[TG_SIDE_B].stable : -1;
        }
        break;
    }
    return NOTHING;
}

const char *tg_state_str(tg_state_t s)
{
    switch (s)
    {
    case TG_IDLE:    return "idle";
    case TG_OPEN:    return "open";
    case TG_PASSING: return "passing";
    case TG_ALARM:   return "alarm";
    }
    return "?";
}

const char *tg_event_str(tg_event_t e)
{
    switch (e)
    {
    case TG_EV_NONE:         return "none";
    case TG_EV_AUTH_OPEN:    return "auth-open";
    case TG_EV_AUTH_BUSY:    return "auth-busy";
    case TG_EV_AUTH_ALARM:   return "auth-alarm";
    case TG_EV_A_LEFT:       return "a-left";
    case TG_EV_B_FIRST:      return "b-first";
    case TG_EV_PASS_OK:      return "pass-ok";
    case TG_EV_TAILGATE:     return "TAILGATE";
    case TG_EV_OPEN_TIMEOUT: return "open-timeout";
    case TG_EV_PASS_TIMEOUT: return "pass-timeout";
    case TG_EV_ALARM_CLEAR:  return "alarm-clear";
    }
    return "?";
}
