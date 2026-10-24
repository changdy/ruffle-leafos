#ifdef __linux__
#define _GNU_SOURCE
#else
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#endif

#include "internal/retroarch/legacy_migration.h"
#include "internal/retroarch/catalog.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/syscall.h>
#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1u << 0)
#endif
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static const char *jw__legacy_basename(const char *path) {
    const char *slash = path ? strrchr(path, '/') : NULL;
    return slash && slash[1] ? slash + 1 : (path ? path : "");
}

static bool jw__legacy_stem(const char *rom_path, char *out, size_t out_size) {
    if (!rom_path || !rom_path[0] || !out || out_size == 0 ||
        snprintf(out, out_size, "%s", jw__legacy_basename(rom_path)) >=
            (int)out_size) {
        return false;
    }
    char *dot = strrchr(out, '.');
    if (dot) {
        *dot = '\0';
    }
    return out[0] != '\0';
}

static bool jw__legacy_save_name(const char *name, const char *stem) {
    static const char *suffixes[] = {
        ".srm", ".sav", ".rtc", ".eep", ".fla", ".sra", ".mcd"
    };
    size_t stem_len = strlen(stem);
    if (strncmp(name, stem, stem_len) != 0) {
        return false;
    }
    const char *suffix = name + stem_len;
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
        if (strcmp(suffix, suffixes[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool jw__legacy_state_suffix(const char *suffix) {
    if (!suffix || strncmp(suffix, ".state", 6u) != 0) {
        return false;
    }
    const char *p = suffix + 6u;
    if (*p == '\0' || strcmp(p, ".auto") == 0 || strcmp(p, ".png") == 0 ||
        strcmp(p, ".auto.png") == 0) {
        return true;
    }
    if (!isdigit((unsigned char)*p)) {
        return false;
    }
    while (isdigit((unsigned char)*p)) {
        p++;
    }
    return *p == '\0' || strcmp(p, ".png") == 0;
}

static bool jw__legacy_state_name(const char *name, const char *stem) {
    size_t stem_len = strlen(stem);
    return strncmp(name, stem, stem_len) == 0 &&
           jw__legacy_state_suffix(name + stem_len);
}

static bool jw__legacy_dir_needs_recovery(const char *flat_dir,
                                          const char *dest_dir,
                                          const char *stem, bool saves) {
    DIR *dir = opendir(flat_dir);
    if (!dir) return false;
    bool needed = false;
    struct dirent *entry;
    while (!needed && (entry = readdir(dir)) != NULL) {
        bool match = saves ? jw__legacy_save_name(entry->d_name, stem)
                           : jw__legacy_state_name(entry->d_name, stem);
        if (!match) continue;

        char dest[PATH_MAX];
        if (snprintf(dest, sizeof(dest), "%s/%s", dest_dir, entry->d_name) >=
            (int)sizeof(dest)) {
            /* Let the full migration produce the actionable path-length error. */
            needed = true;
            continue;
        }
        if (access(dest, F_OK) != 0) {
            /* ENOENT means work remains. Other lookup failures also need the
               full migration so they are logged and surfaced to the user. */
            needed = true;
        }
    }
    closedir(dir);
    return needed;
}

bool jw_ra_legacy_flat_files_need_recovery(const char *source_root,
                                           const char *rom_path,
                                           const char *core_config_folder) {
    char stem[512];
    char saves[PATH_MAX], states[PATH_MAX];
    char saves_dest[PATH_MAX], states_dest[PATH_MAX];
    if (!source_root || !source_root[0] ||
        !jw_ra_core_folder_is_safe(core_config_folder) ||
        !jw__legacy_stem(rom_path, stem, sizeof(stem)) ||
        snprintf(saves, sizeof(saves), "%s/Saves", source_root) >=
            (int)sizeof(saves) ||
        snprintf(states, sizeof(states), "%s/States", source_root) >=
            (int)sizeof(states) ||
        snprintf(saves_dest, sizeof(saves_dest), "%s/%s", saves,
                 core_config_folder) >= (int)sizeof(saves_dest) ||
        snprintf(states_dest, sizeof(states_dest), "%s/%s", states,
                 core_config_folder) >= (int)sizeof(states_dest)) {
        return false;
    }
    return jw__legacy_dir_needs_recovery(saves, saves_dest, stem, true) ||
           jw__legacy_dir_needs_recovery(states, states_dest, stem, false);
}

static int jw__legacy_mkdir(const char *path) {
    if (mkdir(path, 0755) == 0 || errno == EEXIST) {
        struct stat st;
        return stat(path, &st) == 0 && S_ISDIR(st.st_mode) ? 0 : -1;
    }
    return -1;
}

static int jw__legacy_install_no_replace(const char *tmp, const char *dest) {
#ifdef __linux__
#ifdef SYS_renameat2
    if (syscall(SYS_renameat2, AT_FDCWD, tmp, AT_FDCWD, dest,
                RENAME_NOREPLACE) == 0) {
        return 0;
    }
    if (errno == EEXIST) {
        return 1;
    }
    return -1;
#else
    (void)tmp;
    (void)dest;
    errno = ENOTSUP;
    return -1;
#endif
#else
    /* Portable fallback for native tests. Jawaka is the sole pre-launch writer;
       Linux device builds use renameat2 above for atomic no-replace semantics. */
    if (access(dest, F_OK) == 0) {
        return 1;
    }
    return rename(tmp, dest) == 0 ? 0 : -1;
#endif
}

static void jw__legacy_reason(char *out, size_t out_size,
                              const char *operation, int error_number) {
    if (out && out_size > 0) {
        snprintf(out, out_size, "%s: %s", operation,
                 strerror(error_number ? error_number : EIO));
    }
}

static int jw__legacy_copy_one(const char *src, const char *dest,
                               unsigned sequence,
                               char *reason, size_t reason_size) {
    if (reason && reason_size > 0) reason[0] = '\0';
    struct stat st;
    if (stat(src, &st) != 0) {
        jw__legacy_reason(reason, reason_size, "stat source", errno);
        return -1;
    }
    if (!S_ISREG(st.st_mode)) {
        snprintf(reason, reason_size, "source is not a regular file");
        return -1;
    }
    if (access(dest, F_OK) == 0) {
        return 1;
    }
    if (errno != ENOENT) {
        jw__legacy_reason(reason, reason_size, "check destination", errno);
        return -1;
    }

    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof(tmp), "%s.jawaka-migrate-%ld-%u.tmp",
                 dest, (long)getpid(), sequence) >= (int)sizeof(tmp)) {
        snprintf(reason, reason_size, "temporary path too long");
        return -1;
    }
    int in_fd = open(src, O_RDONLY);
    if (in_fd < 0) {
        jw__legacy_reason(reason, reason_size, "open source", errno);
        return -1;
    }
    int out_fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (out_fd < 0) {
        jw__legacy_reason(reason, reason_size, "create temporary file", errno);
        close(in_fd);
        return -1;
    }

    int rc = 0;
    char buffer[64u * 1024u];
    for (;;) {
        ssize_t n = read(in_fd, buffer, sizeof(buffer));
        if (n == 0) {
            break;
        }
        if (n < 0) {
            if (errno == EINTR) continue;
            jw__legacy_reason(reason, reason_size, "read source", errno);
            rc = -1;
            break;
        }
        ssize_t off = 0;
        while (off < n) {
            ssize_t wrote = write(out_fd, buffer + off, (size_t)(n - off));
            if (wrote < 0 && errno == EINTR) continue;
            if (wrote <= 0) {
                jw__legacy_reason(reason, reason_size, "write temporary file",
                                  wrote < 0 ? errno : EIO);
                rc = -1;
                break;
            }
            off += wrote;
        }
        if (rc != 0) break;
    }
    if (rc == 0 && fsync(out_fd) != 0) {
        jw__legacy_reason(reason, reason_size, "sync temporary file", errno);
        rc = -1;
    }
    (void)fchmod(out_fd, st.st_mode & 0777);
#ifdef __linux__
    struct timespec times[2] = { st.st_atim, st.st_mtim };
#else
    struct timespec times[2] = { st.st_atimespec, st.st_mtimespec };
#endif
    if (rc == 0 && futimens(out_fd, times) != 0) {
        jw__legacy_reason(reason, reason_size, "preserve timestamps", errno);
        rc = -1;
    }
    if (close(out_fd) != 0 && rc == 0) {
        jw__legacy_reason(reason, reason_size, "close temporary file", errno);
        rc = -1;
    }
    close(in_fd);
    if (rc == 0) {
        rc = jw__legacy_install_no_replace(tmp, dest);
        if (rc < 0) {
            jw__legacy_reason(reason, reason_size,
                              "install destination without overwrite", errno);
        }
    }
    (void)unlink(tmp);
    return rc;
}

static void jw__legacy_scan_dir(const char *flat_dir, const char *dest_dir,
                                const char *stem, bool saves,
                                jw_ra_legacy_migration_report *report,
                                unsigned *sequence) {
    DIR *dir = opendir(flat_dir);
    if (!dir) {
        if (errno != ENOENT) {
            if (!report->detail[0]) {
                int open_error = errno;
                snprintf(report->detail, sizeof(report->detail),
                         "open directory %.700s: %s", flat_dir,
                         strerror(open_error));
            }
            report->failed++;
        }
        return;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        bool match = saves ? jw__legacy_save_name(entry->d_name, stem)
                           : jw__legacy_state_name(entry->d_name, stem);
        if (!match) continue;

        char src[PATH_MAX];
        char dest[PATH_MAX];
        if (snprintf(src, sizeof(src), "%s/%s", flat_dir, entry->d_name) >=
                (int)sizeof(src) ||
            snprintf(dest, sizeof(dest), "%s/%s", dest_dir, entry->d_name) >=
                (int)sizeof(dest)) {
            if (!report->detail[0]) {
                snprintf(report->detail, sizeof(report->detail),
                         "path too long for legacy file %s", entry->d_name);
            }
            report->failed++;
            continue;
        }
        char reason[192];
        int rc = jw__legacy_copy_one(src, dest, (*sequence)++, reason,
                                     sizeof(reason));
        if (rc == 0) report->copied++;
        else if (rc == 1) report->existing++;
        else {
            if (!report->detail[0]) {
                const char *src_tail = strlen(src) > 360u
                    ? src + strlen(src) - 360u : src;
                const char *dest_tail = strlen(dest) > 360u
                    ? dest + strlen(dest) - 360u : dest;
                snprintf(report->detail, sizeof(report->detail),
                         "source=%.360s destination=%.360s reason=%.190s",
                         src_tail, dest_tail,
                         reason[0] ? reason : "copy failed");
            }
            report->failed++;
        }
    }
    closedir(dir);
}

jw_ra_legacy_migration_result jw_ra_migrate_legacy_flat_files(
    const char *source_root, const char *rom_path,
    const char *effective_core_id, const char *core_config_folder,
    const char *legacy_flat_core, bool ambiguous,
    jw_ra_legacy_migration_report *out) {
    jw_ra_legacy_migration_report local;
    memset(&local, 0, sizeof(local));
    if (!out) out = &local;
    else memset(out, 0, sizeof(*out));

    if (!source_root || !source_root[0] || !rom_path || !rom_path[0] ||
        !effective_core_id || !effective_core_id[0] ||
        !legacy_flat_core || !legacy_flat_core[0] ||
        strcmp(effective_core_id, legacy_flat_core) != 0 ||
        !jw_ra_core_folder_is_safe(core_config_folder)) {
        return JW_RA_LEGACY_MIGRATION_NOT_APPLICABLE;
    }
    if (!jw_ra_legacy_flat_files_need_recovery(source_root, rom_path,
                                               core_config_folder)) {
        return JW_RA_LEGACY_MIGRATION_NO_FILES;
    }
    if (ambiguous) {
        snprintf(out->detail, sizeof(out->detail),
                 "same ROM stem has multiple legacy core owners");
        return JW_RA_LEGACY_MIGRATION_AMBIGUOUS;
    }

    char stem[512];
    char saves[PATH_MAX], states[PATH_MAX];
    char saves_dest[PATH_MAX], states_dest[PATH_MAX];
    if (!jw__legacy_stem(rom_path, stem, sizeof(stem)) ||
        snprintf(saves, sizeof(saves), "%s/Saves", source_root) >= (int)sizeof(saves) ||
        snprintf(states, sizeof(states), "%s/States", source_root) >= (int)sizeof(states) ||
        snprintf(saves_dest, sizeof(saves_dest), "%s/%s", saves,
                 core_config_folder) >= (int)sizeof(saves_dest) ||
        snprintf(states_dest, sizeof(states_dest), "%s/%s", states,
                 core_config_folder) >= (int)sizeof(states_dest)) {
        snprintf(out->detail, sizeof(out->detail), "legacy recovery path too long");
        return JW_RA_LEGACY_MIGRATION_FAILED;
    }

    const char *dirs[] = { saves, states, saves_dest, states_dest };
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        if (jw__legacy_mkdir(dirs[i]) != 0) {
            int mkdir_error = errno;
            snprintf(out->detail, sizeof(out->detail),
                     "create directory %.700s: %s", dirs[i],
                     strerror(mkdir_error ? mkdir_error : EIO));
            out->failed = 1;
            return JW_RA_LEGACY_MIGRATION_FAILED;
        }
    }

    unsigned sequence = 0;
    jw__legacy_scan_dir(saves, saves_dest, stem, true, out, &sequence);
    jw__legacy_scan_dir(states, states_dest, stem, false, out, &sequence);
    if (out->failed > 0) {
        char first_failure[sizeof(out->detail)];
        snprintf(first_failure, sizeof(first_failure), "%s", out->detail);
        snprintf(out->detail, sizeof(out->detail),
                 "copied=%d existing=%d failed=%d first=%.900s",
                 out->copied, out->existing, out->failed,
                 first_failure[0] ? first_failure : "unknown");
        return JW_RA_LEGACY_MIGRATION_FAILED;
    }
    if (out->copied > 0) {
        snprintf(out->detail, sizeof(out->detail), "copied=%d existing=%d",
                 out->copied, out->existing);
        return JW_RA_LEGACY_MIGRATION_COPIED;
    }
    return JW_RA_LEGACY_MIGRATION_NO_FILES;
}
