#include "internal/storage/health.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Captured on an MLP1 whose launcher card flipped read-only (2026-09-14). The
   ROM card stays writable; the launcher card keeps rw in its mount options
   while the superblock reports ro. */
static const char *kMountinfo =
    "18 1 179:6 / / rw,relatime - ext4 /dev/root rw\n"
    "32 18 179:12 / /userdata rw,relatime - ext4 /dev/mmcblk0p12 rw\n"
    "33 18 179:128 / /mnt/sdcard rw,nosuid,nodev,noatime,nodiratime - vfat /dev/mmcblk3 "
    "rw,fmask=0022,dmask=0022,codepage=936,iocharset=utf8,shortname=mixed,errors=remount-ro\n"
    "34 18 179:96 / /media/sdcard1 rw,nosuid,nodev,noatime,nodiratime - vfat /dev/mmcblk1 "
    "ro,fmask=0022,dmask=0022,codepage=936,iocharset=utf8,shortname=mixed,errors=remount-ro\n"
    "40 18 0:30 / /media/with\\040space rw shared:1 - tmpfs tmpfs rw\n";

static const char *kKernelLog =
    "<6>[    3.114109] mmcblk1: mmc1:b36a SDABC 58.2 GiB \n"
    "<6>[    3.824421] mmcblk3: mmc3:b368 SDABC 58.2 GiB \n"
    "<4>[    4.472212] FAT-fs (mmcblk3): utf8 is not a recommended IO charset for FAT filesystems, filesystem will be case sensitive!\n"
    "<4>[    4.475912] FAT-fs (mmcblk3): Volume was not properly unmounted. Some data may be corrupt. Please run fsck.\n"
    "<4>[    9.477216] FAT-fs (mmcblk1): utf8 is not a recommended IO charset for FAT filesystems, filesystem will be case sensitive!\n"
    "<4>[    9.483844] FAT-fs (mmcblk1): Volume was not properly unmounted. Some data may be corrupt. Please run fsck.\n"
    "<3>[   22.187693] FAT-fs (mmcblk1): error, fat_free_clusters: deleting FAT entry beyond EOF\n"
    "<3>[   22.187788] FAT-fs (mmcblk1): Filesystem has been set read-only\n";

static char g_tmp[512];

static void write_file(const char *path, const char *text) {
    FILE *fp = fopen(path, "w");
    assert(fp);
    fputs(text, fp);
    fclose(fp);
}

static void mkdir_p(const char *path) {
    char work[1024];
    snprintf(work, sizeof(work), "%s", path);
    for (char *p = work + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(work, 0755);
            *p = '/';
        }
    }
    mkdir(work, 0755);
}

static void fixture_env(jw_storage_probe_env *env, const char *mountinfo_text) {
    char path[1024];
    jw_storage_probe_env_default(env);
    static char mountinfo[1024], by_uuid[1024], by_label[1024], sys_block[1024], repair[1024];
    snprintf(mountinfo, sizeof(mountinfo), "%s/mountinfo", g_tmp);
    write_file(mountinfo, mountinfo_text);
    snprintf(by_uuid, sizeof(by_uuid), "%s/by-uuid", g_tmp);
    snprintf(by_label, sizeof(by_label), "%s/by-label", g_tmp);
    snprintf(sys_block, sizeof(sys_block), "%s/sys-dev-block", g_tmp);
    snprintf(repair, sizeof(repair), "%s/storage-repair", g_tmp);
    mkdir_p(by_uuid);
    mkdir_p(by_label);
    mkdir_p(repair);
    snprintf(path, sizeof(path), "%s/22A4-0814", by_uuid);
    unlink(path);
    assert(symlink("../../mmcblk1", path) == 0);
    snprintf(path, sizeof(path), "%s/04B1-0820", by_uuid);
    unlink(path);
    assert(symlink("../../mmcblk3", path) == 0);
    snprintf(path, sizeof(path), "%s/MLPPRDLEAF", by_label);
    unlink(path);
    assert(symlink("../../mmcblk1", path) == 0);
    snprintf(path, sizeof(path), "%s/179:96", sys_block);
    mkdir_p(path);
    snprintf(path, sizeof(path), "%s/179:96/ro", sys_block);
    write_file(path, "0\n");
    snprintf(path, sizeof(path), "%s/179:128", sys_block);
    mkdir_p(path);
    snprintf(path, sizeof(path), "%s/179:128/ro", sys_block);
    write_file(path, "1\n");
    env->mountinfo_path = mountinfo;
    env->by_uuid_dir = by_uuid;
    env->by_label_dir = by_label;
    env->sys_dev_block = sys_block;
    env->repair_dir = repair;
    env->require_mounted_roots = true;
    env->skip_statvfs = true;
}

