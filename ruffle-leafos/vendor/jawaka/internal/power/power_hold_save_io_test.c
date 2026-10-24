#include "internal/power/power_hold_save_io.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char root[] = "/tmp/jawaka-power-hold-io.XXXXXX";

static void join(char *out, size_t size, const char *name) {
    assert(snprintf(out, size, "%s/%s", root, name) < (int)size);
}

static void write_file(const char *path, const char *text) {
    FILE *fp = fopen(path, "wb");
    assert(fp);
    fputs(text, fp);
    fclose(fp);
}

static int read_is(const char *path, const char *text) {
    char buf[64] = {0};
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    buf[n] = '\0';
    return strcmp(buf, text) == 0;
}

int main(void) {
    assert(mkdtemp(root));
    char final_path[512], thumb[512], tmp[512], err[512];
    join(final_path, sizeof(final_path), "Game (USA).state99");
    join(thumb, sizeof(thumb), "Game (USA).state99.png");

    assert(jw_power_hold_save_tmp_path(final_path, "a1b2", tmp, sizeof(tmp)));
    assert(strstr(tmp, "Game (USA).state99.tmp-a1b2"));
    char small[8];
    assert(!jw_power_hold_save_tmp_path(final_path, "a1b2", small, sizeof(small)));
    assert(!jw_power_hold_save_tmp_path(final_path, "", tmp, sizeof(tmp)));
    assert(jw_power_hold_save_tmp_path(final_path, "a1b2", tmp, sizeof(tmp)));

    /* A reply naming any other file is rejected; the old state stays. */
    write_file(final_path, "old");
    write_file(thumb, "old-frame");
    write_file(tmp, "new");
    char other[512];
    join(other, sizeof(other), "Other.state99.tmp-a1b2");
    write_file(other, "evil");
    assert(jw_power_hold_save_publish(tmp, other, final_path, thumb, err, sizeof(err)) ==
           JW_PHS_PUBLISH_REJECTED);
    assert(read_is(final_path, "old") && read_is(thumb, "old-frame"));
    unlink(other);

    /* A symlink or empty file at the pinned name is not a finished state. */
    unlink(tmp);
    assert(symlink(final_path, tmp) == 0);
    assert(jw_power_hold_save_publish(tmp, tmp, final_path, thumb, err, sizeof(err)) ==
           JW_PHS_PUBLISH_REJECTED);
    unlink(tmp);
    write_file(tmp, "");
    assert(jw_power_hold_save_publish(tmp, tmp, final_path, thumb, err, sizeof(err)) ==
           JW_PHS_PUBLISH_REJECTED);
    assert(read_is(final_path, "old"));

    /* Thumbnail that cannot be removed: nothing is published. */
    write_file(tmp, "new");
    unlink(thumb);
    assert(mkdir(thumb, 0755) == 0);
    char inside[600];
    snprintf(inside, sizeof(inside), "%s/x", thumb);
    write_file(inside, "x");
    assert(jw_power_hold_save_publish(tmp, tmp, final_path, thumb, err, sizeof(err)) ==
           JW_PHS_PUBLISH_THUMB_ERROR);
    assert(strstr(err, "thumbnail"));
    assert(read_is(final_path, "old") && read_is(tmp, "new"));
    unlink(inside);
    rmdir(thumb);

    /* Rename failure leaves the old slot alone. */
    char blocked_final[512], blocked_inside[600];
    join(blocked_final, sizeof(blocked_final), "Blocked.state99");
    assert(mkdir(blocked_final, 0755) == 0);
    snprintf(blocked_inside, sizeof(blocked_inside), "%s/keep", blocked_final);
    write_file(blocked_inside, "keep");
    assert(jw_power_hold_save_publish(tmp, tmp, blocked_final, NULL, err, sizeof(err)) ==
           JW_PHS_PUBLISH_RENAME_ERROR);
    assert(read_is(tmp, "new") && read_is(blocked_inside, "keep"));
    unlink(blocked_inside);
    rmdir(blocked_final);

    /* Success: old thumbnail gone, slot replaced, temporary name gone. */
    write_file(thumb, "old-frame");
    assert(jw_power_hold_save_publish(tmp, tmp, final_path, thumb, err, sizeof(err)) ==
           JW_PHS_PUBLISH_OK);
    assert(read_is(final_path, "new"));
    assert(access(thumb, F_OK) != 0 && access(tmp, F_OK) != 0);

    /* First save for a game: no old state, no thumbnail. */
    unlink(final_path);
    write_file(tmp, "first");
    assert(jw_power_hold_save_publish(tmp, tmp, final_path, thumb, err, sizeof(err)) ==
           JW_PHS_PUBLISH_OK);
    assert(read_is(final_path, "first"));

    /* Cleanup of an abandoned temporary file is idempotent. */
    write_file(tmp, "partial");
    assert(jw_power_hold_save_remove_tmp(tmp) == 0 && access(tmp, F_OK) != 0);
    assert(jw_power_hold_save_remove_tmp(tmp) == 0);
    assert(jw_power_hold_save_remove_tmp(NULL) == 0);

    /* Orphans of this stem go; the state, another stem's leftovers, names
       outside the id alphabet or length, and symlinks stay. */
    char orphan1[600], orphan2[600], other_orphan[600], bad_id[600], long_id[600],
         no_id[600], link_orphan[600];
    join(orphan1, sizeof(orphan1), "Game (USA).state99.tmp-a1b2");
    join(orphan2, sizeof(orphan2), "Game (USA).state99.tmp-Zz_-9");
    join(other_orphan, sizeof(other_orphan), "Other.state99.tmp-a1b2");
    join(bad_id, sizeof(bad_id), "Game (USA).state99.tmp-a1.b2");
    join(long_id, sizeof(long_id), "Game (USA).state99.tmp-"
         "abcdefghijklmnopqrstuvwxyz0123456");
    join(no_id, sizeof(no_id), "Game (USA).state99.tmp-");
    join(link_orphan, sizeof(link_orphan), "Game (USA).state99.tmp-link1");
    write_file(final_path, "state");
    write_file(orphan1, "partial");
    write_file(orphan2, "partial");
    write_file(other_orphan, "keep");
    write_file(bad_id, "keep");
    write_file(long_id, "keep");
    write_file(no_id, "keep");
    assert(symlink(final_path, link_orphan) == 0);
    assert(jw_power_hold_save_remove_orphans(final_path) == 2);
    assert(access(orphan1, F_OK) != 0 && access(orphan2, F_OK) != 0);
    assert(read_is(final_path, "state"));
    assert(read_is(other_orphan, "keep") && read_is(bad_id, "keep") &&
           read_is(long_id, "keep") && read_is(no_id, "keep"));
    struct stat link_st;
    assert(lstat(link_orphan, &link_st) == 0 && S_ISLNK(link_st.st_mode));
    assert(read_is(final_path, "state"));
    /* Nothing left to remove; a missing directory is reported, not fatal. */
    assert(jw_power_hold_save_remove_orphans(final_path) == 0);
    char missing[600];
    join(missing, sizeof(missing), "no-such-dir/Game.state99");
    assert(jw_power_hold_save_remove_orphans(missing) == -1);
    assert(jw_power_hold_save_remove_orphans(NULL) == -1);
    assert(jw_power_hold_save_remove_orphans("") == -1);
    unlink(other_orphan);
    unlink(bad_id);
    unlink(long_id);
    unlink(no_id);
    unlink(link_orphan);

    unlink(final_path);
    rmdir(root);
    puts("PASS power-hold-save-io-test");
    return 0;
}
