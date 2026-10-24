#ifndef JW_SCRAPE_MD5_H
#define JW_SCRAPE_MD5_H

/* ROM hashing for ScreenScraper md5 disambiguation.

   Files larger than JW_SCRAPE_MD5_MAX_BYTES are not hashed: full-file md5
   of CD images is too slow on device, and name+system lookup carries those.
   Zip archives hash their largest member (streamed, same size cap), which
   matches how ScreenScraper indexes zipped roms. */

#define JW_SCRAPE_MD5_MAX_BYTES (64L * 1024L * 1024L)

/* Returns 0 on success (md5_hex_out + size_out filled), 1 when skipped
   (file too large / archive without a hashable member), -1 on error. */
int jw_scrape_md5(const char *abs_path, char md5_hex_out[33], long *size_out);

#endif /* JW_SCRAPE_MD5_H */
