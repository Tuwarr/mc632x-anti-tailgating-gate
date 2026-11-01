/*
 * 二维码载荷校验（protocol.md §1-§5）
 * 不依赖 RT-Thread，仅用 mbedtls_sha256，可在 PC 上单测（见 _build/qr_door_test/）
 */
#include <string.h>
#include "mbedtls/sha256.h"
#include "qr_verify.h"

#define SHA256_BLOCK 64

void qr_hmac_sha256_hex(const char *key, size_t key_len,
                        const char *msg, size_t msg_len, char out_hex[QR_HMAC_HEX_LEN + 1])
{
    static const char hex[] = "0123456789abcdef";
    unsigned char k[SHA256_BLOCK];
    unsigned char pad[SHA256_BLOCK];
    unsigned char inner[32];
    unsigned char digest[32];
    mbedtls_sha256_context ctx;
    int i;

    memset(k, 0, sizeof(k));
    if (key_len > SHA256_BLOCK)
        mbedtls_sha256((const unsigned char *)key, key_len, k, 0);
    else
        memcpy(k, key, key_len);

    mbedtls_sha256_init(&ctx);

    for (i = 0; i < SHA256_BLOCK; i++)
        pad[i] = k[i] ^ 0x36;
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, pad, SHA256_BLOCK);
    mbedtls_sha256_update(&ctx, (const unsigned char *)msg, msg_len);
    mbedtls_sha256_finish(&ctx, inner);

    for (i = 0; i < SHA256_BLOCK; i++)
        pad[i] = k[i] ^ 0x5c;
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, pad, SHA256_BLOCK);
    mbedtls_sha256_update(&ctx, inner, sizeof(inner));
    mbedtls_sha256_finish(&ctx, digest);

    mbedtls_sha256_free(&ctx);

    /* 只写 QR_HMAC_HEX_LEN 个字符: 缓冲区是 out_hex[QR_HMAC_HEX_LEN + 1],
     * 硬编码 64 位写入在 QR_HMAC_HEX_LEN 缩短后(32)会越界 31 字节腐蚀调用方栈 */
    for (i = 0; i < QR_HMAC_HEX_LEN / 2; i++)
    {
        out_hex[i * 2] = hex[digest[i] >> 4];
        out_hex[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    out_hex[QR_HMAC_HEX_LEN] = '\0';
}

static int const_time_eq(const char *a, const char *b, size_t n)
{
    unsigned char diff = 0;
    size_t i;

    for (i = 0; i < n; i++)
        diff |= (unsigned char)(a[i] ^ b[i]);
    return diff == 0;
}

static int is_digits(const char *s, size_t n)
{
    size_t i;

    if (n == 0)
        return 0;
    for (i = 0; i < n; i++)
        if (s[i] < '0' || s[i] > '9')
            return 0;
    return 1;
}

static int is_lower_hex(const char *s, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
            return 0;
    return 1;
}

void qr_nonce_ring_init(qr_nonce_ring_t *ring)
{
    memset(ring, 0, sizeof(*ring));
}

#if QR_EN_NONCE_CHECK
static int nonce_seen(qr_nonce_ring_t *ring, const char *nonce, uint32_t now)
{
    int i;

    for (i = 0; i < QR_NONCE_RING; i++)
    {
        qr_nonce_slot_t *s = &ring->slot[i];

        if (!s->used)
            continue;
        if (now > s->ts + QR_NONCE_KEEP_S + QR_TIME_WINDOW_S)
        {
            s->used = 0;    /* 已出窗, 清理 */
            continue;
        }
        if (memcmp(s->nonce, nonce, QR_NONCE_LEN) == 0)
            return 1;
    }
    return 0;
}

static void nonce_add(qr_nonce_ring_t *ring, const char *nonce, uint32_t ts)
{
    qr_nonce_slot_t *s = &ring->slot[ring->next];

    memcpy(s->nonce, nonce, QR_NONCE_LEN);
    s->nonce[QR_NONCE_LEN] = '\0';
    s->ts = ts;
    s->used = 1;
    ring->next = (ring->next + 1) % QR_NONCE_RING;
}
#endif

qr_result_t qr_verify(const char *text, size_t len, uint32_t now, qr_nonce_ring_t *ring)
{
    const char *seg[4];
    size_t seg_len[4];
    size_t i, start = 0;
    int n = 0;
    char hmac[QR_HMAC_HEX_LEN + 1];
    uint32_t ts = 0;
    int64_t dt;

    /* 1. 格式: 恰好 4 段 */
    for (i = 0; i <= len; i++)
    {
        if (i == len || text[i] == '|')
        {
            if (n >= 4)
                return QR_ERR_FORMAT;
            seg[n] = text + start;
            seg_len[n] = i - start;
            n++;
            start = i + 1;
        }
    }
    if (n != 4)
        return QR_ERR_FORMAT;
    if (!is_digits(seg[1], seg_len[1]) || seg_len[1] > 10)
        return QR_ERR_FORMAT;
    if (seg_len[2] != QR_NONCE_LEN || !is_lower_hex(seg[2], seg_len[2]))
        return QR_ERR_FORMAT;
    if (seg_len[3] != QR_HMAC_HEX_LEN)
        return QR_ERR_FORMAT;
    if (seg_len[0] != strlen(QR_USER) || memcmp(seg[0], QR_USER, seg_len[0]) != 0)
        return QR_ERR_USER;

    for (i = 0; i < seg_len[1]; i++)
        ts = ts * 10 + (uint32_t)(seg[1][i] - '0');

    /* 2. 签名: msg = user|ts|nonce, 常量时间比较 */
    qr_hmac_sha256_hex(QR_DEMO_KEY, strlen(QR_DEMO_KEY),
                       text, seg_len[0] + 1 + seg_len[1] + 1 + seg_len[2], hmac);
    if (!const_time_eq(hmac, seg[3], QR_HMAC_HEX_LEN))
        return QR_ERR_HMAC;

    /* 3. 时间窗 */
#if QR_EN_TIME_WINDOW
    dt = (int64_t)ts - (int64_t)now;
    if (dt > QR_TIME_WINDOW_S || dt < -QR_TIME_WINDOW_S)
        return QR_ERR_TIME;
#else
    (void)dt;
    (void)now;
#endif

    /* 4. nonce 查重 */
#if QR_EN_NONCE_CHECK
    if (nonce_seen(ring, seg[2], now))
        return QR_ERR_REPLAY;
    nonce_add(ring, seg[2], ts);
#else
    (void)ring;
#endif

    return QR_OK;
}

const char *qr_result_str(qr_result_t r)
{
    switch (r)
    {
    case QR_OK:         return "ok";
    case QR_ERR_FORMAT: return "format";
    case QR_ERR_USER:   return "user";
    case QR_ERR_HMAC:   return "hmac";
    case QR_ERR_TIME:   return "time window";
    case QR_ERR_REPLAY: return "replay";
    }
    return "unknown";
}
