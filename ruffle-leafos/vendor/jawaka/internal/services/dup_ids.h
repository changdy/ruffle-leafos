#ifndef JW_SERVICES_DUP_IDS_H
#define JW_SERVICES_DUP_IDS_H

#include <stdbool.h>
#include <stddef.h>

/* contracts.md's SVC-1 field table entry for `service.id`: "Stable,
 * globally unique, reverse-DNS. Duplicates make both unavailable."
 *
 * This module is the pure comparison step across a set of already
 * parsed/validated manifests' service ids: given every discovered
 * pak's service.id, it reports which ones collide with at least one
 * other. "Both" in the contract text is read as the general case -- if
 * the same service.id appears N>=2 times across N different paks,
 * every one of those N paks' services becomes unavailable, not just
 * the second (or later) one encountered. There is no notion of a
 * "first, legitimate" owner of a colliding id.
 *
 * Comparison is byte-for-byte case-sensitive strcmp(). The reverse-DNS
 * grammar enforced by jw_service_manifest_validate() admits only
 * lowercase ASCII letters and digits, so case variants cannot reach
 * this function through its documented validated-input path;
 * internal/services/manifest.c likewise uses strcmp() when checking a
 * pak's top-level `id` against its `service.id`. This function does not
 * normalize invalid or otherwise unvalidated input.
 *
 * This module does no I/O and knows nothing about pak.json, JSON, or
 * the filesystem: callers feed it the service_id strings they already
 * parsed and validated individually (e.g. via
 * jw_service_manifest_validate()), and are responsible for mapping
 * flagged indices back to whichever pak each one came from.
 *
 * Deliberately O(n^2) direct comparison with no heap allocation: the
 * realistic input size is "however many service paks are installed on
 * one device", not a number where quadratic comparison is a genuine
 * cost, and avoiding allocation entirely means there is no
 * out-of-memory case for this function to fail on -- a duplicate-id
 * check that could itself fail open (report "no duplicates" because it
 * ran out of memory partway through) would be worse than one that is
 * merely slow on unrealistic input.
 */

/* Scans `service_ids` (there are `count` of them) for values that
 * occur more than once, and sets the corresponding slot in
 * `is_duplicate` (also `count` elements, caller-allocated) to true for
 * every entry that collides with at least one other entry; every
 * non-colliding entry's slot is set to false. Returns true if any
 * collision was found at all, false otherwise (including when count
 * is 0).
 *
 * A NULL entry within `service_ids` is treated as never matching
 * anything (including another NULL entry) and its own slot is always
 * set to false -- this function does not reject or crash on it, but a
 * caller should not be passing NULL service ids in the first place
 * (jw_service_manifest_validate() never produces one).
 *
 * If `count` is 0, this function returns false immediately and does
 * not touch `service_ids` or `is_duplicate` (they may be NULL in that
 * case). If `count` is nonzero, both `service_ids` and `is_duplicate`
 * must be non-NULL and have at least `count` elements; passing NULL
 * for either while count > 0 returns false without writing anything
 * (there is nowhere safe to write the result). */
bool jw_svc_find_duplicate_ids(const char *const *service_ids, size_t count, bool *is_duplicate);

#endif