static void test_mount_table(void) {
    jw_storage_mount_table table;
    assert(jw_storage_mount_table_parse(kMountinfo, &table) == 0);
    assert(table.count == 5);
    const jw_storage_mount *launcher = jw_storage_mount_at(&table, "/media/sdcard1");
    assert(launcher && launcher->read_only);
    assert(launcher->major == 179 && launcher->minor == 96);
    assert(strcmp(launcher->device, "/dev/mmcblk1") == 0);
    assert(strcmp(launcher->fs_type, "vfat") == 0);
    const jw_storage_mount *roms = jw_storage_mount_at(&table, "/mnt/sdcard");
    assert(roms && !roms->read_only);
    assert(jw_storage_mount_at(&table, "/media/with space"));
    const jw_storage_mount *inner =
        jw_storage_mount_for_path(&table, "/media/sdcard1/Images/GBA/x.png");
    assert(inner == launcher);
    assert(jw_storage_mount_for_path(&table, "/media/sdcard10/x") ==
           jw_storage_mount_at(&table, "/"));
}

static void test_kernel_scan(void) {
    jw_storage_kernel_evidence ev;
    jw_storage_kernel_scan(kKernelLog, "mmcblk1", &ev);
    assert(ev.mount_seen && ev.dirty_at_boot && ev.set_read_only);
    assert(strstr(ev.message, "deleting FAT entry beyond EOF"));

    /* The dirty bit alone is informational: no error, no read-only flip. */
    jw_storage_kernel_scan(kKernelLog, "mmcblk3", &ev);
    assert(ev.mount_seen && ev.dirty_at_boot && !ev.set_read_only && !ev.message[0]);

    /* A device name that is a prefix of another must not match. */
    jw_storage_kernel_scan("FAT-fs (mmcblk10): Filesystem has been set read-only\n",
                           "mmcblk1", &ev);
    assert(!ev.mount_seen && !ev.set_read_only);

    /* The mount line was overwritten: an orphaned error is not attributable. */
    jw_storage_kernel_scan("<3>[ 22.1] FAT-fs (mmcblk1): Filesystem has been set read-only\n",
                           "mmcblk1", &ev);
    assert(!ev.mount_seen && !ev.set_read_only);

    /* A card replaced after the error starts a new lifetime. */
    char replaced[4096];
    snprintf(replaced, sizeof(replaced), "%s%s", kKernelLog,
             "<6>[  300.0] mmcblk1: mmc1:aaaa OTHER 32.0 GiB\n"
             "<4>[  301.0] FAT-fs (mmcblk1): utf8 is not a recommended IO charset for FAT filesystems\n");
    jw_storage_kernel_scan(replaced, "mmcblk1", &ev);
    assert(ev.mount_seen && !ev.set_read_only && !ev.dirty_at_boot);

    /* ext4 error form. */
    jw_storage_kernel_scan(
        "EXT4-fs (mmcblk1p1): mounted filesystem with ordered data mode\n"
        "EXT4-fs error (device mmcblk1p1): ext4_lookup:1: inode #2: comm x: deleted inode\n"
        "EXT4-fs (mmcblk1p1): Remounting filesystem read-only\n",
        "mmcblk1p1", &ev);
    assert(ev.mount_seen && ev.set_read_only && strstr(ev.message, "ext4_lookup"));
}

