#ifndef JW_UPDATE_SHA256_H
#define JW_UPDATE_SHA256_H

#include <stddef.h>
#include <stdint.h>

/* Streaming API. The effective-catalog tree hash (CAT-1) feeds many files
   through one digest with a length prefix between them, which a one-shot
   over a single buffer cannot express without first concatenating every
   .info file in memory. */
typedef struct {
    uint32_t state[8];
    uint64_t bit_count;
    unsigned char buffer[64];
} jw_sha256_ctx;

void jw_sha256_init(jw_sha256_ctx *ctx);
void jw_sha256_update(jw_sha256_ctx *ctx, const void *data, size_t len);
void jw_sha256_final_hex(jw_sha256_ctx *ctx, char out_hex[65]);

int jw_sha256_file_hex(const char *path,
                       char out_hex[65],
                       char *error,
                       size_t error_size);

/* One-shot SHA-256 of an in-memory buffer, written as a 64-char lowercase hex
   string (NUL-terminated) into out_hex. */
void jw_sha256_buf_hex(const void *data, size_t len, char out_hex[65]);

#endif /* JW_UPDATE_SHA256_H */
