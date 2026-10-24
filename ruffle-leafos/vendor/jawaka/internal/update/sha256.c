#include "internal/update/sha256.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const uint32_t JW_SHA256_K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static uint32_t jw__rotr32(uint32_t x, unsigned n) {
    return (x >> n) | (x << (32u - n));
}

static uint32_t jw__load_be32(const unsigned char *p) {
    return ((uint32_t)p[0] << 24u) |
           ((uint32_t)p[1] << 16u) |
           ((uint32_t)p[2] << 8u) |
           (uint32_t)p[3];
}

static void jw__store_be32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v >> 24u);
    p[1] = (unsigned char)(v >> 16u);
    p[2] = (unsigned char)(v >> 8u);
    p[3] = (unsigned char)v;
}

static void jw__store_be64(unsigned char *p, uint64_t v) {
    for (int i = 7; i >= 0; i--) {
        p[i] = (unsigned char)v;
        v >>= 8u;
    }
}

static void jw__sha256_transform(jw_sha256_ctx *ctx,
                                 const unsigned char block[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = jw__load_be32(block + (i * 4));
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = jw__rotr32(w[i - 15], 7) ^
                      jw__rotr32(w[i - 15], 18) ^
                      (w[i - 15] >> 3u);
        uint32_t s1 = jw__rotr32(w[i - 2], 17) ^
                      jw__rotr32(w[i - 2], 19) ^
                      (w[i - 2] >> 10u);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = ctx->state[0];
    uint32_t b = ctx->state[1];
    uint32_t c = ctx->state[2];
    uint32_t d = ctx->state[3];
    uint32_t e = ctx->state[4];
    uint32_t f = ctx->state[5];
    uint32_t g = ctx->state[6];
    uint32_t h = ctx->state[7];

    for (int i = 0; i < 64; i++) {
        uint32_t s1 = jw__rotr32(e, 6) ^ jw__rotr32(e, 11) ^ jw__rotr32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + s1 + ch + JW_SHA256_K[i] + w[i];
        uint32_t s0 = jw__rotr32(a, 2) ^ jw__rotr32(a, 13) ^ jw__rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

void jw_sha256_init(jw_sha256_ctx *ctx) {
    ctx->state[0] = 0x6a09e667u;
    ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u;
    ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu;
    ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu;
    ctx->state[7] = 0x5be0cd19u;
    ctx->bit_count = 0;
    memset(ctx->buffer, 0, sizeof(ctx->buffer));
}

void jw_sha256_update(jw_sha256_ctx *ctx, const void *data_in, size_t len) {
    const unsigned char *data = (const unsigned char *)data_in;
    if (!ctx || !data || len == 0) {
        return;
    }
    size_t used = (size_t)((ctx->bit_count >> 3u) & 63u);
    ctx->bit_count += (uint64_t)len * 8u;

    if (used > 0) {
        size_t free_space = 64u - used;
        if (len < free_space) {
            memcpy(ctx->buffer + used, data, len);
            return;
        }
        memcpy(ctx->buffer + used, data, free_space);
        jw__sha256_transform(ctx, ctx->buffer);
        data += free_space;
        len -= free_space;
    }

    while (len >= 64u) {
        jw__sha256_transform(ctx, data);
        data += 64u;
        len -= 64u;
    }

    if (len > 0) {
        memcpy(ctx->buffer, data, len);
    }
}

static void jw__sha256_final(jw_sha256_ctx *ctx, unsigned char digest[32]) {
    unsigned char pad[64];
    memset(pad, 0, sizeof(pad));
    pad[0] = 0x80u;

    unsigned char len_be[8];
    jw__store_be64(len_be, ctx->bit_count);

    size_t used = (size_t)((ctx->bit_count >> 3u) & 63u);
    size_t pad_len = (used < 56u) ? (56u - used) : (120u - used);
    jw_sha256_update(ctx, pad, pad_len);
    jw_sha256_update(ctx, len_be, sizeof(len_be));

    for (int i = 0; i < 8; i++) {
        jw__store_be32(digest + (i * 4), ctx->state[i]);
    }
}

static void jw__sha256_digest_to_hex(const unsigned char digest[32],
                                     char out_hex[65]) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out_hex[i * 2] = hex[digest[i] >> 4u];
        out_hex[i * 2 + 1] = hex[digest[i] & 0x0fu];
    }
    out_hex[64] = '\0';
}

void jw_sha256_final_hex(jw_sha256_ctx *ctx, char out_hex[65]) {
    if (!ctx || !out_hex) {
        return;
    }
    unsigned char digest[32];
    jw__sha256_final(ctx, digest);
    jw__sha256_digest_to_hex(digest, out_hex);
}

void jw_sha256_buf_hex(const void *data, size_t len, char out_hex[65]) {
    if (!out_hex) {
        return;
    }
    jw_sha256_ctx ctx;
    jw_sha256_init(&ctx);
    if (data && len > 0) {
        jw_sha256_update(&ctx, (const unsigned char *)data, len);
    }
    unsigned char digest[32];
    jw__sha256_final(&ctx, digest);
    jw__sha256_digest_to_hex(digest, out_hex);
}

int jw_sha256_file_hex(const char *path,
                       char out_hex[65],
                       char *error,
                       size_t error_size) {
    if (out_hex) {
        out_hex[0] = '\0';
    }
    if (error && error_size > 0) {
        error[0] = '\0';
    }
    if (!path || !path[0] || !out_hex) {
        if (error && error_size > 0) {
            snprintf(error, error_size, "%s", "missing SHA-256 input");
        }
        return -1;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        if (error && error_size > 0) {
            snprintf(error, error_size, "cannot open %s", path);
        }
        return -1;
    }

    jw_sha256_ctx ctx;
    jw_sha256_init(&ctx);

    unsigned char buf[32768];
    while (1) {
        size_t got = fread(buf, 1, sizeof(buf), fp);
        if (got > 0) {
            jw_sha256_update(&ctx, buf, got);
        }
        if (got < sizeof(buf)) {
            if (ferror(fp)) {
                fclose(fp);
                if (error && error_size > 0) {
                    snprintf(error, error_size, "cannot read %s", path);
                }
                return -1;
            }
            break;
        }
    }
    fclose(fp);

    unsigned char digest[32];
    jw__sha256_final(&ctx, digest);
    jw__sha256_digest_to_hex(digest, out_hex);
    return 0;
}
