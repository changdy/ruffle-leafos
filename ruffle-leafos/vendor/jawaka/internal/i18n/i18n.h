#ifndef JW_I18N_H
#define JW_I18N_H

#include <stdbool.h>
#include <stddef.h>

/* User-facing string lookup.
 *
 * The English string is the key. `T("Settings")` returns the translation when
 * one is loaded and the literal itself otherwise, so a missing or partial
 * translation degrades to English with no call-site handling. That property is
 * the whole reason the key is the source string rather than an enum: a
 * community translation lands incomplete and stays incomplete for a while.
 *
 * Wrap ONLY user-facing text. Log messages, IPC message types, DB column names,
 * file paths and env var names must stay unwrapped -- they look like UI strings
 * to a regex, which is why the wrapping pass cannot be automated.
 *
 * Identical English needing different translations takes a context prefix that
 * the compiler strips from the displayed text: T("verb|Open") and T("noun|Open")
 * are distinct keys, both rendering "Open" when untranslated.
 *
 * The returned pointer is owned by the table and stays valid until
 * jw_i18n_shutdown(). Do not free it, and do not hold it across a language
 * change -- the launcher relaunches on a language change, so in practice that
 * means do not stash it in a long-lived global.
 */

#define T(s) jw_i18n(s)

/* Marks a UI string that cannot be wrapped in T() where it is written, because
 * it lives in a `static const` initializer and T() is a function call. Expands
 * to the string itself, so codegen is unchanged; it exists purely so the
 * extractor can see the string, and so a reader can tell UI text from a config
 * key or a file path sitting in the same kind of table.
 *
 * The value is still translated at the point it is drawn -- the row renderers
 * call T() on whatever they are handed -- so this marks the string for
 * EXTRACTION, not for lookup. Both halves are needed: an unmarked string never
 * reaches the .po, and a marked string in a renderer that forgets T() renders
 * English no matter how good the translation is.
 *
 *   static const char *const kFontSizeLabels[] = { JW_UI("Small"), ... };
 *
 * This replaces a hand-maintained list of table names in the extractor. That
 * list failed silently: forget to register a table and everything builds,
 * ships, and looks right until a native speaker reads the screen. A missing
 * JW_UI is at least visible in review, next to its marked neighbours.
 *
 * Do NOT mark proper nouns. Font families, theme ids and product names stay
 * English on purpose, and marking them only invites a translator to change
 * something that should not change.
 */
#define JW_UI(s) (s)

/* Load the table for `lang` ("en" or a code like "zh_CN"). "en" (or NULL/empty)
 * loads nothing and every lookup returns its key, which is the no-op default.
 *
 * Sources, in order:
 *   1. $UMRK_INTERNAL_DATA_PATH/i18n/<lang>.tsv  -- a live override, tab
 *      separated "english<TAB>translation" with # comments. This is what a
 *      translator exports from a spreadsheet, so they can iterate on device
 *      without a build. Lines that fail to parse are skipped, not fatal.
 *   2. $UMRK_PLATFORM_PATH/i18n/<lang>.jwi       -- the shipped compiled table.
 *
 * Returns true when a table loaded. Returns false for "en", for a missing
 * table, and for a corrupt one -- all three leave lookups returning their keys,
 * so a caller that ignores the result still behaves correctly.
 */
bool jw_i18n_load(const char *lang);

void jw_i18n_shutdown(void);

/* Never returns NULL for a non-NULL argument; returns `english` on any miss. */
const char *jw_i18n(const char *english);

/* The language actually loaded, "en" when none. Never NULL. */
const char *jw_i18n_language(void);

/* True when `lang` needs the CJK face rather than a themed Latin family. Takes
 * a language code rather than reading global state so callers that resolve a
 * language before loading a table (the fork() path that builds a child's
 * environment) can ask too. NULL, "" and "en" are all false. */
bool jw_i18n_language_is_cjk(const char *lang);

/* Language codes with a table present, newest-first is not meaningful so the
 * order is whatever the directory yields. `out` receives up to `max` pointers
 * into static storage, valid until the next call. Returns the count.
 *
 * The Settings row is hidden when this returns 0: shipping a language picker
 * with nothing to pick reads as broken, and it means a translator can make the
 * row appear simply by dropping their .tsv on the card. */
/* How many translations Leaf will list, English not counted, and the room for one
 * language code. These are the only definitions: the scanner, the Settings list
 * and the tests all size from them, so the two ends cannot disagree about
 * capacity. They did while the numbers were written out by hand in six places --
 * the scanner kept 8, the Settings list had room for 7 after English, and an
 * eighth language simply never appeared. A language past the limit is logged,
 * never dropped silently. */
#define JW_I18N_MAX_LANGUAGES 32
#define JW_I18N_CODE_MAX      16

size_t jw_i18n_available(const char **out, size_t max);

/* Entry count in the loaded table, 0 when none. For diagnostics and the
 * coverage gate; not needed for normal use. */
size_t jw_i18n_count(void);

/* Coverage recording. A missing translation is otherwise silent -- T() returns
 * its key and the screen renders English -- so the only thing that has ever
 * detected one is a native speaker reading the whole device. This makes the gap
 * measurable instead, in any language, and catches keys assembled at runtime
 * that no source-scanning extractor can see.
 *
 * Enabled by JAWAKA_I18N_COVERAGE=1, or by creating
 * $UMRK_INTERNAL_DATA_PATH/i18n/coverage.on -- the marker exists because the UI
 * binaries are spawned by a daemon that init starts, so there is nowhere to
 * export a variable. Read once when a table loads; off, the cost is one
 * predictable branch on a cold global.
 *
 * A key is written once, when first seen. Repeats cost a hash probe and no I/O,
 * which matters because T() sits on the render path and a MISS is the common
 * case there -- values flow through it too, and a game name legitimately falls
 * back.
 *
 * Meaningful only with a table loaded; running in English records nothing.
 */
bool jw_i18n_coverage_enabled(void);

/* Keys are appended to $LOGS_PATH/i18n-coverage-<binary>.txt the moment each is
 * first seen, so the file survives a SIGKILL, a language change and a process
 * that never exits -- all three of which lost a full walkthrough when this
 * wrote once at exit. Nothing needs to be called to save it.
 *
 * Returns how many distinct keys THIS process has recorded. The file may hold
 * more, appended by earlier runs; the consumer sorts and dedups. */
size_t jw_i18n_coverage_count(void);

#endif /* JW_I18N_H */
