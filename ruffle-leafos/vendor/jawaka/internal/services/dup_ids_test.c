#include "internal/services/dup_ids.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void jw__test_empty_and_singleton(void) {
    assert(!jw_svc_find_duplicate_ids(NULL, 0, NULL));

    const char *ignored[] = {"org.umrk.a"};
    bool untouched[] = {true};
    assert(!jw_svc_find_duplicate_ids(ignored, 0, untouched));
    assert(untouched[0]);

    const char *one[] = {"org.umrk.a"};
    bool out1[1];
    assert(!jw_svc_find_duplicate_ids(one, 1, out1));
    assert(!out1[0]);

    puts("PASS dup-ids-test empty and singleton inputs report no duplicates");
}

static void jw__test_all_unique(void) {
    const char *ids[] = {"org.umrk.a", "org.umrk.b", "org.umrk.c"};
    bool out[3];
    assert(!jw_svc_find_duplicate_ids(ids, 3, out));
    assert(!out[0] && !out[1] && !out[2]);

    puts("PASS dup-ids-test all-unique ids report no duplicates");
}

static void jw__test_one_colliding_pair(void) {
    const char *ids[] = {"org.umrk.a", "org.umrk.b", "org.umrk.a"};
    bool out[3];
    assert(jw_svc_find_duplicate_ids(ids, 3, out));
    assert(out[0] && !out[1] && out[2]);

    puts("PASS dup-ids-test flags exactly the colliding pair, leaves the rest clear");
}

static void jw__test_triple_collision(void) {
    const char *ids[] = {
        "org.umrk.a", "org.umrk.unique1", "org.umrk.a",
        "org.umrk.unique2", "org.umrk.a",
    };
    bool out[5];
    assert(jw_svc_find_duplicate_ids(ids, 5, out));
    assert(out[0] && !out[1] && out[2] && !out[3] && out[4]);

    puts("PASS dup-ids-test flags every interleaved member of a 3-way collision");
}

static void jw__test_multiple_independent_collisions(void) {
    const char *ids[] = {"org.umrk.a", "org.umrk.b", "org.umrk.a", "org.umrk.c", "org.umrk.b"};
    bool out[5];
    assert(jw_svc_find_duplicate_ids(ids, 5, out));
    assert(out[0] && out[1] && out[2] && !out[3] && out[4]);

    puts("PASS dup-ids-test flags two independent colliding groups without cross-contamination");
}

static void jw__test_case_sensitive(void) {
    const char *ids[] = {"org.umrk.Foo", "org.umrk.foo"};
    bool out[2];
    assert(!jw_svc_find_duplicate_ids(ids, 2, out));
    assert(!out[0] && !out[1]);

    puts("PASS dup-ids-test comparison is case-sensitive, not a duplicate across case");
}

static void jw__test_degenerate_strings(void) {
    const char *empty_ids[] = {"", "org.umrk.a", ""};
    bool empty_out[3];
    assert(jw_svc_find_duplicate_ids(empty_ids, 3, empty_out));
    assert(empty_out[0] && !empty_out[1] && empty_out[2]);

    char long_id_a[4097];
    char long_id_b[4097];
    char long_id_unique[4097];
    memset(long_id_a, 'a', sizeof(long_id_a) - 1);
    long_id_a[sizeof(long_id_a) - 1] = '\0';
    memcpy(long_id_b, long_id_a, sizeof(long_id_a));
    memcpy(long_id_unique, long_id_a, sizeof(long_id_a));
    long_id_unique[sizeof(long_id_unique) - 2] = 'b';

    const char *long_ids[] = {long_id_a, long_id_unique, long_id_b};
    bool long_out[3];
    assert(jw_svc_find_duplicate_ids(long_ids, 3, long_out));
    assert(long_out[0] && !long_out[1] && long_out[2]);

    puts("PASS dup-ids-test empty and long byte-identical strings collide");
}

static void jw__test_null_entries_never_match(void) {
    const char *ids[] = {NULL, "org.umrk.a", NULL, "org.umrk.a"};
    bool out[4];
    assert(jw_svc_find_duplicate_ids(ids, 4, out));
    assert(!out[0] && out[1] && !out[2] && out[3]);

    const char *all_null[] = {NULL, NULL, NULL};
    bool out2[3];
    assert(!jw_svc_find_duplicate_ids(all_null, 3, out2));
    assert(!out2[0] && !out2[1] && !out2[2]);

    puts("PASS dup-ids-test NULL entries never match anything, including each other");
}

static void jw__test_null_buffers_with_nonzero_count(void) {
    const char *ids[] = {"org.umrk.a", "org.umrk.a"};
    bool out[2] = {true, false};
    assert(!jw_svc_find_duplicate_ids(NULL, 2, out));
    assert(out[0] && !out[1]);
    assert(!jw_svc_find_duplicate_ids(ids, 2, NULL));

    puts("PASS dup-ids-test NULL outer buffers return without writing");
}

int main(void) {
    jw__test_empty_and_singleton();
    jw__test_all_unique();
    jw__test_one_colliding_pair();
    jw__test_triple_collision();
    jw__test_multiple_independent_collisions();
    jw__test_case_sensitive();
    jw__test_degenerate_strings();
    jw__test_null_entries_never_match();
    jw__test_null_buffers_with_nonzero_count();
    puts("PASS dup-ids-test");
    return 0;
}
