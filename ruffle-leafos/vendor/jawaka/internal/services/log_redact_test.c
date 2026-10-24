#include "internal/services/log_redact.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void jw__test_line_with_no_secret_passes_through(void) {
    char out[128];
    bool redacted = jw_svc_log_redact_line("level=info msg=hello world", out, sizeof(out));
    assert(!redacted);
    assert(strcmp(out, "level=info msg=hello world") == 0);

    puts("PASS log-redact-test a line with no recognized secret passes through unchanged");
}

static void jw__test_logfmt_key_value_is_redacted(void) {
    char out[128];
    bool redacted = jw_svc_log_redact_line("level=info token=abc123 msg=ok", out, sizeof(out));
    assert(redacted);
    assert(strcmp(out, "level=info token=[REDACTED] msg=ok") == 0);

    puts("PASS log-redact-test a logfmt-style token= pair is redacted, siblings are untouched");
}

static void jw__test_header_style_redacts_to_end_of_line(void) {
    char out[128];
    bool redacted = jw_svc_log_redact_line("Authorization: Bearer xyz.abc.def", out, sizeof(out));
    assert(redacted);
    assert(strcmp(out, "Authorization: [REDACTED]") == 0);

    puts("PASS log-redact-test a header-style Authorization value is redacted to end of line");
}

static void jw__test_cookie_header_hides_embedded_pairs(void) {
    char out[128];
    bool redacted = jw_svc_log_redact_line("Cookie: session=abc123; other=def456", out, sizeof(out));
    assert(redacted);
    assert(strcmp(out, "Cookie: [REDACTED]") == 0);

    puts("PASS log-redact-test a Cookie header redacts its embedded key=value pairs too");
}

static void jw__test_quoted_value_is_redacted_whole(void) {
    char out[128];
    bool redacted = jw_svc_log_redact_line("password=\"hello world\" next=1", out, sizeof(out));
    assert(redacted);
    assert(strcmp(out, "password=[REDACTED] next=1") == 0);

    puts("PASS log-redact-test a quoted value with embedded spaces is redacted as one unit");
}

static void jw__test_escaped_quote_does_not_end_value(void) {
    char out[128];
    bool redacted = jw_svc_log_redact_line(
        "token=\"abc\\\" = still-secret\" next=1", out, sizeof(out));
    assert(redacted);
    assert(strcmp(out, "token=[REDACTED] next=1") == 0);

    puts("PASS log-redact-test an escaped quote does not expose the rest of a quoted secret");
}

static void jw__test_unterminated_quote_redacts_to_end_of_line(void) {
    char out[128];
    bool redacted = jw_svc_log_redact_line("secret=\"never closes", out, sizeof(out));
    assert(redacted);
    assert(strcmp(out, "secret=[REDACTED]") == 0);

    puts("PASS log-redact-test an unterminated quoted value redacts to end of line");
}

static void jw__test_key_match_is_case_insensitive(void) {
    char out[128];
    bool redacted = jw_svc_log_redact_line("PASSWORD=hunter2", out, sizeof(out));
    assert(redacted);
    assert(strcmp(out, "PASSWORD=[REDACTED]") == 0);

    puts("PASS log-redact-test the sensitive-key match is case-insensitive");
}

static void jw__test_key_spelling_variants_and_sensitive_suffixes(void) {
    char out[256];
    bool redacted = jw_svc_log_redact_line(
        "authToken=aaa refresh-token=bbb client_secret=ccc "
        "five_game_pin_hash=0123456789abcdef",
        out, sizeof(out));
    assert(redacted);
    assert(strcmp(out,
                  "authToken=[REDACTED] refresh-token=[REDACTED] "
                  "client_secret=[REDACTED] "
                  "five_game_pin_hash=[REDACTED]") == 0);

    assert(jw_svc_log_redact_line("AUTHTOKEN=aaa XAPIKEY=bbb", out,
                                  sizeof(out)));
    assert(strcmp(out, "AUTHTOKEN=[REDACTED] XAPIKEY=[REDACTED]") == 0);

    assert(jw_svc_log_redact_line("API key: abc", out, sizeof(out)));
    assert(strcmp(out, "API key: [REDACTED]") == 0);

    assert(jw_svc_log_redact_line(
        "this_is_a_deliberately_long_service_namespace_that_exceeds_"
        "sixty_four_chars_access_token=abc",
        out, sizeof(out)));
    assert(strcmp(
               out,
               "this_is_a_deliberately_long_service_namespace_that_exceeds_"
               "sixty_four_chars_access_token=[REDACTED]") == 0);

    puts("PASS log-redact-test camelCase, separator, suffix, and compact key variants redact");
}

