#ifndef JW_DISCOVERY_CONTENT_H
#define JW_DISCOVERY_CONTENT_H

#include "internal/storage/sources.h"
#include "internal/retroarch/catalog.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char source_id[JW_STORAGE_SOURCE_ID_MAX];
    /* Heap strings owned by the content; jw_content_free releases them. */
    char *rom_relpath;
    char *path;
    uint64_t size;
    bool missing;
} jw_content_file;

typedef struct {
    size_t parent;
    size_t child;
} jw_content_reference;

typedef struct {
    jw_content_file *files;
    size_t file_count;
    /* files[launch_file] is the selected launch file; files are deduplicated. */
    size_t launch_file;
    jw_content_reference *references;
    size_t reference_count;
} jw_content;

/* Read-only inspection of one game: its launch file and the files its M3U,
   CUE, GDI, TOC or CMD descriptors name, followed through nested descriptors.
   Missing referenced files retain their normalized keys; unreadable/malformed
   descriptors and unsafe paths fail with a concrete error. The catalog system
   is needed for CMD file-argument recognition. */
int jw_content_inspect(const jw_storage_source_list *sources,
                       const char *source_id, const char *rom_relpath,
                       const jw_ra_system *system, jw_content *out,
                       char *error, size_t error_size);
bool jw_content_is_descriptor_path(const char *path);
void jw_content_free(jw_content *content);

#endif
