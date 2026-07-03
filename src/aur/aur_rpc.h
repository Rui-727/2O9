/* aur_rpc.h - AUR RPC client
 *
 * Rewrites paru's raur crate in C. Provides:
 *  - AUR search (/rpc/v5/?type=search)
 *  - AUR info   (/rpc/v5/?type=info)
 *  - Batch info queries with caching
 *
 * Uses libcurl for HTTP, cJSON for JSON parsing.
 * See: https://aur.archlinux.org/rpc
 */

#ifndef TWO9_AUR_RPC_H
#define TWO9_AUR_RPC_H

#include <stddef.h>
#include <time.h>

/* AUR package info - mirrors raur::Package */
typedef struct aur_pkg {
	char *name;
	char *pkgbase;
	char *version;
	char *description;
	char *url;
	char *url_path;        /* /cgit/aur.git/plain/PKGBUILD?h=<base> */
	int num_votes;
	int popularity;
	time_t submitted;
	time_t modified;
	char **depends;
	size_t depends_count;
	char **makedepends;
	size_t makedepends_count;
	char **checkdepends;
	size_t checkdepends_count;
	char **optdepends;
	size_t optdepends_count;
	char **conflicts;
	size_t conflicts_count;
	char **provides;
	size_t provides_count;
	char **groups;
	size_t groups_count;
	char **licenses;
	size_t licenses_count;
	char **keywords;
	size_t keywords_count;
	int out_of_date;       /* 1 if flagged, 0 if OK */
	char *maintainer;
	int first_submitted;
	int last_modified;
	char **replaces;
	size_t replaces_count;
	struct aur_pkg *next;  /* linked list */
} aur_pkg_t;

/* AUR RPC response */
typedef struct aur_rpc_result {
	int success;
	char *error;           /* error message if !success */
	aur_pkg_t *packages;   /* linked list of results */
	size_t count;
} aur_rpc_result_t;

/* AUR RPC cache - avoids repeated queries */
typedef struct aur_cache {
	char *base_url;        /* e.g. "https://aur.archlinux.org" */
	void *curl_handle;     /* reused curl handle */
	/* TODO: hash map for cached results */
} aur_cache_t;

/* Open/close an AUR RPC cache */
aur_cache_t *aur_cache_open(const char *base_url);
void aur_cache_close(aur_cache_t *cache);

/* Search AUR packages by name/description */
aur_rpc_result_t aur_search(aur_cache_t *cache, const char *query,
                            const char *by_field);

/* Get info for a single package */
aur_rpc_result_t aur_info(aur_cache_t *cache, const char *pkg_name);

/* Batch info query - takes array of names, returns all matches.
 * This is the workhorse - used by install, upgrade, query. */
aur_rpc_result_t aur_info_batch(aur_cache_t *cache,
                                const char **names, size_t count);

/* Free a result and all its packages */
void aur_rpc_result_free(aur_rpc_result_t *r);

/* Free a single package */
void aur_pkg_free(aur_pkg_t *pkg);

#endif /* TWO9_AUR_RPC_H */
