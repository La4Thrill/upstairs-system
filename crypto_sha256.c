/**
 * ============================================================
 *  crypto_sha256.c —— SHA-256 + 加盐口令记录
 *
 *  说明：
 *    · SHA-256 为公开标准算法（FIPS 180-4），此处是紧凑实现，
 *      不依赖 OpenSSL，便于在课程环境里静态编译；
 *    · 口令不落明文：库里只存 sha256$<盐>$<摘要>；
 *    · 盐用 xorshift64* 生成，种子混入时间、clock()、变量地址。
 *      课程演示足够；生产环境请换 CSPRNG 并考虑 PBKDF2/Argon2。
 * ============================================================
 */

#include "crypto_sha256.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ---------------- SHA-256 ---------------- */

typedef struct {
    uint32_t h[8];
    uint64_t len;      /* 总字节数 */
    unsigned char buf[64];
    size_t buf_len;
} Sha256Ctx;

static const uint32_t K[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(Sha256Ctx* c, const unsigned char* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    }
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = ROTR(w[i - 15], 7) ^ ROTR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROTR(w[i - 2], 17) ^ ROTR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3];
    uint32_t e = c->h[4], f = c->h[5], g = c->h[6], hh = c->h[7];

    for (int i = 0; i < 64; ++i) {
        uint32_t S1 = ROTR(e, 6) ^ ROTR(e, 11) ^ ROTR(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = hh + S1 + ch + K[i] + w[i];
        uint32_t S0 = ROTR(a, 2) ^ ROTR(a, 13) ^ ROTR(a, 22);
        uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + maj;

        hh = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }

    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += hh;
}

static void sha256_init(Sha256Ctx* c) {
    c->h[0] = 0x6a09e667u; c->h[1] = 0xbb67ae85u; c->h[2] = 0x3c6ef372u; c->h[3] = 0xa54ff53au;
    c->h[4] = 0x510e527fu; c->h[5] = 0x9b05688cu; c->h[6] = 0x1f83d9abu; c->h[7] = 0x5be0cd19u;
    c->len = 0;
    c->buf_len = 0;
}

static void sha256_update(Sha256Ctx* c, const unsigned char* data, size_t len) {
    c->len += (uint64_t)len;

    if (c->buf_len) {
        size_t need = 64 - c->buf_len;
        size_t take = (len < need) ? len : need;
        memcpy(c->buf + c->buf_len, data, take);
        c->buf_len += take;
        data += take;
        len -= take;
        if (c->buf_len == 64) {
            sha256_block(c, c->buf);
            c->buf_len = 0;
        }
    }

    while (len >= 64) {
        sha256_block(c, data);
        data += 64;
        len -= 64;
    }

    if (len) {
        memcpy(c->buf, data, len);
        c->buf_len = len;
    }
}

static void sha256_final(Sha256Ctx* c, unsigned char out[32]) {
    uint64_t bits = c->len * 8;

    unsigned char pad = 0x80;
    sha256_update(c, &pad, 1);
    c->len -= 1;   /* pad 不计入长度 */

    unsigned char zero = 0x00;
    while (c->buf_len != 56) {
        sha256_update(c, &zero, 1);
        c->len -= 1;
    }

    unsigned char lenbuf[8];
    for (int i = 0; i < 8; ++i) {
        lenbuf[i] = (unsigned char)((bits >> (56 - 8 * i)) & 0xFF);
    }
    sha256_update(c, lenbuf, 8);

    for (int i = 0; i < 8; ++i) {
        out[i * 4]     = (unsigned char)((c->h[i] >> 24) & 0xFF);
        out[i * 4 + 1] = (unsigned char)((c->h[i] >> 16) & 0xFF);
        out[i * 4 + 2] = (unsigned char)((c->h[i] >> 8) & 0xFF);
        out[i * 4 + 3] = (unsigned char)(c->h[i] & 0xFF);
    }
}

void crypto_sha256(const unsigned char* data, size_t len, unsigned char out[32]) {
    Sha256Ctx c;
    sha256_init(&c);
    sha256_update(&c, data, len);
    sha256_final(&c, out);
}

