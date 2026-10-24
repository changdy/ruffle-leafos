#include "internal/launcher/pico8.h"
#include "cmd/jawaka-osd/game_launch.h"
#include "internal/db/db.h"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static void script(const char *path, const char *body) {
    FILE *f = fopen(path, "w");
    assert(f);
    fprintf(f, "#!/bin/sh\n%s\n", body);
    fclose(f);
    assert(chmod(path, 0700) == 0);
}

static int scalar(sqlite3 *db, const char *sql) {
    sqlite3_stmt *st=NULL;
    assert(sqlite3_prepare_v2(db,sql,-1,&st,NULL)==SQLITE_OK);
    assert(sqlite3_step(st)==SQLITE_ROW);
    int value=sqlite3_column_int(st,0); sqlite3_finalize(st); return value;
}
static int count_commit(void *p) { (*(int *)p)++; return 0; }
static void library_test(void) {
    sqlite3 *db=NULL;
    assert(sqlite3_open(":memory:",&db)==SQLITE_OK);
    assert(jw_db_apply_schema(db)==0);
    assert(jw_db_scan_begin(db)==0);
    assert(jw_db_insert_game(db,"PICO8","cart","Roms/PICO8/Splore/cart.p8.png",NULL)==0);
    assert(jw_db_insert_game(db,"PICO8","other","Roms/PICO8/other.p8",NULL)==0);
    char report[]="/tmp/jw-pico8-library-XXXXXX";
    int fd=mkstemp(report); assert(fd>=0);
    FILE *fp=fdopen(fd,"w"); assert(fp);
    fputs("cart\tA readable title\n",fp); fclose(fp);
    assert(jw_pico8_apply_library(db,report)==0);
    assert(scalar(db,"SELECT COUNT(*) FROM game_settings WHERE key='core_id' AND value='pico8_native'")==1);
    assert(scalar(db,"SELECT COUNT(*) FROM game_settings WHERE key='imported_display_name' AND value='A readable title'")==1);
    assert(sqlite3_exec(db,"UPDATE game_settings SET value='fake08' WHERE key='core_id';"
        "UPDATE games SET playtime_s=42 WHERE name='cart'; INSERT INTO favorites SELECT 'game',id,1 FROM games WHERE name='cart';",NULL,NULL,NULL)==SQLITE_OK);
    assert(jw_pico8_apply_library(db,report)==0);
    assert(scalar(db,"SELECT COUNT(*) FROM game_settings WHERE key='core_id' AND value='fake08'")==1);
    assert(scalar(db,"SELECT COUNT(*) FROM games JOIN favorites ON target_id=games.id WHERE playtime_s=42")==1);
    /* Choosing Default removes the override; repeated imports must not reseed. */
    assert(sqlite3_exec(db,"DELETE FROM game_settings WHERE key='core_id'",NULL,NULL,NULL)==SQLITE_OK);
    assert(jw_pico8_apply_library(db,report)==0);
    assert(scalar(db,"SELECT COUNT(*) FROM game_settings WHERE key='core_id'")==0);
    fp=fopen(report,"w"); assert(fp); fputs("../other\tInvalid\n",fp); fclose(fp);
    assert(jw_pico8_apply_library(db,report)!=0);
    /* Many carts still use only two commits, and a malformed trailing row
       cannot apply a valid prefix. */
    fp=fopen(report,"w"); assert(fp);
    for (int i=0; i<100; i++) {
        char path[160]; snprintf(path,sizeof(path),"Roms/PICO8/Splore/batch_%d.p8.png",i);
        assert(jw_db_insert_game(db,"PICO8","batch",path,NULL)==0);
        fprintf(fp,"batch_%d\tBatch title %d\n",i,i);
    }
    fputs("../bad\tInvalid\n",fp); fclose(fp);
    int commits=0; sqlite3_commit_hook(db,count_commit,&commits);
    assert(jw_pico8_apply_library(db,report)!=0 && commits==0);
    assert(scalar(db,"SELECT COUNT(*) FROM game_settings WHERE key='core_id'")==0);
    fp=fopen(report,"w"); assert(fp);
    for (int i=0; i<100; i++) fprintf(fp,"batch_%d\tBatch title %d\n",i,i);
    fclose(fp);
    /* Fail midway through seeding: all core choices and markers roll back. */
    assert(sqlite3_exec(db,"CREATE TRIGGER fail_seed BEFORE INSERT ON game_settings "
        "WHEN NEW.key='pico8_imported' AND NEW.game_id=(SELECT id FROM games WHERE rom_path='Roms/PICO8/Splore/batch_50.p8.png') "
        "BEGIN SELECT RAISE(ABORT,'fixture'); END",NULL,NULL,NULL)==SQLITE_OK);
    assert(jw_pico8_apply_library(db,report)!=0);
    assert(scalar(db,"SELECT COUNT(*) FROM game_settings WHERE key='core_id'")==0);
    assert(scalar(db,"SELECT COUNT(*) FROM game_settings WHERE key='pico8_imported'")==1);
    assert(sqlite3_exec(db,"DROP TRIGGER fail_seed",NULL,NULL,NULL)==SQLITE_OK);
    commits=0;
    assert(jw_pico8_apply_library(db,report)==0 && commits==2);
    assert(scalar(db,"SELECT COUNT(*) FROM game_settings WHERE key='core_id'")==100);
    commits=0;
    assert(jw_pico8_apply_library(db,report)==0 && commits==2);
    unlink(report); sqlite3_close(db);
}

