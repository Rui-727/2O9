/* gen_index.c - hash index for the generation DB
 *
 * See gen_index.h for design rationale. This is the xbps-inspired
 * immutable in-memory dict pattern: parse JSON once, build a hash
 * table, do O(1) lookups for the rest of the process lifetime.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include "gen_index.h"
#include "cJSON.h"

/* ── Hash function (FNV-1a, same as xbps's proplib uses) ────────── */
static size_t fnv1a_hash(const char *s, size_t bucket_count)
{
    size_t hash = 14695981039346656037ULL;  /* FNV offset basis */
    while (*s) {
        hash ^= (unsigned char)*s++;
        hash *= 1099511628211ULL;  /* FNV prime */
    }
    return hash % bucket_count;
}

/* ── Choose bucket count as next power of 2 >= entry count ──────── */
static size_t next_pow2(size_t n)
{
    size_t p = 16;  /* minimum 16 buckets */
    while (p < n * 2) p <<= 1;  /* load factor ~0.5 */
    return p;
}

/* ── Public API ─────────────────────────────────────────────────── */

gen_index_t *gen_index_load(const char *db_root, int generation_id)
{
    if (!db_root || generation_id <= 0) return NULL;

    char manifest_path[PATH_MAX];
    snprintf(manifest_path, sizeof(manifest_path),
             "%s/generations/%d/manifest.json", db_root, generation_id);

    /* Read the file */
    FILE *f = fopen(manifest_path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(fsize + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t nread = fread(buf, 1, (size_t)fsize, f);
    buf[nread] = '\0';
    fclose(f);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return NULL;

    cJSON *arr = cJSON_GetObjectItem(root, "packages");
    if (!cJSON_IsArray(arr)) {
        cJSON_Delete(root);
        return NULL;
    }

    /* Count packages to size the hash table */
    size_t pkg_count = cJSON_GetArraySize(arr);
    if (pkg_count == 0) {
        cJSON_Delete(root);
        return NULL;
    }

    gen_index_t *idx = calloc(1, sizeof(*idx));
    idx->bucket_count = next_pow2(pkg_count);
    idx->buckets = calloc(idx->bucket_count, sizeof(gen_index_entry_t *));
    idx->entry_count = 0;

    /* Populate */
    cJSON *item;
    cJSON_ArrayForEach(item, arr) {
        cJSON *jname    = cJSON_GetObjectItem(item, "name");
        cJSON *jversion = cJSON_GetObjectItem(item, "version");
        cJSON *jstore   = cJSON_GetObjectItem(item, "store_path");
        cJSON *jorigin  = cJSON_GetObjectItem(item, "origin");
        if (!cJSON_IsString(jname)) continue;

        gen_index_entry_t *e = calloc(1, sizeof(*e));
        e->name = strdup(jname->valuestring);
        e->version = strdup(cJSON_IsString(jversion) ? jversion->valuestring : "unknown");
        e->store_path = cJSON_IsString(jstore) ? strdup(jstore->valuestring) : NULL;
        e->origin = strdup(cJSON_IsString(jorigin) ? jorigin->valuestring : "repo");
        e->generation_id = generation_id;

        size_t bucket = fnv1a_hash(e->name, idx->bucket_count);
        e->next = idx->buckets[bucket];
        idx->buckets[bucket] = e;
        idx->entry_count++;
    }

    cJSON_Delete(root);
    return idx;
}

void gen_index_free(gen_index_t *idx)
{
    if (!idx) return;
    for (size_t i = 0; i < idx->bucket_count; i++) {
        gen_index_entry_t *e = idx->buckets[i];
        while (e) {
            gen_index_entry_t *next = e->next;
            free(e->name);
            free(e->version);
            free(e->store_path);
            free(e->origin);
            free(e);
            e = next;
        }
    }
    free(idx->buckets);
    free(idx);
}

const gen_index_entry_t *gen_index_lookup(const gen_index_t *idx, const char *name)
{
    if (!idx || !name) return NULL;
    size_t bucket = fnv1a_hash(name, idx->bucket_count);
    for (gen_index_entry_t *e = idx->buckets[bucket]; e; e = e->next) {
        if (strcmp(e->name, name) == 0)
            return e;
    }
    return NULL;
}

int gen_index_contains(const gen_index_t *idx, const char *name)
{
    return gen_index_lookup(idx, name) != NULL;
}

size_t gen_index_foreach(const gen_index_t *idx,
                         void (*cb)(const gen_index_entry_t *entry, void *ud),
                         void *user_data)
{
    if (!idx || !cb) return 0;
    size_t count = 0;
    for (size_t i = 0; i < idx->bucket_count; i++) {
        for (gen_index_entry_t *e = idx->buckets[i]; e; e = e->next) {
            cb(e, user_data);
            count++;
        }
    }
    return count;
}