static void jw__test_quoted_and_whitespace_separated_key(void) {
    char out[128];
    bool redacted = jw_svc_log_redact_line(
        "{\"API key\" \t: \t\"abc\", \"message\":\"ok\"}", out,
        sizeof(out));
    assert(redacted);
    assert(strcmp(out, "{\"API key\" \t: \t[REDACTED]") == 0);

    assert(jw_svc_log_redact_line("token \t= \t'abc def' next=1", out,
                                  sizeof(out)));
    assert(strcmp(out, "token \t= \t[REDACTED] next=1") == 0);

    puts("PASS log-redact-test quoted keys and delimiter whitespace are handled and preserved");
}

static void jw__test_similar_non_secret_keys_pass_through(void) {
    const char *line =
        "auth=enabled cookie_count=3 password_policy=required "
        "pin_count=4 secret_status=unset";
    char out[160];
    bool redacted = jw_svc_log_redact_line(line, out, sizeof(out));
    assert(!redacted);
    assert(strcmp(out, line) == 0);

    assert(!jw_svc_log_redact_line(
        "2026-07-28 12:34:56 INFO url=https://example.test:8443/path",
        out, sizeof(out)));
    assert(strcmp(out,
                  "2026-07-28 12:34:56 INFO "
                  "url=https://example.test:8443/path") == 0);

    assert(!jw_svc_log_redact_line(
        "controller pin: /dev/input/event0 already selects device=2", out,
        sizeof(out)));
    assert(strcmp(out,
                  "controller pin: /dev/input/event0 already selects "
                  "device=2") == 0);

    puts("PASS log-redact-test ambiguous auth, similar keys, URLs, and controller pins pass through");
}

static void jw__test_auth_credentials_and_schemes_are_redacted(void) {
    char out[160];
    bool redacted = jw_svc_log_redact_line(
        "auth=opaque-credential next=1", out, sizeof(out));
    assert(redacted);
    assert(strcmp(out, "auth=[REDACTED] next=1") == 0);

    redacted = jw_svc_log_redact_line(
        "authorization=Bearer actual-token next=1", out, sizeof(out));
    assert(redacted);
    assert(strcmp(out, "authorization=[REDACTED] next=1") == 0);

    redacted = jw_svc_log_redact_line(
        "proxy_authorization=Basic dXNlcjpwYXNz result=ok", out,
        sizeof(out));
    assert(redacted);
    assert(strcmp(out, "proxy_authorization=[REDACTED] result=ok") == 0);

    puts("PASS log-redact-test auth credentials include unquoted Bearer and Basic payloads");
}

static void jw__test_pin_and_secret_and_apikey(void) {
    char out[128];
    assert(jw_svc_log_redact_line("pin=4321", out, sizeof(out)));
    assert(strcmp(out, "pin=[REDACTED]") == 0);

    assert(jw_svc_log_redact_line("secret=topsecret", out, sizeof(out)));
    assert(strcmp(out, "secret=[REDACTED]") == 0);

    assert(jw_svc_log_redact_line("api_key=sk_live_abcdef", out, sizeof(out)));
    assert(strcmp(out, "api_key=[REDACTED]") == 0);

    puts("PASS log-redact-test pin, secret, and api_key keys are all recognized");
}

static void jw__test_multiple_sensitive_pairs_all_redacted(void) {
    char out[128];
    bool redacted = jw_svc_log_redact_line("token=aaa user=bob pwd=bbb", out, sizeof(out));
    assert(redacted);
    assert(strcmp(out, "token=[REDACTED] user=bob pwd=[REDACTED]") == 0);

    puts("PASS log-redact-test multiple sensitive pairs on one line are all redacted");
}

static void jw__test_crypt_hash_marker_redacts_whole_line(void) {
    char out[128];
    bool redacted = jw_svc_log_redact_line(
        "user root hash $6$abcxyz$therealhashvaluehere==", out, sizeof(out));
    assert(redacted);
    assert(strcmp(out, "[REDACTED LINE]") == 0);

    puts("PASS log-redact-test a crypt hash marker redacts the entire line");
}

static void jw__test_shadow_entry_redacts_whole_line(void) {
    char out[128];
    bool redacted = jw_svc_log_redact_line(
        "root:$6$abcd$longhashvalue:19000:0:99999:7:::", out, sizeof(out));
    assert(redacted);
    assert(strcmp(out, "[REDACTED LINE]") == 0);

    puts("PASS log-redact-test a shadow-file-shaped entry redacts via its crypt hash marker");
}

