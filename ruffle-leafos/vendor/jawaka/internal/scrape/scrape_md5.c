#include "internal/scrape/scrape_md5.h"

#include "md5.h"
#include "miniz.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static int jw__md5_stream_file(const char *path, char md5_hex_out[33],
                               long *size_out) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;

    md5_ctx ctx;
    md5_init(&ctx);

    unsigned char buf[8192];
    size_t n;
    long total = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        md5_update(&ctx, buf, n);
        total += (long)n;
    }
    int read_error = ferror(f);
    fclose(f);
    if (read_error)
        return -1;

    uint8_t digest[16];
    md5_final(&ctx, digest);
    md5_hex(digest, md5_hex_out);
    *size_out = total;
    return 0;
}

static size_t jw__md5_zip_cb(void *opaque, mz_uint64 file_ofs,
                             const void *buf, size_t n) {
    (void)file_ofs;
    md5_update((md5_ctx *)opaque, buf, n);
    return n;
}

static int jw__md5_zip_largest(const char *zip_path, char md5_hex_out[33],
                               long *size_out) {
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, zip_path, 0))
        return -1;

    int num_files = (int)mz_zip_reader_get_num_files(&zip);
    int largest_idx = -1;
    mz_uint64 largest_size = 0;
    for (int i = 0; i < num_files; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, (mz_uint)i, &st) || st.m_is_directory)
            continue;
        if (st.m_uncomp_size > largest_size) {
            largest_size = st.m_uncomp_size;
            largest_idx = i;
        }
    }

    if (largest_idx < 0) {
        mz_zip_reader_end(&zip);
        return 1;
    }
    if (largest_size > (mz_uint64)JW_SCRAPE_MD5_MAX_BYTES) {
        mz_zip_reader_end(&zip);
        return 1;
    }

    md5_ctx ctx;
    md5_init(&ctx);
    mz_bool ok = mz_zip_reader_extract_to_callback(&zip, (mz_uint)largest_idx,
                                                   jw__md5_zip_cb, &ctx, 0);
    mz_zip_reader_end(&zip);
    if (!ok)
        return -1;

    uint8_t digest[16];
    md5_final(&ctx, digest);
    md5_hex(digest, md5_hex_out);
    *size_out = (long)largest_size;
    return 0;
}

int jw_scrape_md5(const char *abs_path, char md5_hex_out[33], long *size_out) {
    if (!abs_path || !md5_hex_out || !size_out)
        return -1;
    md5_hex_out[0] = '\0';
    *size_out = 0;

    struct stat st;
    if (stat(abs_path, &st) != 0 || !S_ISREG(st.st_mode))
        return -1;

    size_t len = strlen(abs_path);
    if (len > 4 && strcasecmp(abs_path + len - 4, ".zip") == 0)
        return jw__md5_zip_largest(abs_path, md5_hex_out, size_out);

    if (st.st_size > JW_SCRAPE_MD5_MAX_BYTES)
        return 1;

    return jw__md5_stream_file(abs_path, md5_hex_out, size_out);
}