static void test_probe_and_monitor(void) {
    jw_storage_probe_env env;
    fixture_env(&env, kMountinfo);
    jw_storage_health h;

    jw_storage_health_probe(&env, "launcher_sd", "/media/sdcard1", NULL, &h);
    assert(h.mounted && h.access == JW_STORAGE_ACCESS_READ_ONLY);
    assert(h.cause == JW_STORAGE_CAUSE_UNKNOWN);   /* no kernel access */
    assert(strcmp(h.uuid, "22A4-0814") == 0);
    assert(strcmp(h.label, "MLPPRDLEAF") == 0);
    assert(!h.block_write_protected);
    assert(h.repair == JW_STORAGE_REPAIR_NONE);

    jw_storage_health_monitor monitor;
    memset(&monitor, 0, sizeof(monitor));
    bool newly = false;
    assert(jw_storage_health_monitor_update(&monitor, 0, &h, &newly));
    assert(newly && monitor.generation == 1);

    jw_storage_health with_kernel;
    jw_storage_health_probe(&env, "launcher_sd", "/media/sdcard1", kKernelLog, &with_kernel);
    assert(with_kernel.cause == JW_STORAGE_CAUSE_FILESYSTEM_ERROR);
    assert(with_kernel.dirty_at_boot);
    assert(jw_storage_health_monitor_update(&monitor, 0, &with_kernel, &newly));
    assert(!newly && monitor.generation == 2);

    /* The next tick has no kernel read; the explanation is retained. */
    assert(!jw_storage_health_monitor_update(&monitor, 0, &h, &newly));
    assert(monitor.slots[0].health.cause == JW_STORAGE_CAUSE_FILESYSTEM_ERROR);
    assert(monitor.generation == 2);

    /* Missing mount under an existing stub directory is never writable. */
    jw_storage_health_probe(&env, "secondary_sd", "/media/sdcard2", NULL, &h);
    assert(!h.mounted && h.access == JW_STORAGE_ACCESS_UNKNOWN);

    /* Block write protection on the ROM card only explains a read-only state. */
    jw_storage_health_probe(&env, "secondary_sd", "/mnt/sdcard", kKernelLog, &h);
    assert(h.access == JW_STORAGE_ACCESS_READ_WRITE && h.block_write_protected);
    assert(h.cause == JW_STORAGE_CAUSE_UNKNOWN && h.dirty_at_boot);

    /* Swapped roles: the same card under the other root is a new identity. */
    static const char *swapped =
        "18 1 179:6 / / rw,relatime - ext4 /dev/root rw\n"
        "33 18 179:96 / /mnt/sdcard rw - vfat /dev/mmcblk1 ro,errors=remount-ro\n"
        "34 18 179:128 / /media/sdcard1 rw - vfat /dev/mmcblk3 rw,errors=remount-ro\n";
    fixture_env(&env, swapped);
    jw_storage_health_probe(&env, "launcher_sd", "/media/sdcard1", NULL, &h);
    assert(h.access == JW_STORAGE_ACCESS_READ_WRITE);
    assert(strcmp(h.uuid, "04B1-0820") == 0);
    assert(jw_storage_health_monitor_update(&monitor, 0, &h, &newly));
    assert(!newly && monitor.slots[0].health.cause == JW_STORAGE_CAUSE_UNKNOWN);

    /* Failed mount table read: access unknown, never writable. */
    env.mountinfo_path = "/nonexistent/mountinfo";
    jw_storage_health_probe(&env, "launcher_sd", "/media/sdcard1", NULL, &h);
    assert(!h.mounted && h.access == JW_STORAGE_ACCESS_UNKNOWN);
}

