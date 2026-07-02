/* store.h — putting packages in /nix/store/
 *
 * The store adapter takes a .pkg.tar.zst and puts it in the store.
 * That's it. Three things happen:
 *
 *   1. Extract the .pkg.tar.zst into /nix/store/<name>-<version>/
 *   2. Return the store path so the caller can record it
 *   3. Build a manifest of files (so the symlink farm knows what to link)
 *
 * Two backends, picked at runtime:
 *   - NIX_STORE: shells out to `nix-store --add` (the real Nix toolchain)
 *   - DIRECT:    extracts with libarchive directly (no nix dependency,
 *                useful for testing and for systems without nix installed)
 *
 * See DESIGN.md §4 and §6 for the why.
 */

#ifndef TWO9_STORE_H
#define TWO9_STORE_H

#include <stddef.h>

/* A single file entry in a store path */
typedef struct store_entry {
        char *path;       /* relative path within the store (e.g. "bin/nvim") */
        char *symlink;    /* if not NULL, this entry is a symlink to this target */
        int is_dir;       /* 1 if directory entry */
        int is_config;    /* 1 if file should go to /etc/ instead of ~/.local/ */
        struct store_entry *next;
} store_entry_t;

/* A store path with its file manifest */
typedef struct store_manifest {
        char *store_path;   /* e.g. /nix/store/neovim-0.9.5 (no hash, just name-version) */
        char *pkg_name;     /* e.g. neovim */
        char *pkg_version;  /* e.g. 0.9.5 */
        store_entry_t *entries;  /* linked list of files */
        size_t entry_count;
} store_manifest_t;

/* Store backend selection */
typedef enum {
        STORE_BACKEND_NIX_STORE,   /* nix-store --add (default) */
        STORE_BACKEND_DIRECT,      /* libarchive extraction (testing) */
} store_backend_t;

/* Result of adding a package to the store */
typedef struct store_add_result {
        int success;               /* 0 = ok, nonzero = error */
        char *store_path;          /* set on success */
        char *error_msg;           /* set on failure */
} store_add_result_t;

/* Add a .pkg.tar.zst to the store.
 * Returns a result struct. Caller must free with store_add_result_free(). */
store_add_result_t store_add(const char *pkg_path, store_backend_t backend);

/* Free a store_add_result */
void store_add_result_free(store_add_result_t *r);

/* Build a manifest from a store path by reading its file listing.
 * Returns NULL on error. Caller must free with store_manifest_free(). */
store_manifest_t *store_manifest_create(const char *store_path,
                                        const char *pkg_name,
                                        const char *pkg_version);

/* Free a manifest and all its entries */
void store_manifest_free(store_manifest_t *m);

/* Free a linked list of entries */
void store_entry_list_free(store_entry_t *e);

#endif /* TWO9_STORE_H */
