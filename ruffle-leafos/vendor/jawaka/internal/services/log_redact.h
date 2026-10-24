#ifndef JW_SERVICES_LOG_REDACT_H
#define JW_SERVICES_LOG_REDACT_H

#include <stdbool.h>
#include <stddef.h>

/* CTL-1's redaction rule (contracts.md#ctl-1--control-and-status-ipc):
 * "Log tails and exports redact API keys, tokens, cookies, PINs,
 * passwords, password hashes, private keys, and shadow entries."
 *
 * jw_svc_log_redact_line() applies that rule to ONE line of log text at
 * a time (Jawaka's log tail/export is inherently line-oriented). It
 * recognizes two kinds of secret:
 *
 *   - A `key: value` or `key=value` pair whose key (case-insensitively,
 *     with delimiter-adjacent whitespace trimmed) names a known-sensitive
 *     field. Unquoted keys use [A-Za-z0-9_-], plus the common two-word
 *     `API key`, `private key`, and `password hash` forms; quoted
 *     structured-data keys may contain whitespace. Separator and camelCase
 *     variants, and compound names ending in a sensitive component (for
 *     example, `authToken`, `csrf_token`, and `five_game_pin_hash`), are
 *     recognized without treating names such as `cookie_count` or
 *     `password_policy` as secret. This covers API keys, tokens (including
 *     `Authorization: Bearer ...`), cookies, PINs, passwords, and labeled
 *     password hashes.
 *     The ambiguous bare field `auth` passes through only for a small set
 *     of exact status values such as `enabled`, `disabled`, and `true`;
 *     other values are treated as credentials.
 *
 *     A `key:` match redacts the rest of the line (the header-style
 *     convention of one value per line); a `key=` match redacts only that
 *     one token or quoted value, then scanning continues for further pairs
 *     later on the same line (the logfmt-style convention of several
 *     `key=value` pairs per line). Existing whitespace after either
 *     delimiter is preserved rather than synthesized.
 *
 *   - A small set of self-identifying markers, redacted by replacing
 *     the entire line (they have no clean "value" boundary to redact
 *     around safely): common modular password-hash prefixes (including
 *     MD5, bcrypt, SHA-crypt, scrypt, yescrypt, and Argon2); a complete
 *     nine-field /etc/shadow record (including locked or empty password
 *     fields); and the literal substring "PRIVATE KEY", covering PEM
 *     private-key markers.
 *
 * What this does NOT cover, deliberately, to stay a bounded, honest
 * primitive rather than a general secret scanner: credentials embedded
 * in a URL's userinfo (user:pass@host), a bearer/JWT-shaped token with
 * no recognizable key at all, an unlabeled password hash with no
 * recognized modular prefix, and the body of a multi-line PEM block
 * between its BEGIN/END markers (only the marker lines themselves, which
 * contain the literal "PRIVATE KEY" substring, are caught). A caller
 * piping arbitrary untrusted multi-line blobs through this needs a
 * different tool.
 */

/* Copies `line` into `out` (bounded by out_size, always NUL-terminated
 * when out_size > 0), with every recognized secret redacted: a matched
 * key/value pair's value becomes the placeholder "[REDACTED]"; a
 * matched marker replaces the entire line with "[REDACTED LINE]".
 *
 * Returns true if any redaction was applied anywhere in `line`, even if
 * the redacted portion ended up past the point where `out` was
 * truncated -- this function always scans the whole input line before
 * returning, so a caller can trust the boolean regardless of `out_size`.
 * Returns false if `line` contained no recognized secret; `out` is then
 * a truncated copy of `line` unchanged.
 *
 * `line` and `out` must not be NULL, must not overlap, and `out_size` must
 * be nonzero, or this returns false without touching `out` (there would be
 * nowhere safe to write a terminator). `line` should be a single line with
 * no embedded newline -- pass one line at a time. */
bool jw_svc_log_redact_line(const char *line, char *out, size_t out_size);

#endif