static void test_repair_state(void) {
    jw_storage_probe_env env;
    fixture_env(&env, kMountinfo);
    char path[1024];
    char id[64];

    snprintf(path, sizeof(path), "%s/holds", env.repair_dir);
    mkdir_p(path);
    snprintf(path, sizeof(path), "%s/holds/22A4-0814", env.repair_dir);
    write_file(path, "state=failed\nrequest_id=r-1\n");
    assert(jw_storage_repair_hold_for_uuid(&env, "22A4-0814", id, sizeof(id)) ==
           JW_STORAGE_REPAIR_FAILED);
    assert(strcmp(id, "r-1") == 0);
    write_file(path, "state=pending\nrequest_id=$(reboot)\n");
    assert(jw_storage_repair_hold_for_uuid(&env, "22A4-0814", id, sizeof(id)) ==
           JW_STORAGE_REPAIR_PENDING);
    assert(id[0] == '\0');
    /* An unreadable or unknown state still holds. */
    write_file(path, "state=bogus\n");
    assert(jw_storage_repair_hold_for_uuid(&env, "22A4-0814", NULL, 0) ==
           JW_STORAGE_REPAIR_FAILED);
    assert(jw_storage_repair_hold_for_uuid(&env, "../etc", NULL, 0) ==
           JW_STORAGE_REPAIR_NONE);
    /* A paused-shutdown hold protects like any failed hold; the trigger only
       selects wording, and must be a plain name. */
    char trigger[32];
    write_file(path, "state=unverified\nrequest_id=r-9\ntrigger=paused-shutdown\n");
    assert(jw_storage_repair_hold_for_uuid(&env, "22A4-0814", NULL, 0) ==
           JW_STORAGE_REPAIR_FAILED);
    assert(jw_storage_repair_hold_trigger(&env, "22A4-0814", trigger, sizeof(trigger)));
    assert(strcmp(trigger, "paused-shutdown") == 0);
    write_file(path, "state=unverified\ntrigger=../x\n");
    assert(!jw_storage_repair_hold_trigger(&env, "22A4-0814", trigger, sizeof(trigger)));
    assert(trigger[0] == '\0');
    assert(!jw_storage_repair_hold_trigger(&env, "04B1-0820", trigger, sizeof(trigger)));

    jw_storage_repair_result result;
    assert(!jw_storage_repair_last_result(&env, "22A4-0814", &result));
    snprintf(path, sizeof(path), "%s/results", env.repair_dir);
    mkdir_p(path);
    snprintf(path, sizeof(path), "%s/last-result", env.repair_dir);
    write_file(path, "r-2\n");
    snprintf(path, sizeof(path), "%s/results/r-2.summary", env.repair_dir);
    write_file(path, "request_id=r-2\nuuid=22A4-0814\noutcome=repaired\nmount_state=read-write\n"
                     "mode=repair\nchanges_complete=false\nreported_changes=7\n");
    assert(jw_storage_repair_last_result(&env, "22A4-0814", &result));
    assert(strcmp(result.outcome, "repaired") == 0 && result.reported_change_count == 7);
    assert(!result.changes_complete && !result.acknowledged);
    snprintf(path, sizeof(path), "%s/acks", env.repair_dir);
    mkdir_p(path);
    snprintf(path, sizeof(path), "%s/acks/r-2", env.repair_dir);
    write_file(path, "");
    assert(jw_storage_repair_last_result(&env, "22A4-0814", &result) && result.acknowledged);
    assert(!jw_storage_repair_last_result(&env, "04B1-0820", &result));
    assert(!jw_storage_repair_last_result(&env, "../etc", &result));
    snprintf(path, sizeof(path), "%s/last-results", env.repair_dir);
    mkdir_p(path);
    snprintf(path, sizeof(path), "%s/last-results/04B1-0820", env.repair_dir);
    write_file(path, "r-4\n");
    snprintf(path, sizeof(path), "%s/results/r-4.summary", env.repair_dir);
    write_file(path, "request_id=r-4\nuuid=04B1-0820\noutcome=clean\n"
                     "mode=check\norigin=automatic-check\nmount_state=read-write\n");
    assert(jw_storage_repair_last_result(&env, "04B1-0820", &result));
    assert(strcmp(result.origin, "automatic-check") == 0);
    assert(result.trigger[0] == '\0');
    assert(strcmp(result.request_id, "r-4") == 0);
    assert(!result.acknowledged);
    write_file(path, "request_id=r-4\nuuid=04B1-0820\noutcome=clean\n"
                     "mode=check\norigin=automatic-check\ntrigger=paused-shutdown\n");
    assert(jw_storage_repair_last_result(&env, "04B1-0820", &result));
    assert(strcmp(result.trigger, "paused-shutdown") == 0);
    /* Never attach a corrupt per-card index to a different volume. */
    snprintf(path, sizeof(path), "%s/last-results/04B1-0820", env.repair_dir);
    write_file(path, "r-2\n");
    assert(!jw_storage_repair_last_result(&env, "04B1-0820", &result));
    /* A summary for a different request is not this result. */
    snprintf(path, sizeof(path), "%s/results/r-2.summary", env.repair_dir);
    write_file(path, "request_id=r-3\noutcome=repaired\n");
    assert(!jw_storage_repair_last_result(&env, "22A4-0814", &result));
}

