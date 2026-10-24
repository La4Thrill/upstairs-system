#ifndef CRYPTO_SHA256_H
#define CRYPTO_SHA256_H

/**
 * ============================================================
 *  crypto_sha256.c —— 轻量哈希与口令记录
 * ------------------------------------------------------------
 *  用途：不再明文存密码。
 *  口令记录格式（单列自描述，便于旧数据迁移）：
 *
 *      sha256$<32 位十六进制盐>$<64 位十六进制摘要>
 *
 *  校验时按记录里的盐重算摘要，避免"明文可比"与"盐复用"。
 * ============================================================
 */

#include <stddef.h>
#include <stdbool.h>

/** 计算 SHA-256 摘要 */
void crypto_sha256(const unsigned char* data, size_t len, unsigned char out[32]);

/** 计算 SHA-256 并输出 64 位小写十六进制（out 至少 65 字节） */
void crypto_sha256_hex(const unsigned char* data, size_t len, char* out);

/** 生成随机字节（内部 xorshift64*，种子混入时间/时钟/地址） */
void crypto_random_bytes(unsigned char* out, size_t n);

/** 生成 n 字节随机数的十六进制串（out 至少 2n+1 字节） */
void crypto_random_hex(char* out, size_t n);

/** 由明文口令生成口令记录（带新盐），out 至少 160 字节 */
bool crypto_password_record(const char* password, char* out, size_t out_len);

/** 校验明文口令与口令记录 */
bool crypto_password_verify(const char* password, const char* record);

/** 记录是否已经是哈希格式（用于把旧明文行就地升级） */
bool crypto_password_is_hashed(const char* record);

#endif /* CRYPTO_SHA256_H */