static void jw__test_locked_shadow_and_modern_hash_redact_whole_line(void) {
    char out[128];
    assert(jw_svc_log_redact_line(
        "daemon:*:19000:0:99999:7:::", out, sizeof(out)));
    assert(strcmp(out, "[REDACTED LINE]") == 0);

    assert(jw_svc_log_redact_line(
        "password hash is $y$j9T$saltsalt$hashhash", out, sizeof(out)));
    assert(strcmp(out, "[REDACTED LINE]") == 0);

    puts("PASS log-redact-test locked shadow entries and modern hash prefixes redact");
}

static void jw__test_private_key_marker_redacts_whole_line(void) {
    char out[128];
    bool redacted = jw_svc_log_redact_line(
        "-----BEGIN RSA PRIVATE KEY-----", out, sizeof(out));
    assert(redacted);
    assert(strcmp(out, "[REDACTED LINE]") == 0);

    puts("PASS log-redact-test a PEM private-key marker line redacts the entire line");
}

static void jw__test_truncation_still_reports_redacted(void) {
    char out[8];
    /* The secret's placeholder cannot possibly fit in 8 bytes, but the
     * boolean must still reflect that the full input line contained
     * one. */
    bool redacted = jw_svc_log_redact_line("token=abcdefghijklmnop", out, sizeof(out));
    assert(redacted);
    assert(strlen(out) < sizeof(out));

    puts("PASS log-redact-test the redacted flag is correct even when output is truncated");
}

static void jw__test_every_small_output_stays_in_bounds(void) {
    unsigned char guarded[20];
    const char *line =
        "prefix=ordinary token=abcdefghijklmnopqrstuvwxyz pwd=secret";

    for (size_t out_size = 1; out_size <= 16; out_size++) {
        memset(guarded, 0xa5, sizeof(guarded));
        char *out = (char *)&guarded[1];

        assert(jw_svc_log_redact_line(line, out, out_size));
        assert(guarded[0] == 0xa5);
        assert(guarded[out_size + 1] == 0xa5);
        assert(out[out_size - 1] == '\0');
    }

    puts("PASS log-redact-test output sizes 1 through 16 retain canaries and terminate");
}

static void jw__test_invalid_arguments(void) {
    char out[16];
    assert(!jw_svc_log_redact_line(NULL, out, sizeof(out)));
    assert(!jw_svc_log_redact_line("token=abc", NULL, sizeof(out)));
    assert(!jw_svc_log_redact_line("token=abc", out, 0));

    puts("PASS log-redact-test NULL line/out or a zero out_size is handled safely");
}

static void jw__test_empty_line_and_bare_delimiters(void) {
    char out[32];
    assert(!jw_svc_log_redact_line("", out, sizeof(out)));
    assert(strcmp(out, "") == 0);

    assert(!jw_svc_log_redact_line("=", out, sizeof(out)));
    assert(strcmp(out, "=") == 0);

    assert(!jw_svc_log_redact_line(":", out, sizeof(out)));
    assert(strcmp(out, ":") == 0);

    assert(jw_svc_log_redact_line("token=", out, sizeof(out)));
    assert(strcmp(out, "token=[REDACTED]") == 0);

    assert(jw_svc_log_redact_line("token:", out, sizeof(out)));
    assert(strcmp(out, "token:[REDACTED]") == 0);

    puts("PASS log-redact-test empty lines, bare delimiters, and empty sensitive values are safe");
}

int main(void) {
    jw__test_line_with_no_secret_passes_through();
    jw__test_logfmt_key_value_is_redacted();
    jw__test_header_style_redacts_to_end_of_line();
    jw__test_cookie_header_hides_embedded_pairs();
    jw__test_quoted_value_is_redacted_whole();
    jw__test_escaped_quote_does_not_end_value();
    jw__test_unterminated_quote_redacts_to_end_of_line();
    jw__test_key_match_is_case_insensitive();
    jw__test_key_spelling_variants_and_sensitive_suffixes();
    jw__test_quoted_and_whitespace_separated_key();
    jw__test_similar_non_secret_keys_pass_through();
    jw__test_auth_credentials_and_schemes_are_redacted();
    jw__test_pin_and_secret_and_apikey();
    jw__test_multiple_sensitive_pairs_all_redacted();
    jw__test_crypt_hash_marker_redacts_whole_line();
    jw__test_shadow_entry_redacts_whole_line();
    jw__test_locked_shadow_and_modern_hash_redact_whole_line();
    jw__test_private_key_marker_redacts_whole_line();
    jw__test_truncation_still_reports_redacted();
    jw__test_every_small_output_stays_in_bounds();
    jw__test_invalid_arguments();
    jw__test_empty_line_and_bare_delimiters();
    puts("PASS log-redact-test");
    return 0;
}