static void test_path_check(void) {
    /* Real directories stand in for the card roots; the mount table says which
       of them are mounted and how. */
    char mountinfo[4096];
    char card1[1024], card2[1024], stub[1024], file[1024];
    snprintf(card1, sizeof(card1), "%s/cards/one", g_tmp);
    snprintf(card2, sizeof(card2), "%s/cards/two", g_tmp);
    snprintf(stub, sizeof(stub), "%s/cards/stub", g_tmp);
    mkdir_p(card1);
    mkdir_p(card2);
    mkdir_p(stub);
    char r1[1024], r2[1024], rs[1024];
    assert(realpath(card1, r1) && realpath(card2, r2) && realpath(stub, rs));
    snprintf(mountinfo, sizeof(mountinfo),
             "18 1 179:6 / / rw - ext4 /dev/root rw\n"
             "33 18 179:96 / %s rw - vfat /dev/mmcblk1 ro,errors=remount-ro\n"
             "34 18 179:128 / %s rw - vfat /dev/mmcblk3 rw,errors=remount-ro\n",
             r1, r2);
    jw_storage_probe_env env;
    fixture_env(&env, mountinfo);
    const char *roots[] = { card1, card2, stub };
    char reason[64];

    snprintf(file, sizeof(file), "%s/Images/GBA/new.png", card1);
    assert(jw_storage_path_check_env(&env, roots, 3, file, reason, sizeof(reason)) ==
           JW_STORAGE_WRITE_READ_ONLY);
    assert(strcmp(reason, "storage-read-only") == 0);

    snprintf(file, sizeof(file), "%s/Images/GBA/new.png", card2);
    assert(jw_storage_path_check_env(&env, roots, 3, file, reason, sizeof(reason)) ==
           JW_STORAGE_WRITE_OK);
    assert(reason[0] == '\0');

    /* A bare directory where a card should be mounted. */
    snprintf(file, sizeof(file), "%s/Saves/x.sav", stub);
    assert(jw_storage_path_check_env(&env, roots, 3, file, reason, sizeof(reason)) ==
           JW_STORAGE_WRITE_MISSING);

    /* A repair hold keeps a writable-looking card off limits. */
    char path[1024];
    snprintf(path, sizeof(path), "%s/holds", env.repair_dir);
    mkdir_p(path);
    snprintf(path, sizeof(path), "%s/holds/04B1-0820", env.repair_dir);
    write_file(path, "state=failed\nrequest_id=r-9\n");
    snprintf(file, sizeof(file), "%s/Images/GBA/new.png", card2);
    assert(jw_storage_path_check_env(&env, roots, 3, file, reason, sizeof(reason)) ==
           JW_STORAGE_WRITE_REPAIR_HOLD);
    unlink(path);

    /* Outside every card root, the owning mount alone decides. */
    assert(jw_storage_path_check_env(&env, roots, 3, g_tmp, reason, sizeof(reason)) ==
           JW_STORAGE_WRITE_OK);

    env.mountinfo_path = "/nonexistent/mountinfo";
    assert(jw_storage_path_check_env(&env, roots, 3, file, reason, sizeof(reason)) ==
           JW_STORAGE_WRITE_UNKNOWN);
}

