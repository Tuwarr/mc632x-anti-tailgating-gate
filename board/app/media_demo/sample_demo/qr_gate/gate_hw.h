/*
 * gate_hw.h — qr_gate.c(舵机/扫码/TCP) 与 tg_gate.c(防尾随) 之间的接口
 */
#ifndef __GATE_HW_H__
#define __GATE_HW_H__

/* qr_gate.c: 门杆硬件 */
int  servo_hw_init(void);
void gate_open(void);
void gate_close(void);
void gate_pulse(const char *who);         /* 开杆, QR_GATE_HOLD_MS 后自动关 (防尾随关闭时用) */

/* tg_gate.c: 所有"授权开门"都走这里 */
#define GATE_REQ_OPENED  0
#define GATE_REQ_BUSY    1                /* 上一个人还没放行完 */
#define GATE_REQ_ALARM   2                /* 尾随报警保持中 */
#define GATE_REQ_NOHW   (-1)              /* PWM 不可用 */

int  gate_request(const char *who);       /* 授权开门; 防尾随关闭时等同 gate_pulse */
int  gate_can_open(void);                 /* 验签前预判, 忙时不消耗 nonce; 返回值同上 */
int  tg_start(void);                      /* 启动防尾随 tick 线程, 可重复调用 */
int  tg_status_line(char *buf, int size); /* "TG on state=idle a=2 b=0 pass=5 alarm=1 last=pass-ok" */

/* sample_nna_obj_detect.c 每帧人形检测后调用 (弱符号, 没有本模块时为空)
 * cx/cy: 框中心 0~1; conf: 置信度 0~1 */
void qr_gate_nn_report(int grp_id, int n, const float *cx, const float *cy, const float *conf);

#endif