void crypto_sha256_hex(const unsigned char* data, size_t len, char* out) {
    static const char hex[] = "0123456789abcdef";
    unsigned char d[32];
    crypto_sha256(data, len, d);
    for (int i = 0; i < 32; ++i) {
        out[i * 2] = hex[(d[i] >> 4) & 0xF];
        out[i * 2 + 1] = hex[d[i] & 0xF];
    }
    out[64] = '\0';
}

/* ---------------- 随机数 ---------------- */

static uint64_t g_rng = 0;
static uint64_t g_rng_counter = 0;

static void rng_seed_once(void) {
    if (g_rng != 0) {
        return;
    }
    uint64_t seed = (uint64_t)time(NULL) * 1000003ULL;
    seed ^= (uint64_t)clock() << 21;
    seed ^= (uint64_t)(uintptr_t)&g_rng;
    g_rng = seed ? seed : 0x9E3779B97F4A7C15ULL;
}

static uint64_t rng_next(void) {
    rng_seed_once();
    uint64_t x = g_rng;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    g_rng = x;
    g_rng_counter++;
    return x * 2685821657736338717ULL + g_rng_counter;
}

void crypto_random_bytes(unsigned char* out, size_t n) {
    if (!out) {
        return;
    }
    for (size_t i = 0; i < n; ++i) {
        out[i] = (unsigned char)(rng_next() & 0xFF);
    }
}

void crypto_random_hex(char* out, size_t n) {
    static const char hex[] = "0123456789abcdef";
    if (!out) {
        return;
    }
    for (size_t i = 0; i < n; ++i) {
        unsigned char b = (unsigned char)(rng_next() & 0xFF);
        out[i * 2] = hex[(b >> 4) & 0xF];
        out[i * 2 + 1] = hex[b & 0xF];
    }
    out[n * 2] = '\0';
}

/* ---------------- 口令记录 ---------------- */

#define CRYPTO_SALT_HEX 32   /* 16 字节盐 */
#define CRYPTO_RECORD_PREFIX "sha256$"

bool crypto_password_is_hashed(const char* record) {
    return record && strncmp(record, CRYPTO_RECORD_PREFIX, strlen(CRYPTO_RECORD_PREFIX)) == 0;
}

bool crypto_password_record(const char* password, char* out, size_t out_len) {
    if (!password || !out || out_len < CRYPTO_SALT_HEX + 64 + 16) {
        return false;
    }

    char salt[CRYPTO_SALT_HEX + 1];
    crypto_random_hex(salt, CRYPTO_SALT_HEX / 2);

    char material[CRYPTO_SALT_HEX + 256];
    int n = snprintf(material, sizeof(material), "%s%s", salt, password);
    if (n <= 0 || n >= (int)sizeof(material)) {
        return false;
    }

    char digest[65];
    crypto_sha256_hex((const unsigned char*)material, (size_t)n, digest);

    snprintf(out, out_len, "%s%s$%s", CRYPTO_RECORD_PREFIX, salt, digest);
    return true;
}

bool crypto_password_verify(const char* password, const char* record) {
    if (!password || !record) {
        return false;
    }

    if (!crypto_password_is_hashed(record)) {
        /* 旧数据：明文比较（调用方应尽快升级成哈希记录） */
        return strcmp(password, record) == 0;
    }

    const char* salt = record + strlen(CRYPTO_RECORD_PREFIX);
    const char* sep = strchr(salt, '$');
    if (!sep) {
        return false;
    }

    char salt_buf[CRYPTO_SALT_HEX + 1];
    size_t salt_len = (size_t)(sep - salt);
    if (salt_len == 0 || salt_len > CRYPTO_SALT_HEX) {
        return false;
    }
    memcpy(salt_buf, salt, salt_len);
    salt_buf[salt_len] = '\0';

    const char* stored = sep + 1;

    char material[CRYPTO_SALT_HEX + 256];
    int n = snprintf(material, sizeof(material), "%s%s", salt_buf, password);
    if (n <= 0 || n >= (int)sizeof(material)) {
        return false;
    }

    char digest[65];
    crypto_sha256_hex((const unsigned char*)material, (size_t)n, digest);

    return strcmp(digest, stored) == 0;
}
