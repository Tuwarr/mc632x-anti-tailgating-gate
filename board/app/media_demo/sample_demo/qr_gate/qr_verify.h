#ifndef __QR_VERIFY_H__
#define __QR_VERIFY_H__
/*
 * 二维码载荷校验（_build/qrtool/protocol.md §1-§5）
 * 纯逻辑，不依赖 RT-Thread；PC 单测见 _build/qr_door_test/
 */
#include <stdint.h>
#include <stddef.h>

#define QR_DEMO_KEY            "mc632x-demo-key-2026"   /* 量产必须改, 见 protocol.md §8 */
#define QR_USER                "d1"                     /* 短 uid: 缩短载荷让二维码更稀疏, 远距解码更稳 */
#define QR_TIME_WINDOW_S       30
#define QR_NONCE_LEN           8
#define QR_HMAC_HEX_LEN        32    /* HMAC-SHA256 截断为 128 位: 常量时间前缀比较, 抗碰撞性足够门禁场景 */
#define QR_NONCE_RING          16
#define QR_NONCE_KEEP_S        60

#ifndef QR_EN_TIME_WINDOW
#ifdef QR_SKIP_TIME_WINDOW
#define QR_EN_TIME_WINDOW      0
#else
#define QR_EN_TIME_WINDOW      1     /* 0 仅调试: 跳过时间窗 */
#endif
#endif
#ifndef QR_EN_NONCE_CHECK
#define QR_EN_NONCE_CHECK      1     /* 0 仅调试: 跳过 nonce 查重 */
#endif

typedef enum
{
    QR_OK = 0,
    QR_ERR_FORMAT,
    QR_ERR_USER,
    QR_ERR_HMAC,
    QR_ERR_TIME,
    QR_ERR_REPLAY,
} qr_result_t;

typedef struct
{
    char     nonce[QR_NONCE_LEN + 1];
    uint32_t ts;
    int      used;
} qr_nonce_slot_t;

typedef struct
{
    qr_nonce_slot_t slot[QR_NONCE_RING];
    int             next;
} qr_nonce_ring_t;

void        qr_hmac_sha256_hex(const char *key, size_t key_len,
                               const char *msg, size_t msg_len, char out_hex[QR_HMAC_HEX_LEN + 1]);
void        qr_nonce_ring_init(qr_nonce_ring_t *ring);
/* 顺序: 格式 -> HMAC(常量时间) -> 时间窗 -> nonce 查重; 只有全部通过才把 nonce 入表 */
qr_result_t qr_verify(const char *text, size_t len, uint32_t now, qr_nonce_ring_t *ring);
const char *qr_result_str(qr_result_t r);

#endif /* __QR_VERIFY_H__ */