static void test_unmounted_hold(void) {
    /* The second card (mmcblk3) is inserted, held, and refused by hotplug. */
    static const char *launcher_only =
        "18 1 179:6 / / rw,relatime - ext4 /dev/root rw\n"
        "34 18 179:96 / /media/sdcard1 rw - vfat /dev/mmcblk1 rw,errors=remount-ro\n";
    jw_storage_probe_env env;
    fixture_env(&env, launcher_only);
    static char dev_dir[1024];
    char path[1024];
    char holds[1024];
    snprintf(dev_dir, sizeof(dev_dir), "%s/dev", g_tmp);
    mkdir_p(dev_dir);
    env.dev_dir = dev_dir;
    unsigned char sector[512];
    memset(sector, 0, sizeof(sector));
    memcpy(sector + 82, "FAT32   ", 8);
    sector[510] = 0x55;
    sector[511] = 0xAA;
    for (int i = 0; i < 2; i++) {
        snprintf(path, sizeof(path), "%s/%s", dev_dir, i == 0 ? "mmcblk1" : "mmcblk3");
        FILE *fp = fopen(path, "wb");
        assert(fp && fwrite(sector, 1, sizeof(sector), fp) == sizeof(sector));
        fclose(fp);
    }
    snprintf(holds, sizeof(holds), "%s/holds", env.repair_dir);
    mkdir_p(holds);
    snprintf(path, sizeof(path), "%s/22A4-0814", holds);
    unlink(path);
    snprintf(path, sizeof(path), "%s/04B1-0820", holds);
    unlink(path);

    jw_storage_health h;
    jw_storage_health_probe(&env, "secondary_sd", "/mnt/sdcard", NULL, &h);
    assert(!h.mounted && !h.uuid[0]);
    assert(!jw_storage_health_probe_unmounted_hold(&env, &h) && !h.uuid[0]);

    write_file(path, "state=failed\nrequest_id=r-5\n");
    assert(jw_storage_health_probe_unmounted_hold(&env, &h));
    assert(!h.mounted && h.repair == JW_STORAGE_REPAIR_FAILED);
    assert(strcmp(h.device, "/dev/mmcblk3") == 0 && strcmp(h.uuid, "04B1-0820") == 0);
    assert(strcmp(h.fs_type, "vfat") == 0 && strcmp(h.repair_request_id, "r-5") == 0);

    /* Not a FAT or exFAT volume: no identity to offer. */
    snprintf(path, sizeof(path), "%s/mmcblk3", dev_dir);
    write_file(path, "not a boot sector");
    jw_storage_health_probe(&env, "secondary_sd", "/mnt/sdcard", NULL, &h);
    assert(!jw_storage_health_probe_unmounted_hold(&env, &h) && !h.uuid[0]);

    /* A held card that is mounted belongs to the normal probe. */
    snprintf(path, sizeof(path), "%s/04B1-0820", holds);
    unlink(path);
    snprintf(path, sizeof(path), "%s/22A4-0814", holds);
    write_file(path, "state=failed\nrequest_id=r-6\n");
    assert(!jw_storage_health_probe_unmounted_hold(&env, &h));
    unlink(path);
}

static void test_refresh_request(void) {
    assert(!jw_storage_take_refresh_request());
    jw_storage_report_write_error("/x", 28 /* ENOSPC */);
    assert(!jw_storage_take_refresh_request());
    jw_storage_report_write_error("/x", 30 /* EROFS on Linux and macOS */);
    assert(jw_storage_take_refresh_request());
    assert(!jw_storage_take_refresh_request());
}

int main(void) {
    const char *base = getenv("TMPDIR");
    snprintf(g_tmp, sizeof(g_tmp), "%s/jw-storage-health-XXXXXX",
             base && base[0] ? base : "/tmp");
    assert(mkdtemp(g_tmp));

    test_mount_table();
    test_kernel_scan();
    test_probe_and_monitor();
    test_repair_state();
    test_path_check();
    test_unmounted_hold();
    test_refresh_request();

    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_tmp);
    (void)system(cmd);
    printf("storage-health-test: ok\n");
    return 0;
}
