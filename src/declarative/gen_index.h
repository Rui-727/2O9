/* gen_index.h - hash index for the generation DB
 *
 * xbps uses an immutable in-memory dictionary (mmap'd plist) for
 * package lookups - O(1) by name, no re-parsing. 2O9's generation DB
 * is JSON (human-readable, debuggable) but slow to parse repeatedly.
 *
 * This index is a cache: we parse the JSON manifest once, build a
 * hash table mapping package name -> {version, store_path, origin,
 * generation_id}, and keep it in memory for the lifetime of the
 * process. For CLI tools that do multiple lookups (209 a b c info,
 * 209 a b c search), this turns O(n*k) JSON parses into O(n) + O(k)
 * hash lookups.
 *
 * The JSON manifest.json remains the source of truth. This index is
 * a derived structure - rebuilt on every process start, never written
 * to disk. If we later want to persist it (like xbps's binary plist),
 * we can add a .idx sidecar file invalidated by mtime check.
 */

#ifndef TWO9_GEN_INDEX_H
#define TWO9_GEN_INDEX_H

#include "gen.h"

/* An entry in the hash index */
typedef struct gen_index_entry {
        char *name;           /* package name (hash key) */
        char *version;
        char *store_path;     /* may be NULL */
        char *origin;         /* "repo", "aur", "imperative" */
        int generation_id;    /* which generation this came from */
        struct gen_index_entry *next;  /* hash chain (separate chaining) */
} gen_index_entry_t;

/* The index itself - a hash table with separate chaining */
typedef struct gen_index {
        gen_index_entry_t **buckets;
        size_t bucket_count;
        size_t entry_count;
} gen_index_t;

/* Build an index from the current generation's manifest.
 * Reads manifest.json via cJSON, builds a hash table keyed by package name.
 * Returns NULL on failure (no DB, no current gen, parse error).
 * Caller must free with gen_index_free(). */
gen_index_t *gen_index_load(const char *db_root, int generation_id);

/* Free an index and all its entries */
void gen_index_free(gen_index_t *idx);

/* Look up a package by name. Returns NULL if not found.
 * The returned pointer is owned by the index - don't free it. */
const gen_index_entry_t *gen_index_lookup(const gen_index_t *idx, const char *name);

/* Check if a package is installed (exists in the index).
 * Faster than gen_index_lookup if you don't need the entry. */
int gen_index_contains(const gen_index_t *idx, const char *name);

/* Iterate over all entries. Calls cb for each entry with user_data.
 * Returns the number of entries iterated. */
size_t gen_index_foreach(const gen_index_t *idx,
                         void (*cb)(const gen_index_entry_t *entry, void *ud),
                         void *user_data);

#endif /* TWO9_GEN_INDEX_H */