int main(void) {
    library_test();
    assert(JW_PICO8_EXIT_CONFIRM_MS == JW_OSD_GAME_TRANSIENT_MS);
    long long requested = 0;
    assert(jw_pico8_exit_signal(&requested, 1000) == SIGTERM);
    assert(jw_pico8_exit_signal(&requested, 1100) == 0);
    assert(jw_pico8_exit_signal(&requested, 2999) == 0);
    assert(requested == 1000);
    assert(jw_pico8_exit_signal(&requested, 3000) == SIGKILL);
    assert(jw_pico8_exit_signal(&requested, 4000) == SIGKILL);
    requested = 0; /* Next session starts with a normal quit again. */
    assert(jw_pico8_exit_signal(&requested, 5000) == SIGTERM);
    long long deadline = 0;
    assert(!jw_pico8_exit_confirmed(&deadline, 1000));
    assert(jw_pico8_exit_confirmed(&deadline, 1100));
    assert(deadline == 0);
    assert(!jw_pico8_exit_confirmed(&deadline, 1200));
    assert(!jw_pico8_exit_confirmed(&deadline, 5200)); /* Expired, even at boundary. */
    assert(jw_pico8_exit_confirmed(&deadline, 5300));
    assert(jw_pico8_core_matches("pico8_native", "mlp1/PICO8.pak", "launch-cart.sh"));
    assert(!jw_pico8_core_matches("pico8_native", "shared/PICO8.pak", "launch-cart.sh"));
    assert(!jw_pico8_core_matches("other", "mlp1/PICO8.pak", "launch-cart.sh"));
    assert(!jw_pico8_core_matches("pico8_native", "mlp1/PICO8.pak", "other.sh"));
    assert(!jw_pico8_core_matches(NULL, NULL, NULL));
    jw_storage_source_list sources = { .count = 2 };
    jw_storage_source *a = &sources.sources[0], *b = &sources.sources[1];
    a->primary = true;
    a->available = b->available = true;
    strcpy(a->root, "/card two");
    strcpy(a->bios_path, "/card two/BIOS");
    strcpy(a->userdata_path, "/card two/Private data");
    strcpy(a->roms_path, "/card two/Roms");
    strcpy(b->roms_path, "/card one/Roms");
    unsetenv("RECORDINGS_PATH");
    jw_pico8_paths paths;
    assert(jw_pico8_resolve_paths(&sources, b, &paths));
    assert(strcmp(paths.runtime, "/card two/BIOS/PICO8") == 0);
    assert(strcmp(paths.home, "/card two/Private data/pico8") == 0);
    assert(strcmp(paths.root, "/card one/Roms/PICO8") == 0);
    assert(strcmp(paths.desktop, "/card two/Recordings/PICO8") == 0);
    assert(jw_pico8_resolve_paths(&sources, NULL, &paths));
    assert(strcmp(paths.root, "/card two/Roms/PICO8") == 0);
    setenv("RECORDINGS_PATH", "/card two/Captures", 1);
    assert(jw_pico8_resolve_paths(&sources, NULL, &paths));
    assert(strcmp(paths.desktop, "/card two/Captures/PICO8") == 0);
    a->available = false;
    assert(!jw_pico8_resolve_paths(&sources, NULL, &paths));
    a->available = true;
    b->available = false;
    assert(!jw_pico8_resolve_paths(&sources, b, &paths));
    assert(jw_pico8_resolve_paths(&sources, NULL, &paths));
    jw_pico8_export(&paths, true);
    assert(strcmp(getenv("UMRK_PICO8_SESSION"), "1") == 0);
    jw_pico8_export(NULL, false);
    assert(!getenv("UMRK_PICO8_SESSION") && !getenv("UMRK_PICO8_HOME_PATH"));
    char dir[] = "/tmp/jw-pico8-XXXXXX";
    assert(mkdtemp(dir));
    strcpy(a->apps_path, dir);
    char platform_dir[4096], pak_dir[4096], resolved[4096];
    snprintf(platform_dir, sizeof(platform_dir), "%s/mlp1", dir);
    snprintf(pak_dir, sizeof(pak_dir), "%s/mlp1/PICO8.pak", dir);
    assert(mkdir(platform_dir, 0700) == 0 && mkdir(pak_dir, 0700) == 0);
    assert(realpath(pak_dir, resolved));
    assert(jw_pico8_app_matches(a, resolved));
    assert(!jw_pico8_app_matches(b, resolved));
    assert(!jw_pico8_app_matches(a, platform_dir));
    rmdir(pak_dir);
    rmdir(platform_dir);
    char wrapper[4096];
    snprintf(wrapper, sizeof(wrapper), "%s/check.sh", dir);
    script(wrapper, "[ \"$1\" = --check ] && [ -z \"${UMRK_PICO8_SESSION:-}\" ] && [ \"$UMRK_PICO8_HOME_PATH\" = '/card two/Private data/pico8' ]");
    assert(jw_pico8_preflight(wrapper, &paths));
    assert(!getenv("UMRK_PICO8_HOME_PATH"));
    script(wrapper, "exit 7");
    assert(!jw_pico8_preflight(wrapper, &paths));
    script(wrapper, "sleep 30 &\nwait");
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    assert(!jw_pico8_preflight(wrapper, &paths));
    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed = end.tv_sec - start.tv_sec + (end.tv_nsec - start.tv_nsec) / 1e9;
    assert(elapsed >= 1.9 && elapsed < 4.0);
    unlink(wrapper);
    assert(!jw_pico8_preflight(wrapper, &paths));
    rmdir(dir);
    puts("PASS: PICO-8 identity, primary paths, child exports, bounded preflight, import titles and core preference preservation");
    return 0;
}
